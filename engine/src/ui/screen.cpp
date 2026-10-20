#include "ui/screen.h"

#include "core/path.h"
#include "render/sprite.h"
#include "res/bigfile.h"
#include "res/bmp.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <string>
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

int attr_int(const SmlMarkupTag& tag, std::string_view name, int fallback) {
    const std::string* value = sml_attribute(tag, name);
    if (value == nullptr) {
        return fallback;
    }
    const char* begin = value->c_str();
    char* end = nullptr;
    const long parsed = std::strtol(begin, &end, 10);
    if (end == begin) {
        return fallback;
    }
    if (parsed < std::numeric_limits<int>::min()) {
        return std::numeric_limits<int>::min();
    }
    if (parsed > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(parsed);
}

std::uint64_t fnv1a64(std::span<const std::uint8_t> data) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const std::uint8_t byte : data) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out,
               std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "screen: cannot read " + path.string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        error = "screen: read error on " + path.string();
        return false;
    }
    return true;
}

bool image_name_safe(std::string_view name) {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    if (name.find("..") != std::string_view::npos ||
        name.find('\\') != std::string_view::npos ||
        name.find(':') != std::string_view::npos || name.front() == '/') {
        return false;
    }
    return true;
}

void set_pixel(RenderResult& target, int x, int y, const std::uint8_t (&color)[3]) {
    if (x < 0 || y < 0 || x >= target.width || y >= target.height) {
        return;
    }
    const std::size_t at = (static_cast<std::size_t>(y) * target.width +
                            static_cast<std::size_t>(x)) *
                           3u;
    target.rgb[at] = color[0];
    target.rgb[at + 1] = color[1];
    target.rgb[at + 2] = color[2];
}

} // namespace

// ------------------------------------------------------------------ load --

std::optional<Screen> Screen::load(const std::filesystem::path& screen,
                                   const std::filesystem::path& game,
                                   std::string& error) {
    error.clear();
    std::error_code ec;
    std::filesystem::path resolved;
    if (std::filesystem::is_regular_file(screen, ec)) {
        resolved = screen;
    } else if (!screen.is_absolute()) {
        if (const auto hit = resolve_ci(game, screen.string())) {
            resolved = *hit;
        } else if (const auto hit = resolve_ci(game / "SML", screen.string())) {
            resolved = *hit;
        }
    }
    if (resolved.empty()) {
        error = "screen: file not found: " + screen.string();
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes;
    if (!read_file(resolved, bytes, error)) {
        return std::nullopt;
    }
    return load(bytes, resolved.string(), game, error);
}

std::optional<Screen> Screen::load(std::span<const std::uint8_t> bytes,
                                   const std::filesystem::path& game,
                                   std::string& error) {
    return load(bytes, {}, game, error);
}

std::optional<Screen> Screen::load(std::span<const std::uint8_t> bytes,
                                   std::string_view name,
                                   const std::filesystem::path& game,
                                   std::string& error) {
    Screen screen;
    if (!screen.load_bytes(bytes, name, game, error)) {
        return std::nullopt;
    }
    return screen;
}

bool Screen::load_bytes(std::span<const std::uint8_t> bytes, std::string_view name,
                        const std::filesystem::path& game, std::string& error) {
    error.clear();
    game_ = game;
    std::string parse_error;
    auto document = parse_sml(bytes, parse_error);
    if (!document) {
        error = "screen: " + parse_error;
        return false;
    }
    document_ = std::move(*document);
    state_ = ScreenState{};
    state_.loaded = true;
    state_.source = name.empty() ? std::string("<bytes>") : std::string(name);
    state_.tags = document_.tags.size();
    state_.elements = document_.elements.size();
    return true;
}

// ----------------------------------------------------------------- layout --

bool Screen::load_fonts(std::string& error) {
    fonts_.clear();
    // Only the default font and the numbers the document actually selects: the
    // rest of the 12 engine atlases are irrelevant for this screen (and each is
    // a BMP read from the installation).
    std::set<int> wanted{kDefaultFont};
    for (const SmlMarkupTag& tag : document_.tags) {
        if (lowered(tag.name) != "f" || tag.closing ||
            sml_attribute(tag, "s") == nullptr) {
            continue;
        }
        wanted.insert(attr_int(tag, "s", kDefaultFont));
    }
    for (const int number : wanted) {
        if (number < 0 || number >= kFontNumbers || font_spec(number) == nullptr) {
            continue; // the layout reports "no atlas for font N" if it is used
        }
        std::string font_error;
        auto atlas = load_font(game_, number, font_error);
        if (atlas) {
            fonts_.emplace(number, std::move(*atlas));
        }
    }
    if (fonts_.find(kDefaultFont) == fonts_.end()) {
        error = "screen: font " + std::to_string(kDefaultFont) +
                " is unavailable under " + game_.string();
        return false;
    }
    return true;
}

bool Screen::load_image(const std::string& name, std::string& error) {
    const std::string key = lowered(name);
    if (images_.find(key) != images_.end()) {
        return true;
    }
    if (!image_name_safe(name)) {
        error = "screen: unsafe <I> image name \"" + name + "\"";
        return false;
    }

    ScreenImageAsset asset;
    asset.name = name;
    std::vector<std::uint8_t> blob;

    // Loose illustrations next to the markup (all 15 shipped names).
    const std::string with_ext = "SML/" + name + ".bmp";
    const std::string plain = "SML/" + name;
    std::filesystem::path loose;
    if (const auto hit = resolve_ci(game_, with_ext)) {
        loose = *hit;
    } else if (const auto hit = resolve_ci(game_, plain)) {
        loose = *hit;
    }
    if (!loose.empty()) {
        if (!read_file(loose, blob, error)) {
            return false;
        }
        asset.source = loose.string();
    } else {
        // Fallback: a regular BMP inside FRONTEND/BMP.IDX (BIGFILE archive).
        std::filesystem::path index = game_ / "FRONTEND" / "BMP.IDX";
        std::error_code ec;
        if (!std::filesystem::exists(index, ec)) {
            if (const auto hit = resolve_ci(game_, "FRONTEND/BMP.IDX")) {
                index = *hit;
            }
        }
        std::string archive_error;
        auto archive = BigFile::open(index, archive_error);
        if (!archive) {
            error = "screen: image \"" + name + "\": " + archive_error;
            return false;
        }
        const std::string wanted = lowered(name);
        const BigEntry* entry = nullptr;
        for (const BigEntry& candidate : archive->entries()) {
            const std::string candidate_name = lowered(candidate.name);
            if (candidate_name == wanted || candidate_name == wanted + ".bmp") {
                entry = &candidate;
                break;
            }
        }
        if (entry == nullptr) {
            error = "screen: image \"" + name + "\" not found in SML/ or FRONTEND/BMP.IDX";
            return false;
        }
        std::string read_error;
        auto bytes = archive->read(*entry, read_error);
        if (!bytes) {
            error = "screen: image \"" + name + "\": " + read_error;
            return false;
        }
        blob = std::move(*bytes);
        asset.source = "FRONTEND/BMP.IDX:" + entry->name;
    }

    std::string bmp_error;
    auto image = parse_bmp(blob, bmp_error);
    if (!image) {
        error = "screen: image \"" + name + "\": " + bmp_error;
        return false;
    }
    asset.width = image->width;
    asset.height = image->height;
    asset.pixels = std::move(image->pixels);
    asset.palette = std::move(image->palette);
    // res/sprite requires a complete 256-entry table; BMPs with a shorter
    // colour table (none in the shipment) get zero-filled spare entries.
    if (asset.palette.size() < kScreenPaletteBytes) {
        asset.palette.resize(kScreenPaletteBytes, 0);
    }
    images_.emplace(key, std::move(asset));
    return true;
}

bool Screen::load_images(std::string& error) {
    images_.clear();
    slots_.clear();
    for (const LayoutPage& page : layout_result_.pages) {
        for (const LayoutImage& image : page.images) {
            if (image.name.empty()) {
                error = "screen: <I> without f= on page " + std::to_string(page.index);
                return false;
            }
            if (!load_image(image.name, error)) {
                return false;
            }

            int scale = 1;
            for (const SmlMarkupTag& tag : document_.tags) {
                if (tag.pos != image.pos || lowered(tag.name) != "i") {
                    continue;
                }
                scale = attr_int(tag, "s", attr_int(tag, "scale", 1));
                break;
            }
            scale = std::clamp(scale, 1, kSpriteMaxScale);

            ScreenImageSlot slot;
            slot.page = page.index;
            slot.name = image.name;
            slot.x = image.x;
            slot.y = image.y;
            slot.width = image.width;
            slot.height = image.height;
            slot.scale = scale;
            slots_.push_back(std::move(slot));
        }
    }
    return true;
}

bool Screen::layout(int page_width, int page_height) {
    state_.error.clear();
    if (!state_.loaded) {
        state_.error = "screen: load() has not run";
        return false;
    }
    state_.laid_out = false;
    lines_.clear();
    page_runs_.clear();
    slots_.clear();
    page_width_ = page_width;
    page_height_ = page_height;

    std::string font_error;
    if (fonts_.empty() && !load_fonts(font_error)) {
        state_.error = font_error;
        state_.pages = 0;
        state_.lines = 0;
        state_.images = 0;
        return false;
    }

    const FontLookup lookup = [this](int number) -> const FontAtlas* {
        const auto it = fonts_.find(number);
        return it == fonts_.end() ? nullptr : &it->second;
    };
    LayoutOptions options;
    options.width = page_width;
    options.height = page_height;
    std::string layout_error;
    layout_result_ = layout_text(document_, lookup, kDefaultFont, options, &layout_error);
    if (!layout_error.empty()) {
        state_.error = "screen: " + layout_error;
        layout_result_ = LayoutResult{};
        state_.pages = 0;
        state_.lines = 0;
        state_.images = 0;
        return false;
    }

    std::string image_error;
    if (!load_images(image_error)) {
        state_.error = image_error;
        layout_result_ = LayoutResult{};
        slots_.clear();
        state_.pages = 0;
        state_.lines = 0;
        state_.images = 0;
        return false;
    }

    page_runs_.assign(layout_result_.pages.size(), {});
    for (const LayoutRun& run : layout_result_.runs) {
        for (std::size_t page = 0; page < layout_result_.pages.size(); ++page) {
            const LayoutPage& candidate = layout_result_.pages[page];
            if (run.first >= candidate.start &&
                (candidate.end == 0 || run.first < candidate.end)) {
                page_runs_[page].push_back(&run);
                break;
            }
        }
    }
    build_lines();

    state_.laid_out = true;
    state_.pages = layout_result_.pages.size();
    state_.lines = lines_.size();
    state_.images = slots_.size();
    clear_navigation();
    return true;
}

void Screen::build_lines() {
    lines_.clear();
    for (const LayoutPage& page : layout_result_.pages) {
        const std::vector<const LayoutRun*>& runs =
            page_runs_[static_cast<std::size_t>(page.index)];
        for (std::size_t index = 0; index < page.lines.size(); ++index) {
            const LayoutLine& line = page.lines[index];
            ScreenLine out;
            out.page = page.index;
            out.index = static_cast<int>(index);
            out.x = line.x;
            out.y = line.y;
            out.width = line.width;
            out.height = line.height;
            out.font = line.font;
            out.first = line.first;
            out.last = line.last;
            for (const LayoutRun* run : runs) {
                if (run->y != line.y) {
                    continue;
                }
                out.text += run->text;
            }
            lines_.push_back(std::move(out));
        }
    }
}

void Screen::clear_navigation() {
    state_.page = 0;
    state_.rendered_page = -1;
    state_.has_selection = false;
    state_.selected = 0;
    state_.drawn_lines = 0;
    state_.drawn_images = 0;
    state_.rgb_hash = 0;
}

// ------------------------------------------------------------- navigation --

bool Screen::select(std::size_t index) {
    state_.error.clear();
    if (!state_.laid_out) {
        state_.error = "screen: layout() has not run";
        return false;
    }
    if (index >= lines_.size()) {
        state_.error = "screen: selection " + std::to_string(index) + " out of range (" +
                       std::to_string(lines_.size()) + " lines)";
        return false;
    }
    state_.has_selection = true;
    state_.selected = index;
    state_.page = lines_[index].page;
    return true;
}

bool Screen::scroll(int delta) {
    state_.error.clear();
    if (!state_.laid_out) {
        state_.error = "screen: layout() has not run";
        return false;
    }
    if (lines_.empty()) {
        if (delta == 0) {
            return true;
        }
        state_.error = "screen: no lines to scroll";
        return false;
    }
    // With no selection, +1 lands on the first line and -1 stays there.
    const long long base = state_.has_selection
                               ? static_cast<long long>(state_.selected)
                               : (delta > 0 ? -1ll : 0ll);
    long long target = base + static_cast<long long>(delta);
    target = std::clamp(target, 0ll, static_cast<long long>(lines_.size()) - 1ll);
    return select(static_cast<std::size_t>(target));
}

bool Screen::show_page(int page) {
    state_.error.clear();
    if (!state_.laid_out) {
        state_.error = "screen: layout() has not run";
        return false;
    }
    if (page < 0 || page >= static_cast<int>(layout_result_.pages.size())) {
        state_.error = "screen: page " + std::to_string(page) + " out of range (0.." +
                       std::to_string(layout_result_.pages.size()) + ")";
        return false;
    }
    state_.page = page;
    return true;
}

// ---------------------------------------------------------------- render --

bool Screen::render(RenderResult& target, std::span<const std::uint8_t> palette,
                    std::string& error) {
    return render(target, palette, state_.page, error);
}

bool Screen::render(RenderResult& target, std::span<const std::uint8_t> palette, int page,
                    std::string& error) {
    error.clear();
    state_.error.clear();
    if (!state_.laid_out) {
        error = "screen: layout() has not run";
        state_.error = error;
        return false;
    }
    if (target.width <= 0 || target.height <= 0) {
        error = "screen: target has no pixels (" + std::to_string(target.width) + "x" +
                std::to_string(target.height) + ")";
        state_.error = error;
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(target.width) *
                                 static_cast<std::size_t>(target.height) * 3u;
    if (target.rgb.size() != expected) {
        error = "screen: target RGB is " + std::to_string(target.rgb.size()) +
                " bytes, expected " + std::to_string(expected);
        state_.error = error;
        return false;
    }
    if (target.width < page_width_ || target.height < page_height_) {
        error = "screen: target " + std::to_string(target.width) + "x" +
                std::to_string(target.height) + " is smaller than the page " +
                std::to_string(page_width_) + "x" + std::to_string(page_height_);
        state_.error = error;
        return false;
    }
    if (page < 0 || page >= static_cast<int>(layout_result_.pages.size())) {
        error = "screen: page " + std::to_string(page) + " out of range (0.." +
                std::to_string(layout_result_.pages.size()) + ")";
        state_.error = error;
        return false;
    }
    if (!palette.empty() && palette.size() < kScreenPaletteBytes) {
        error = "screen: palette has " + std::to_string(palette.size() / 4) +
                " entries, need 256 (or an empty span)";
        state_.error = error;
        return false;
    }

    // Images first: they are the page's background layer.  The shipped float
    // slots are filled by text on later rows; text that follows <I> on the same
    // row keeps text_layout's pen position and can overlap the bitmap
    // (docs/SCREEN_CPP.md §6).
    std::size_t drawn_images = 0;
    for (const ScreenImageSlot& slot : slots_) {
        if (slot.page != page) {
            continue;
        }
        const auto it = images_.find(lowered(slot.name));
        if (it == images_.end()) {
            error = "screen: image \"" + slot.name + "\" was not loaded";
            state_.error = error;
            return false;
        }
        const ScreenImageAsset& asset = it->second;
        Sprite sprite;
        sprite.width = asset.width;
        sprite.height = asset.height;
        sprite.pixels = asset.pixels;
        const std::span<const std::uint8_t> colors =
            palette.empty()
                ? std::span<const std::uint8_t>(asset.palette.data(), asset.palette.size())
                : palette;
        SpriteTarget surface;
        surface.width = target.width;
        surface.height = target.height;
        surface.stride = target.width * 3;
        surface.pixels = std::span<std::uint8_t>(target.rgb.data(), target.rgb.size());
        SpriteBlit params;
        params.x = slot.x;
        params.y = slot.y;
        params.scale = slot.scale;
        params.transparent_index = -1;  // illustrations are opaque
        std::string blit_error;
        if (!blit(surface, colors, sprite, params, blit_error)) {
            error = "screen: image \"" + slot.name + "\": " + blit_error;
            state_.error = error;
            return false;
        }
        ++drawn_images;
    }

    const FontLookup lookup = [this](int number) -> const FontAtlas* {
        const auto it = fonts_.find(number);
        return it == fonts_.end() ? nullptr : &it->second;
    };
    LayoutResult page_layout;
    page_layout.width = page_width_;
    page_layout.height = page_height_;
    page_layout.line_height = layout_result_.line_height;
    page_layout.font = layout_result_.font;
    const std::vector<const LayoutRun*>& runs = page_runs_[static_cast<std::size_t>(page)];
    page_layout.runs.reserve(runs.size());
    for (const LayoutRun* run : runs) {
        page_layout.runs.push_back(*run);
    }
    std::string text_error;
    if (!draw_text(target, page_layout, lookup, 0, 0, text_error)) {
        error = "screen: " + text_error;
        state_.error = error;
        return false;
    }

    // Selection frame (UI-layer policy; drawn after the text so it stays visible).
    std::size_t drawn_lines = 0;
    if (state_.has_selection && state_.selected < lines_.size()) {
        const ScreenLine& line = lines_[state_.selected];
        if (line.page == page && line.width > 0 && line.height > 0) {
            const int left = std::max(0, line.x - 1);
            const int right = std::min(page_width_ - 1, line.x + line.width);
            const int top = std::max(0, line.y - 1);
            const int bottom = std::min(page_height_ - 1, line.y + line.height - 1);
            for (int x = left; x <= right; ++x) {
                set_pixel(target, x, top, kScreenSelectionColor);
                set_pixel(target, x, bottom, kScreenSelectionColor);
            }
            for (int y = top; y <= bottom; ++y) {
                set_pixel(target, left, y, kScreenSelectionColor);
                set_pixel(target, right, y, kScreenSelectionColor);
            }
            ++drawn_lines;
        }
    }

    state_.rendered_page = page;
    state_.drawn_images = drawn_images;
    state_.drawn_lines = drawn_lines;
    state_.rgb_hash = fnv1a64(std::span<const std::uint8_t>(target.rgb.data(), target.rgb.size()));
    return true;
}

} // namespace gm
