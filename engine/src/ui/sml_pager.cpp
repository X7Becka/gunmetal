#include "ui/sml_pager.h"

#include "core/path.h"

#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace gm {
namespace {

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out,
               std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "sml_pager: cannot read " + path.string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        error = "sml_pager: read error on " + path.string();
        return false;
    }
    return true;
}

} // namespace

SmlPager::SmlPager(std::filesystem::path game, SmlPagerOptions options)
    : game_(std::move(game)), options_(options) {}

SmlPager::SmlPager(FontLookup fonts, SmlPagerOptions options)
    : options_(options), external_fonts_(std::move(fonts)) {}

// ------------------------------------------------------------------ fonts --

FontLookup SmlPager::make_lookup() {
    return [this](int number) -> const FontAtlas* {
        const auto it = owned_fonts_.find(number);
        if (it != owned_fonts_.end()) {
            return &it->second;
        }
        return external_fonts_ ? external_fonts_(number) : nullptr;
    };
}

bool SmlPager::ensure_fonts(std::string& error) {
    if (external_fonts_) {
        // The caller owns the atlases; `layout_text` asks for the ones the
        // document actually uses and reports a missing font itself.
        return true;
    }
    if (game_.empty()) {
        error = "sml_pager: no font source (pass a game directory or a FontLookup)";
        return false;
    }

    // Only the logical fonts the document selects, plus the default.  This is
    // the same policy as `Screen::load_fonts` (docs/SCREEN_CPP.md): the other
    // atlases of the 12-slot engine table are irrelevant here.
    std::set<int> wanted{options_.default_font};
    for (const SmlMarkupTag& tag : document_.tags) {
        if (lowered(tag.name) != "f" || tag.closing ||
            sml_attribute(tag, "s") == nullptr) {
            continue;
        }
        const std::string* value = sml_attribute(tag, "s");
        char* end = nullptr;
        const long number = std::strtol(value->c_str(), &end, 10);
        if (end != value->c_str() &&
            number >= std::numeric_limits<int>::min() &&
            number <= std::numeric_limits<int>::max()) {
            wanted.insert(static_cast<int>(number));
        }
    }

    for (const int number : wanted) {
        if (number < 0 || number >= kFontNumbers || font_spec(number) == nullptr) {
            continue; // the layout reports "no atlas for font N" when it is used
        }
        if (owned_fonts_.find(number) != owned_fonts_.end()) {
            continue;
        }
        std::string font_error;
        auto atlas = load_font(game_, number, font_error);
        if (atlas) {
            owned_fonts_.emplace(number, std::move(*atlas));
        }
    }
    if (owned_fonts_.find(options_.default_font) == owned_fonts_.end()) {
        auto base = load_font(game_, options_.default_font, error);
        if (!base) {
            if (error.empty()) {
                error = "sml_pager: default font " +
                        std::to_string(options_.default_font) + " is unavailable";
            }
            return false;
        }
        owned_fonts_.emplace(options_.default_font, std::move(*base));
    }
    error.clear();
    return true;
}

// --------------------------------------------------------------- document --

void SmlPager::close() {
    document_ = SmlDocument{};
    layout_ = LayoutResult{};
    page_first_line_.clear();
    line_count_ = 0;
    page_ = 0;
    line_ = 0;
    opened_ = false;
    error_.clear();
}

void SmlPager::reset() { close(); }

void SmlPager::fail(std::string message, std::string* error) {
    close();
    error_ = std::move(message);
    if (error != nullptr) {
        *error = error_;
    }
}

bool SmlPager::open(const SmlDocument& document, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    SmlDocument copy = document;
    return open_parsed(std::move(copy), error);
}

bool SmlPager::open(const std::filesystem::path& path, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    std::error_code ec;
    std::filesystem::path resolved;
    if (std::filesystem::is_regular_file(path, ec)) {
        resolved = path;
    } else if (!path.is_absolute() && !game_.empty()) {
        if (const auto hit = resolve_ci(game_, path.string())) {
            resolved = *hit;
        } else if (const auto hit = resolve_ci(game_ / "SML", path.string())) {
            resolved = *hit;
        }
    }
    if (resolved.empty()) {
        fail("sml_pager: file not found: " + path.string(), error);
        return false;
    }

    std::vector<std::uint8_t> bytes;
    std::string read_error;
    if (!read_file(resolved, bytes, read_error)) {
        fail(read_error, error);
        return false;
    }
    std::string parse_error;
    auto document = parse_sml(bytes, parse_error);
    if (!document) {
        fail("sml_pager: " + parse_error, error);
        return false;
    }
    return open_parsed(std::move(*document), error);
}

bool SmlPager::open_parsed(SmlDocument document, std::string* error) {
    close();
    document_ = std::move(document);
    if (document_.text.empty()) {
        fail("sml_pager: empty document", error);
        return false;
    }

    std::string font_error;
    if (!ensure_fonts(font_error)) {
        fail(font_error, error);
        return false;
    }

    LayoutOptions layout_options;
    layout_options.width = options_.width;
    layout_options.height = options_.height;
    layout_options.line_height = options_.line_height;
    layout_options.big_line_height = options_.big_line_height;
    layout_options.max_pages = options_.max_pages;
    layout_options.image_width = options_.image_width;
    layout_options.image_height = options_.image_height;
    layout_options.wrap = options_.wrap;

    std::string layout_error;
    layout_ = layout_text(document_, make_lookup(), options_.default_font,
                          layout_options, &layout_error);
    if (!layout_error.empty() || layout_.pages.empty()) {
        const std::string message =
            layout_error.empty() ? "sml_pager: layout produced no pages" : layout_error;
        fail(message, error);
        return false;
    }

    build_line_index();
    opened_ = true;
    page_ = 0;
    line_ = 0;
    error_.clear();
    return true;
}

void SmlPager::build_line_index() {
    line_count_ = 0;
    page_first_line_.assign(layout_.pages.size() + 1, 0);
    for (std::size_t i = 0; i < layout_.pages.size(); ++i) {
        page_first_line_[i] = line_count_;
        line_count_ += static_cast<int>(layout_.pages[i].lines.size());
    }
    page_first_line_.back() = line_count_;
}

// ------------------------------------------------------------------ pages --

bool SmlPager::next_page() {
    if (!opened_ || page_ + 1 >= page_count()) {
        return false;
    }
    page_ = page_ + 1;
    line_ = page_first_line_[static_cast<std::size_t>(page_)];
    return true;
}

bool SmlPager::prev_page() {
    if (!opened_ || page_ <= 0 || page_ >= page_count()) {
        return false;
    }
    page_ = page_ - 1;
    line_ = page_first_line_[static_cast<std::size_t>(page_)];
    return true;
}

bool SmlPager::first_page() {
    if (!opened_ || page_count() == 0 || page_ == 0) {
        return false;
    }
    page_ = 0;
    line_ = 0;
    return true;
}

bool SmlPager::last_page() {
    if (!opened_ || page_count() == 0 || page_ == page_count() - 1) {
        return false;
    }
    page_ = page_count() - 1;
    line_ = page_first_line_[static_cast<std::size_t>(page_)];
    return true;
}

bool SmlPager::show_page(int page) {
    if (!opened_ || page < 0 || page >= page_count() || page == page_) {
        return false;
    }
    page_ = page;
    line_ = page_first_line_[static_cast<std::size_t>(page_)];
    return true;
}

const std::vector<LayoutLine>& SmlPager::lines() const {
    static const std::vector<LayoutLine> empty;
    if (!opened_ || page_ < 0 || page_ >= page_count()) {
        return empty;
    }
    return layout_.pages[static_cast<std::size_t>(page_)].lines;
}

const std::vector<LayoutImage>& SmlPager::images() const {
    static const std::vector<LayoutImage> empty;
    if (!opened_ || page_ < 0 || page_ >= page_count()) {
        return empty;
    }
    return layout_.pages[static_cast<std::size_t>(page_)].images;
}

const LayoutPage* SmlPager::current_page() const {
    if (!opened_ || page_ < 0 || page_ >= page_count()) {
        return nullptr;
    }
    return &layout_.pages[static_cast<std::size_t>(page_)];
}

// ------------------------------------------------------------------ lines --

bool SmlPager::scroll_next_line() {
    if (!opened_ || line_count_ <= 0 || line_ + 1 >= line_count_) {
        return false;
    }
    ++line_;
    // Pages without lines collapse to the same prefix; take the page whose
    // [first, first + lines) range contains the cursor.
    for (int page = page_count() - 1; page >= 0; --page) {
        const int first = page_first_line_[static_cast<std::size_t>(page)];
        const int next = page_first_line_[static_cast<std::size_t>(page) + 1];
        if (line_ >= first && line_ < next) {
            page_ = page;
            break;
        }
    }
    return true;
}

bool SmlPager::scroll_prev_line() {
    if (!opened_ || line_count_ <= 0 || line_ <= 0) {
        return false;
    }
    --line_;
    for (int page = page_count() - 1; page >= 0; --page) {
        const int first = page_first_line_[static_cast<std::size_t>(page)];
        const int next = page_first_line_[static_cast<std::size_t>(page) + 1];
        if (line_ >= first && line_ < next) {
            page_ = page;
            break;
        }
    }
    return true;
}

// ----------------------------------------------------------------- status --

std::string SmlPager::status() const {
    if (!opened_ || layout_.pages.empty()) {
        return "0/0";
    }
    return "page " + std::to_string(page_ + 1) + "/" +
           std::to_string(layout_.pages.size());
}

} // namespace gm
