#include "render/text_layout.h"

#include "core/path.h"
#include "res/bigfile.h"
#include "res/bmp.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace gm {
namespace {

// ------------------------------------------------------------------ table --
//
// The twelve records below are the literal arguments of the ``0x47fe30``
// calls in the font loader (``0x490145`` .. ``0x49054f``).  The argument order
// used here is (number, atlas, cell_w, cell_h, first, count, columns,
// column_pitch, row_pitch, origin_x, origin_y, space, extra_before,
// extra_after, ink_trim); the callee stores them at ``0x65ddd0``:
//
//   +0x00 width, +0x04 height, +0x08 first, +0x0c last, +0x10 extra_before,
//   +0x14 extra_after, +0x18 glyph array (1 advance byte + w*h pixels each).
//
// Fonts 6 and 13..30 are never initialised by the loader.
constexpr FontSpec kSpecs[] = {
    // FONTS4, three body weights plus a highlight copy (y = 0/42/84/126).
    {0, "FONTS4", 8, 8, 32, 96, 32, 16, 14, 8, 0, 3, 0, 1, true},
    // FONTS1 holds two small faces (y = 0 and y = 96).
    {1, "FONTS1", 8, 8, 32, 96, 32, 16, 16, 8, 0, 4, 0, 1, true},
    {2, "FONTS1", 8, 8, 32, 96, 32, 16, 16, 8, 96, 4, 0, 1, true},
    // FONTS2 is the 16x16 display face.
    {3, "FONTS2", 16, 16, 32, 64, 32, 24, 32, 8, 32, 7, 0, 1, true},
    {4, "FONTS4", 8, 8, 32, 96, 32, 16, 14, 8, 42, 3, 0, 1, true},
    {5, "FONTS4", 8, 8, 32, 96, 32, 16, 14, 8, 84, 3, 0, 1, true},
    // FONTS3 is a second small face (y = 32).
    {7, "FONTS3", 8, 8, 32, 64, 32, 16, 16, 8, 32, 4, 0, 1, true},
    {8, "FONTS4", 8, 8, 32, 64, 32, 16, 14, 8, 126, 3, 0, 1, true},
    // NUMFONTS: four copies of the ten digits, fixed cell width (raw copy).
    {9, "NUMFONTS", 8, 10, 48, 10, 10, 16, 16, 8, 6, 8, 0, 1, false},
    {10, "NUMFONTS", 8, 10, 48, 10, 10, 16, 16, 0, 22, 8, 0, 1, false},
    {11, "NUMFONTS", 8, 8, 48, 10, 10, 16, 8, 8, 32, 8, 0, 1, false},
    {12, "NUMFONTS", 8, 8, 48, 10, 10, 16, 8, 0, 40, 8, 0, 1, false},
};

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool tag_is(const SmlMarkupTag& tag, std::string_view name) {
    return lowered(tag.name) == name;
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

std::string normalize(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(c == '\r' || c == '\n' ? ' ' : c);
    }
    return out;
}

// The atlas is stored bottom-up (the BMP is loaded without a row flip) and the
// glyph is addressed in buffer coordinates: image row = height - 1 - y.
std::uint8_t atlas_pixel(const BmpImage& image, int x, int y) {
    return image.pixels[static_cast<std::size_t>(image.height - 1 - y) *
                            static_cast<std::size_t>(image.width) +
                        static_cast<std::size_t>(x)];
}

} // namespace

const FontSpec* font_table_spec(int number) {
    for (const FontSpec& spec : kSpecs) {
        if (spec.number == number) {
            return &spec;
        }
    }
    return nullptr;
}

const FontSpec* font_spec(int number) {
    return font_table_spec(logical_font_number(number));
}

int logical_font_number(int number) {
    // 0x497b10 / 0x497b60: dec eax twice -- 1 -> 0, 2 -> 5.
    if (number == 1) {
        return 0;
    }
    if (number == 2) {
        return 5;
    }
    return number;
}

int font_line_height(int number, int small, int big) {
    // 0x4983c8: sub eax,3 / neg / sbb / and al,0xf8 / add 0x12.
    return number == kBigFont ? big : small;
}

int FontAtlas::advance_of(int ch) const {
    if (ch < first_char || glyphs.empty()) {
        return 0;
    }
    const std::size_t index = static_cast<std::size_t>(ch - first_char);
    if (index >= advance.size()) {
        return 0;
    }
    return advance[index];
}

const FontGlyph* FontAtlas::glyph(int ch) const {
    if (ch < first_char || glyphs.empty()) {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(ch - first_char);
    if (index >= glyphs.size()) {
        return nullptr;
    }
    return &glyphs[index];
}

int FontAtlas::measure(std::string_view text) const {
    int total = 0;
    for (const char c : text) {
        total += advance_of(static_cast<unsigned char>(c));
    }
    return total;
}

int measure_text(const FontAtlas& font, std::string_view text) {
    return font.measure(text);
}

// ------------------------------------------------------------------ load --

std::optional<FontAtlas> load_font_bmp(int number, std::span<const std::uint8_t> bmp,
                                       std::string& error) {
    error.clear();
    if (number < 0 || number >= kFontNumbers) {
        error = "text_layout: font " + std::to_string(number) + " outside 0..30";
        return std::nullopt;
    }
    const FontSpec* spec = font_spec(number);
    if (spec == nullptr) {
        error = "text_layout: font " + std::to_string(number) +
                " has no atlas in the engine font table";
        return std::nullopt;
    }
    std::string parse_error;
    auto image = parse_bmp(bmp, parse_error);
    if (!image) {
        error = "text_layout: " + std::string(spec->atlas) + ": " + parse_error;
        return std::nullopt;
    }
    if (image->width <= 0 || image->height <= 0) {
        error = "text_layout: " + std::string(spec->atlas) + ": empty bitmap";
        return std::nullopt;
    }

    // The furthest cell the grid touches; reject an atlas that is too small
    // instead of reading outside it.
    const int rows = (spec->glyph_count + spec->columns - 1) / spec->columns;
    const int need_w = spec->origin_x + (spec->columns - 1) * spec->column_pitch +
                       spec->cell_width;
    const int need_h = spec->origin_y + (rows - 1) * spec->row_pitch + spec->cell_height;
    if (need_w > image->width || need_h > image->height) {
        error = "text_layout: " + std::string(spec->atlas) + " is " +
                std::to_string(image->width) + "x" + std::to_string(image->height) +
                ", font " + std::to_string(number) + " needs " + std::to_string(need_w) +
                "x" + std::to_string(need_h);
        return std::nullopt;
    }

    FontAtlas atlas;
    atlas.number = number;
    atlas.source = spec->number;
    atlas.atlas = spec->atlas;
    atlas.width = image->width;
    atlas.height = image->height;
    atlas.cell_width = spec->cell_width;
    atlas.cell_height = spec->cell_height;
    atlas.first_char = spec->first_char;
    atlas.last_char = spec->first_char + spec->glyph_count - 1;
    atlas.columns = spec->columns;
    atlas.palette = image->palette;
    atlas.advance.assign(static_cast<std::size_t>(spec->glyph_count), 0);
    atlas.glyphs.resize(static_cast<std::size_t>(spec->glyph_count));

    for (int i = 0; i < spec->glyph_count; ++i) {
        const int gx = spec->origin_x + (i % spec->columns) * spec->column_pitch;
        const int gy = spec->origin_y + (i / spec->columns) * spec->row_pitch;
        const auto nonzero = [&](int column) {
            for (int row = 0; row < spec->cell_height; ++row) {
                if (atlas_pixel(*image, gx + column, gy + row) != 0) {
                    return true;
                }
            }
            return false;
        };

        FontGlyph& glyph = atlas.glyphs[static_cast<std::size_t>(i)];
        int advance = 0;
        if (spec->ink_trim) {
            if (spec->first_char + i == ' ') {
                // 0x47ff5b: the space advance comes from the call argument.
                advance = spec->space_advance;
            } else {
                int low = -1;
                for (int column = 0; column < spec->cell_width; ++column) {
                    if (nonzero(column)) {
                        low = column;
                        break;
                    }
                }
                if (low >= 0) {
                    int high = spec->cell_width - 1;
                    while (high > low && !nonzero(high)) {
                        --high;
                    }
                    glyph.width = high - low + 1;
                    advance = glyph.width;
                    glyph.pixels.reserve(static_cast<std::size_t>(glyph.width) *
                                         static_cast<std::size_t>(spec->cell_height));
                    for (int row = 0; row < spec->cell_height; ++row) {
                        for (int column = 0; column < glyph.width; ++column) {
                            glyph.pixels.push_back(
                                atlas_pixel(*image, gx + low + column, gy + row));
                        }
                    }
                }
                // No ink: the engine stores advance 0 (0x480031).
            }
        } else {
            glyph.width = spec->cell_width;
            advance = spec->first_char + i == ' ' ? spec->space_advance : spec->cell_width;
            glyph.pixels.reserve(static_cast<std::size_t>(glyph.width) *
                                 static_cast<std::size_t>(spec->cell_height));
            for (int row = 0; row < spec->cell_height; ++row) {
                for (int column = 0; column < glyph.width; ++column) {
                    glyph.pixels.push_back(atlas_pixel(*image, gx + column, gy + row));
                }
            }
        }
        glyph.advance = advance + spec->extra_before + spec->extra_after;
        atlas.advance[static_cast<std::size_t>(i)] = glyph.advance;
    }
    return atlas;
}

std::optional<FontAtlas> load_font(const std::filesystem::path& game, int number,
                                   std::string& error) {
    error.clear();
    if (number < 0 || number >= kFontNumbers) {
        error = "text_layout: font " + std::to_string(number) + " outside 0..30";
        return std::nullopt;
    }
    const FontSpec* spec = font_spec(number);
    if (spec == nullptr) {
        error = "text_layout: font " + std::to_string(number) +
                " has no atlas in the engine font table";
        return std::nullopt;
    }

    std::filesystem::path index = game / "FRONTEND" / "BMP.IDX";
    std::error_code ec;
    if (!std::filesystem::exists(index, ec)) {
        if (const auto resolved = resolve_ci(game, "FRONTEND/BMP.IDX")) {
            index = *resolved;
        } else {
            error = "text_layout: FRONTEND/BMP.IDX not found under " + game.string();
            return std::nullopt;
        }
    }

    std::string open_error;
    auto archive = BigFile::open(index, open_error);
    if (!archive) {
        error = "text_layout: " + open_error;
        return std::nullopt;
    }

    const std::string wanted = lowered(spec->atlas);
    const BigEntry* entry = nullptr;
    for (const BigEntry& candidate : archive->entries()) {
        if (lowered(candidate.name) == wanted) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) {
        error = "text_layout: " + std::string(spec->atlas) + " missing from " + index.string();
        return std::nullopt;
    }
    std::string read_error;
    auto blob = archive->read(*entry, read_error);
    if (!blob) {
        error = "text_layout: " + read_error;
        return std::nullopt;
    }
    return load_font_bmp(number, *blob, error);
}

// ---------------------------------------------------------------- layout --

namespace {

struct WrapResult {
    std::size_t chars = 0;
    int width = 0;
    bool wrap = false;
};

/// Greedy word wrap, the result of the search in ``0x497c20``: the longest
/// space-delimited prefix that fits, or a forced break when a single word is
/// wider than the line.
WrapResult wrap_run(std::string_view run, const FontAtlas& font, int available) {
    WrapResult out;
    if (run.empty()) {
        return out;
    }
    if (available <= 0) {
        out.wrap = true;
        return out;
    }
    int width = 0;
    std::size_t index = 0;
    std::int64_t last_space = -1;
    while (index < run.size()) {
        const int advance = font.advance_of(static_cast<unsigned char>(run[index]));
        if (width + advance > available) {
            break;
        }
        width += advance;
        if (run[index] == ' ') {
            last_space = static_cast<std::int64_t>(index);
        }
        ++index;
    }
    if (index == run.size()) {
        out.chars = index;
        out.width = width;
        // 0x497cf1: a run that fills the line exactly still triggers a break.
        out.wrap = width >= available;
        return out;
    }
    if (last_space >= 0) {
        out.chars = static_cast<std::size_t>(last_space) + 1;
        out.width = font.measure(run.substr(0, out.chars));
        out.wrap = true;
        return out;
    }
    if (index == 0) {
        // Nothing fits: force one character so a page can never loop forever.
        out.chars = 1;
        out.width = font.advance_of(static_cast<unsigned char>(run[0]));
        out.wrap = true;
        return out;
    }
    out.chars = index;
    out.width = width;
    out.wrap = true;
    return out;
}

struct PageState {
    int x = 0;
    int y = 0;
    int column_start = 0;   // 0x68ee74
    int right_edge = 0;     // 0x68e658
    int right = 0;          // current limit (right_edge minus right floats)
    int left_float_bottom = 0;   // 0x68e654
    int right_float_bottom = 0;  // 0x68e644
    int font = kDefaultFont;
    int saved_font = kDefaultFont;
    int line_height = kDefaultLineHeight;
    std::size_t next = 0;
};

struct LayoutContext {
    const SmlDocument& doc;
    const LayoutOptions& options;
    const FontLookup& fonts;
    std::string* error = nullptr;
    bool report_missing = false;
    bool failed = false;

    void fail(std::string message) {
        if (!failed && error != nullptr) {
            *error = std::move(message);
        }
        failed = true;
    }
};

const FontAtlas* atlas_for(LayoutContext& ctx, int number) {
    const FontAtlas* atlas = ctx.fonts(number);
    if (atlas == nullptr && ctx.report_missing) {
        ctx.fail("text_layout: no atlas for font " + std::to_string(number));
    }
    return atlas;
}

int height_for(const LayoutContext& ctx, int font) {
    return font_line_height(font, ctx.options.line_height, ctx.options.big_line_height);
}

void line_feed(PageState& state, const LayoutContext& ctx, std::size_t after) {
    state.y += state.line_height;
    // 0x497bc1: past the bottom of a left float the column start resets.
    if (state.y > state.left_float_bottom) {
        state.column_start = 0;
    }
    // 0x497bdb: past the bottom of a right float the right edge is restored.
    if (state.y > state.right_float_bottom) {
        state.right_edge = ctx.options.width;
    }
    state.right = state.right_edge;
    state.x = state.column_start;
    // The engine only records the marker when the *next* line still fits
    // (0x497c08).  We record the first line that does not fit instead, so pages
    // never repeat a line (see docs/TEXT_LAYOUT.md).
    state.next = after;
}

std::string next_run(const SmlDocument& doc, std::size_t tag_index) {
    const auto& tags = doc.tags;
    const std::size_t begin = tags[tag_index].pos + tags[tag_index].raw.size();
    const std::size_t end = tag_index + 1 < tags.size() ? tags[tag_index + 1].pos : doc.text.size();
    if (end <= begin) {
        return {};
    }
    return normalize(std::string_view(doc.text).substr(begin, end - begin));
}

} // namespace

LayoutResult layout_text(const SmlDocument& doc, const FontAtlas& font,
                         const LayoutOptions& options, std::string* error) {
    const FontLookup lookup = [&font](int) { return &font; };
    return layout_text(doc, lookup, font.number, options, error);
}

LayoutResult layout_text(const SmlDocument& doc, const FontLookup& fonts, int default_font,
                         const LayoutOptions& options, std::string* error) {
    LayoutContext ctx{doc, options, fonts, error, true, false};
    if (error != nullptr) {
        error->clear();
    }
    LayoutResult result;
    result.width = options.width;
    result.height = options.height;
    result.line_height = options.line_height;
    result.font = default_font;

    if (doc.text.empty()) {
        ctx.fail("text_layout: empty document");
        return result;
    }
    if (options.width <= 0 || options.height <= 0) {
        ctx.fail("text_layout: non-positive page size " + std::to_string(options.width) +
                 "x" + std::to_string(options.height));
        return result;
    }
    if (options.line_height <= 0 || options.big_line_height <= 0) {
        ctx.fail("text_layout: non-positive line height");
        return result;
    }
    const FontAtlas* base = atlas_for(ctx, default_font);
    if (base == nullptr || !base->has_glyphs()) {
        ctx.fail("text_layout: font " + std::to_string(default_font) + " has no glyphs");
        return result;
    }
    if (options.max_pages <= 0) {
        ctx.fail("text_layout: non-positive page limit");
        return result;
    }

    const auto& tags = doc.tags;
    std::size_t start = 0;
    while (true) {
        PageState state;
        state.right_edge = options.width;
        state.right = options.width;

        // Recover the font state from the ``<f>`` tags that precede the page
        // start (0x497a80 scans exactly these tags before laying out).
        state.font = default_font;
        state.saved_font = default_font;
        for (const SmlMarkupTag& tag : tags) {
            if (tag.pos >= start) {
                break;
            }
            if (!tag_is(tag, "f")) {
                continue;
            }
            if (tag.closing) {
                state.font = state.saved_font;
            } else {
                state.saved_font = state.font;
                if (sml_attribute(tag, "s") != nullptr) {
                    state.font = attr_int(tag, "s", state.font);
                }
            }
        }
        state.line_height = height_for(ctx, state.font);

        LayoutPage page;
        page.index = static_cast<int>(result.pages.size());
        page.start = start;
        std::size_t next = 0;
        bool closed = false;

        // Merge runs that share a baseline into one visual line (a <s> or a
        // centred column can emit several runs on the same row).
        const auto emit_run = [&](const LayoutRun& piece) {
            result.runs.push_back(piece);
            if (!page.lines.empty() && page.lines.back().y == piece.y) {
                LayoutLine& line = page.lines.back();
                line.width = std::max(line.width, piece.x + piece.width - line.x);
                line.last = piece.last;
            } else {
                LayoutLine line;
                line.x = piece.x;
                line.y = piece.y;
                line.width = piece.width;
                line.height = state.line_height;
                line.font = piece.font;
                line.first = piece.first;
                line.last = piece.last;
                page.lines.push_back(std::move(line));
            }
            page.used_height = std::max(page.used_height, piece.y + state.line_height);
        };

        std::size_t tag_index = 0;
        while (tag_index < tags.size() && tags[tag_index].pos < start) {
            ++tag_index;
        }
        std::size_t pos = start;
        while (pos < doc.text.size()) {
            if (state.y > options.height - state.line_height) {
                break;
            }
            while (tag_index < tags.size() && tags[tag_index].pos < pos) {
                ++tag_index;
            }
            const std::size_t tag_pos = tag_index < tags.size() ? tags[tag_index].pos
                                                                : doc.text.size();
            if (tag_pos > pos) {
                std::string run = normalize(std::string_view(doc.text).substr(pos, tag_pos - pos));
                std::size_t skip = 0;
                if (state.x == state.column_start) {
                    while (skip < run.size() && run[skip] == ' ') {
                        ++skip;
                    }
                }
                if (skip > 0) {
                    pos += skip;
                    run.erase(0, skip);
                }
                if (run.empty()) {
                    pos = tag_pos;
                    continue;
                }
                const FontAtlas* atlas = atlas_for(ctx, state.font);
                if (atlas == nullptr) {
                    break;
                }
                const int available = state.right - state.x;
                WrapResult wrapped;
                if (options.wrap) {
                    wrapped = wrap_run(run, *atlas, available);
                } else {
                    wrapped.chars = run.size();
                    wrapped.width = atlas->measure(run);
                }
                if (wrapped.chars == 0) {
                    // No progress possible at the start of a line: force one
                    // glyph (the engine would loop in 0x497c20 here).
                    if (state.x == state.column_start) {
                        wrapped.chars = 1;
                        wrapped.width = atlas->advance_of(static_cast<unsigned char>(run[0]));
                        wrapped.wrap = true;
                    } else {
                        line_feed(state, ctx, pos);
                        continue;
                    }
                }

                LayoutRun piece;
                piece.x = state.x;
                piece.y = state.y;
                piece.width = wrapped.width;
                piece.font = state.font;
                piece.first = pos;
                piece.last = pos + wrapped.chars;
                piece.text = run.substr(0, wrapped.chars);
                emit_run(piece);

                state.x += wrapped.width;
                pos += wrapped.chars;
                if (wrapped.wrap) {
                    line_feed(state, ctx, pos);
                }
                continue;
            }

            const SmlMarkupTag& tag = tags[tag_index];
            const std::size_t tag_end = tag.pos + tag.raw.size();
            if (tag.closing) {
                if (tag_is(tag, "c")) {
                    line_feed(state, ctx, tag_end);
                } else if (tag_is(tag, "f")) {
                    state.font = state.saved_font;
                    state.line_height = height_for(ctx, state.font);
                } else if (tag_is(tag, "d")) {
                    closed = true;
                    next = 0;
                    break;
                }
            } else if (tag_is(tag, "r")) {
                // 0x497d7c: a newline at the very start of a page is a no-op.
                if (tag.pos != start) {
                    line_feed(state, ctx, tag_end);
                }
            } else if (tag_is(tag, "p")) {
                // 0x497da0: half a line of paragraph spacing, then a newline;
                // at a full line the extra half is skipped.
                if (state.x != state.right) {
                    state.y += state.line_height / 2;
                }
                line_feed(state, ctx, tag_end);
            } else if (tag_is(tag, "c")) {
                // 0x497ec2: centre the next text run in the remaining width.
                const std::string segment = next_run(doc, tag_index);
                const FontAtlas* atlas = atlas_for(ctx, state.font);
                const int measured = atlas != nullptr ? atlas->measure(segment) : 0;
                const int remaining = state.right - state.x;
                if (measured < remaining) {
                    state.x += (remaining - measured) / 2;
                }
            } else if (tag_is(tag, "f")) {
                // 0x497e9f: remember the previous font, then select ``s``.
                state.saved_font = state.font;
                if (sml_attribute(tag, "s") != nullptr) {
                    state.font = attr_int(tag, "s", state.font);
                }
                state.line_height = height_for(ctx, state.font);
            } else if (tag_is(tag, "x")) {
                // 0x497f38: value * width / 100, guarded to [column, right).
                const int percent = attr_int(tag, "v", attr_int(tag, "x", 0));
                const int placed = static_cast<int>(
                    (static_cast<long long>(percent) * options.width) / 100);
                if (placed < state.right && placed >= state.column_start) {
                    state.x = placed;
                }
            } else if (tag_is(tag, "s")) {
                // 0x497de8: insert one space worth of advance.
                const FontAtlas* atlas = atlas_for(ctx, state.font);
                const int space = atlas != nullptr ? atlas->measure(" ") : 0;
                const int advance = std::min(space, state.right - state.x);
                if (advance > 0) {
                    LayoutRun piece;
                    piece.x = state.x;
                    piece.y = state.y;
                    piece.width = advance;
                    piece.font = state.font;
                    piece.first = tag.pos;
                    piece.last = tag_end;
                    piece.text = " ";
                    emit_run(piece);
                    state.x += advance;
                }
            } else if (tag_is(tag, "k")) {
                // 0x497e2c: the callsign "Soldier".
                const FontAtlas* atlas = atlas_for(ctx, state.font);
                const std::string callsign = "Soldier";
                const int advance = atlas != nullptr ? atlas->measure(callsign) : 0;
                if (advance >= state.right - state.x) {
                    line_feed(state, ctx, tag_end);
                    if (state.y > options.height - state.line_height) {
                        // The callsign did not fit on this page: retry it on
                        // the next one instead of drawing below the page.
                        state.next = tag.pos;
                        break;
                    }
                }
                LayoutRun piece;
                piece.x = state.x;
                piece.y = state.y;
                piece.width = advance;
                piece.font = state.font;
                piece.first = tag.pos;
                piece.last = tag_end;
                piece.text = callsign;
                emit_run(piece);
                state.x += advance;
            } else if (tag_is(tag, "i")) {
                // 0x498012: inline illustration, laid out as a float.  The
                // engine moves the column start (left) or the right edge
                // (right) and restores it once the line passes the image.
                LayoutImage image;
                image.width = options.image_width;
                image.height = options.image_height;
                image.align = 'l';
                if (const std::string* align = sml_attribute(tag, "a")) {
                    if (!align->empty()) {
                        const char c = (*align)[0];
                        image.align = (c == 'R' || c == 'r') ? 'r' : 'l';
                    }
                }
                if (const std::string* name = sml_attribute(tag, "f")) {
                    image.name = *name;
                }
                // Placement is the float anchor, not the (corpus-constant)
                // x/y attributes: the engine moves the column start for a
                // left image and the right edge for a right one.  The image
                // is clamped to the page like 0x498332 does.
                image.y = std::clamp(state.y, 0, options.height);
                image.height = std::max(0, std::min(options.image_height,
                                                    options.height - image.y));
                image.pos = tag.pos;
                const int bottom = image.y + image.height;
                if (image.align == 'r') {
                    image.x = std::max(0, state.right_edge - image.width);
                    state.right_edge = image.x;
                    state.right = state.right_edge;
                    state.right_float_bottom = bottom;
                } else {
                    image.x = state.column_start;
                    const FontAtlas* atlas = atlas_for(ctx, state.font);
                    const int space = atlas != nullptr ? atlas->measure(" ") : 0;
                    state.column_start =
                        std::min(options.width, state.column_start + space + image.width);
                    state.left_float_bottom = bottom;
                }
                page.images.push_back(std::move(image));
                page.used_height = std::max(page.used_height, bottom);
            }
            // <m>, <d>, unknown letters and </r>, </p>, </x>, </s>, </k>, </i>
            // are layout no-ops in the engine.
            pos = tag_end;
            if (tag_index < tags.size()) {
                ++tag_index;
            }
        }

        if (!closed && !ctx.failed) {
            next = pos >= doc.text.size() ? 0 : state.next;
            if (next <= start) {
                next = 0;
            }
        }
        page.end = next;
        result.pages.push_back(std::move(page));
        if (closed || ctx.failed || next == 0) {
            break;
        }
        // A document that ends exactly at a page boundary would otherwise add
        // an empty page; drop it and mark the previous page as final.
        if (result.pages.size() >= static_cast<std::size_t>(options.max_pages)) {
            result.truncated = true;
            ctx.fail("text_layout: page limit " + std::to_string(options.max_pages) +
                     " reached (truncated)");
            break;
        }
        start = next;
    }

    // Drop a trailing page with no content (page break just before </d>).
    if (!result.pages.empty()) {
        const LayoutPage& last = result.pages.back();
        if (last.lines.empty() && last.images.empty() && result.pages.size() > 1) {
            result.pages.pop_back();
            result.pages.back().end = 0;
        }
    }
    return result;
}

TextBounds text_bounds(const LayoutResult& layout) {
    TextBounds bounds;
    bounds.pages = static_cast<int>(layout.pages.size());
    for (const LayoutPage& page : layout.pages) {
        for (const LayoutLine& line : page.lines) {
            ++bounds.lines;
            bounds.width = std::max(bounds.width, line.x + line.width);
            bounds.height = std::max(bounds.height, line.y + line.height);
        }
        for (const LayoutImage& image : page.images) {
            bounds.width = std::max(bounds.width, image.x + image.width);
            bounds.height = std::max(bounds.height, image.y + image.height);
        }
    }
    return bounds;
}

namespace {

bool check_target(const RenderResult& target, std::string& error) {
    if (target.width <= 0 || target.height <= 0) {
        error = "text_layout: target has no pixels (" + std::to_string(target.width) + "x" +
                std::to_string(target.height) + ")";
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(target.width) *
                                 static_cast<std::size_t>(target.height) * 3u;
    if (target.rgb.size() != expected) {
        error = "text_layout: target RGB is " + std::to_string(target.rgb.size()) +
                " bytes, expected " + std::to_string(expected);
        return false;
    }
    return true;
}

bool blit_run(RenderResult& target, const LayoutRun& run, const FontAtlas& atlas,
              int origin_x, int origin_y, std::string& error) {
    if (!atlas.has_glyphs()) {
        error = "text_layout: font " + std::to_string(run.font) + " is metrics-only";
        return false;
    }
    const std::size_t palette_entries = atlas.palette.size() / 4;
    if (palette_entries == 0) {
        error = "text_layout: font " + std::to_string(run.font) + " has no palette";
        return false;
    }
    int pen = run.x;
    for (const char raw : run.text) {
        const auto ch = static_cast<unsigned char>(raw);
        const FontGlyph* glyph = atlas.glyph(ch);
        if (glyph == nullptr) {
            continue;
        }
        const int advance = atlas.advance_of(ch);
        if (glyph->width > 0 && !glyph->pixels.empty()) {
            for (int row = 0; row < atlas.cell_height; ++row) {
                // Glyph row 0 is the bottom row (engine buffer order).
                const int y = origin_y + run.y + atlas.cell_height - 1 - row;
                if (y < 0 || y >= target.height) {
                    continue;
                }
                for (int column = 0; column < glyph->width; ++column) {
                    const std::uint8_t index =
                        glyph->pixels[static_cast<std::size_t>(row) * glyph->width + column];
                    if (index == 0 || index >= palette_entries) {
                        continue;
                    }
                    const int x = origin_x + pen + column;
                    if (x < 0 || x >= target.width) {
                        continue;
                    }
                    const std::size_t dst =
                        (static_cast<std::size_t>(y) * target.width + x) * 3u;
                    const std::size_t src = static_cast<std::size_t>(index) * 4u;
                    target.rgb[dst] = atlas.palette[src];
                    target.rgb[dst + 1] = atlas.palette[src + 1];
                    target.rgb[dst + 2] = atlas.palette[src + 2];
                }
            }
        }
        pen += advance;
    }
    return true;
}

} // namespace

bool draw_text(RenderResult& target, const LayoutResult& layout, const FontAtlas& font,
               int origin_x, int origin_y, std::string& error) {
    const FontLookup lookup = [&font](int) { return &font; };
    return draw_text(target, layout, lookup, origin_x, origin_y, error);
}

bool draw_text(RenderResult& target, const LayoutResult& layout, const FontLookup& fonts,
               int origin_x, int origin_y, std::string& error) {
    error.clear();
    if (!check_target(target, error)) {
        return false;
    }
    for (const LayoutRun& run : layout.runs) {
        const FontAtlas* atlas = fonts(run.font);
        if (atlas == nullptr) {
            error = "text_layout: no atlas for font " + std::to_string(run.font);
            return false;
        }
        if (!blit_run(target, run, *atlas, origin_x, origin_y, error)) {
            return false;
        }
    }
    return true;
}

} // namespace gm
