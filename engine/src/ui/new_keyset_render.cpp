#include "ui/new_keyset_render.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "render/text_layout.h"
#include "res/bigfile.h"
#include "res/bmp.h"

namespace gm {

std::string new_keyset_name(const NewKeysetRenderOptions& options) {
    // The field is a C string: the text rule measures it with `repnz scas`
    // (`0x405b3a`), so a byte after a NUL is not part of the name.
    const std::size_t nul = options.name.find('\0');
    const std::string_view text = nul == std::string::npos
                                      ? std::string_view(options.name)
                                      : std::string_view(options.name).substr(0, nul);
    // `push 0x10` at `0x48f323` with the `strlen >= 15` guard of `0x405c1d`:
    // the field never holds more than 15 characters plus the NUL.
    return std::string(text.substr(0, static_cast<std::size_t>(kNewKeysetMaxChars)));
}

int new_keyset_text_x(int name_width) {
    // `mov esi,0x62` at `0x48ec60`; `sub eax,edx`/`sar eax,1` at `0x48ec5a`/
    // `0x48ec65` (the width is non-negative, so the shift is a floor).
    return kNewKeysetNameCentre - name_width / 2;
}

int new_keyset_caret_width(std::uint8_t character) {
    // `0x48ec22`..`0x48ec3b`: the frame is as wide as the character it sits on.
    if (character == 'M' || character == 'W') {
        return kNewKeysetCaretWide;
    }
    if (character == 'I') {
        return kNewKeysetCaretNarrow;
    }
    return kNewKeysetCaretWidth;
}

namespace {

constexpr std::string_view kPrefix = "new_keyset_render: ";

/// Strip the module prefix of a nested error (the atlases come from
/// `render/text_layout`, the archive from `res/bigfile`) so the caller sees one
/// `"new_keyset_render: ..."` chain instead of a doubled one.
std::string detail_of(std::string_view message) {
    constexpr std::string_view kTextLayout = "text_layout: ";
    constexpr std::string_view kBigFile = "bigfile: ";
    if (message.rfind(kTextLayout, 0) == 0) {
        return std::string(message.substr(kTextLayout.size()));
    }
    if (message.rfind(kBigFile, 0) == 0) {
        return std::string(message.substr(kBigFile.size()));
    }
    return std::string(message);
}

/// Failure contract: no stale pixels and no stale numbers for the caller.
void clear(std::vector<std::uint32_t>& rgba, int& width, int& height,
           NewKeysetRenderStats* stats) {
    rgba.clear();
    width = 0;
    height = 0;
    if (stats != nullptr) {
        *stats = NewKeysetRenderStats{};
    }
}

/// One palette entry as `0xRRGGBBAA`; the BMP reserved byte is not alpha, so it
/// is forced to 255 (same as `ui/save_name_render` and `ui/play_options_render`).
std::uint32_t palette_colour(std::span<const std::uint8_t> palette, std::size_t index) {
    const std::size_t base = index * 4u;
    return (static_cast<std::uint32_t>(palette[base + 0]) << 24) |
           (static_cast<std::uint32_t>(palette[base + 1]) << 16) |
           (static_cast<std::uint32_t>(palette[base + 2]) << 8) | 0xFFu;
}

/// The minimal index layer of the exact fade (`0x682f90`): table + level,
/// applied to the palette index of a pixel **before** its colour is looked up
/// (docs/HUD_LUT.md §2b).  `table == nullptr` is the unfaded path.
struct PaletteFade {
    const HudLut* table = nullptr;
    int level = kHudLutIdentityLevel;

    bool active() const { return table != nullptr; }
    std::uint8_t index(std::uint8_t raw) const {
        return table == nullptr ? raw : map(*table, level, raw);
    }
};

/// `PaletteFade::index` with a bounds check: a mapped index the source palette
/// cannot hold is a text, never a wrapped or dropped pixel.
bool fade_index(const PaletteFade& fade, std::uint8_t raw, std::size_t palette_entries,
                std::string_view source, std::uint8_t& out, std::string& error) {
    const std::uint8_t mapped = fade.index(raw);
    if (mapped >= palette_entries) {
        error = std::string(kPrefix) + "fade LUT level " + std::to_string(fade.level) +
                " maps index " + std::to_string(raw) + " to " + std::to_string(mapped) +
                ", outside the " + std::to_string(palette_entries) + "-entry palette of " +
                std::string(source);
        return false;
    }
    out = mapped;
    return true;
}

/// One 8-bpp `FRONTEND/BMP.IDX` entry (`PLATE` or a background), kept in
/// palette indices so the exact fade can run before the palette lookup.
struct Sprite {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> palette; ///< BMP palette, 4 bytes per entry
    std::vector<std::uint8_t> indices; ///< one palette index per pixel
    std::string name;

    std::size_t palette_entries() const { return palette.size() / 4u; }
};

/// Case-insensitive comparison of an archive entry name with a wanted name.
bool same_name_ci(std::string_view have, std::string_view want) {
    if (have.size() != want.size()) {
        return false;
    }
    const auto upper = [](char c) {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    };
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (upper(have[i]) != upper(want[i])) {
            return false;
        }
    }
    return true;
}

/// Case-insensitive lookup of one `FRONTEND/BMP.IDX` entry and its parsed BMP,
/// exactly like `ui/save_name_render::read_bmp_entry` (read-only, the index is
/// reopened per asset, so no archive object outlives the call).
bool read_bmp_entry(const std::filesystem::path& game, std::string_view name,
                    BmpImage& image, std::string& error) {
    if (game.empty()) {
        error = "no game directory for FRONTEND/BMP.IDX";
        return false;
    }
    const std::filesystem::path idx = game / "FRONTEND" / "BMP.IDX";
    std::optional<BigFile> archive = BigFile::open(idx, error);
    if (!archive) {
        return false;
    }
    const BigEntry* entry = nullptr;
    for (const BigEntry& candidate : archive->entries()) {
        if (same_name_ci(candidate.name, name)) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) {
        error = std::string(name) + " is not in " + idx.string();
        return false;
    }
    std::optional<std::vector<std::uint8_t>> bytes = archive->read(*entry, error);
    if (!bytes) {
        return false;
    }
    std::optional<BmpImage> parsed = parse_bmp(*bytes, error);
    if (!parsed) {
        return false;
    }
    if (parsed->bpp != 8) {
        error = std::string(name) + " is not 8 bpp";
        return false;
    }
    if (parsed->palette.size() / 4u == 0) {
        error = std::string(name) + " has no palette";
        return false;
    }
    image = std::move(*parsed);
    return true;
}

/// Decode one `FRONTEND/BMP.IDX` entry into a palette-index sprite.  The error
/// has no module prefix; the callers add theirs.
bool load_entry(const std::filesystem::path& game, std::string_view name, Sprite& sprite,
                std::string& error) {
    BmpImage image;
    if (!read_bmp_entry(game, name, image, error)) {
        return false;
    }
    sprite.width = image.width;
    sprite.height = image.height;
    sprite.palette = std::move(image.palette);
    sprite.indices = std::move(image.pixels);
    sprite.name = std::string(name);
    return true;
}

/// Load one sprite of `FRONTEND/BMP.IDX` and refuse a size that is not the one
/// docs/UI_SCREENS.md §9 proves (`PLATE` 196x131).
bool load_sprite(const std::filesystem::path& game, std::string_view name, int want_width,
                 int want_height, Sprite& sprite, std::string& error) {
    std::string load_error;
    if (!load_entry(game, name, sprite, load_error)) {
        error = std::string(kPrefix) + std::string(name) + ": " + detail_of(load_error);
        return false;
    }
    if (sprite.width != want_width || sprite.height != want_height) {
        error = std::string(kPrefix) + std::string(name) + " is " +
                std::to_string(sprite.width) + "x" + std::to_string(sprite.height) +
                ", expected " + std::to_string(want_width) + "x" +
                std::to_string(want_height);
        return false;
    }
    return true;
}

/// The canvas plus the 196x131 write window of `0x485dd0`/`0x486050` (the copy
/// of `PLATE`): a pixel outside it cannot reach the screen.
struct Layer {
    std::vector<std::uint32_t>& pixels;
    std::vector<std::uint8_t>& covered;
    int width = 0;
    int height = 0;
    int clip_x0 = 0;
    int clip_y0 = 0;
    int clip_x1 = 0;
    int clip_y1 = 0;
};

/// Width of a text run with the atlas advances (`0x47fa50`), needed for the
/// centring of the title (`0x485dd0`'s `centered` branch) and the name.
int text_width(const FontAtlas& atlas, std::string_view text) {
    int width = 0;
    for (const char raw : text) {
        width += atlas.advance_of(static_cast<unsigned char>(raw));
    }
    return width;
}

/// Paint one run at (`x`, `y`), `y` being the top row of the glyph cell.
///
/// `FontGlyph::pixels` row 0 is written at the destination row and the blitter
/// `0x47faf0` goes down (`ui/save_name_render` and `ui/play_options_render` use
/// the same order).  Palette index 0 is transparent.  Characters without a
/// glyph increment `missing_glyphs` and are skipped instead of failing.
bool draw_run(const FontLookup& fonts, int font, std::string_view text, int x, int y,
              const PaletteFade& fade, Layer& layer, NewKeysetRenderStats& stats,
              std::string& error) {
    const FontAtlas* atlas = fonts(font);
    if (atlas == nullptr) {
        error = std::string(kPrefix) + "no atlas for font " + std::to_string(font);
        return false;
    }
    if (!atlas->has_glyphs()) {
        error = std::string(kPrefix) + "font " + std::to_string(font) + " is metrics-only";
        return false;
    }
    const std::size_t palette_entries = atlas->palette.size() / 4u;
    if (palette_entries == 0) {
        error = std::string(kPrefix) + "font " + std::to_string(font) + " has no palette";
        return false;
    }

    int pen = x;
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch == '\r' || ch == '\n') {
            continue;
        }
        const FontGlyph* glyph = atlas->glyph(ch);
        if (glyph == nullptr) {
            ++stats.missing_glyphs;
            continue;
        }
        if (glyph->width > 0 && !glyph->pixels.empty()) {
            for (int row = 0; row < atlas->cell_height; ++row) {
                const int py = y + row;
                if (py < layer.clip_y0 || py >= layer.clip_y1) {
                    continue;
                }
                for (int column = 0; column < glyph->width; ++column) {
                    const std::uint8_t index =
                        glyph->pixels[static_cast<std::size_t>(row) * glyph->width + column];
                    if (index == 0 || index >= palette_entries) {
                        continue;
                    }
                    const int px = pen + column;
                    if (px < layer.clip_x0 || px >= layer.clip_x1) {
                        continue;
                    }
                    std::uint8_t mapped = index;
                    if (fade.active() &&
                        !fade_index(fade, index, palette_entries,
                                    "font " + std::to_string(font), mapped, error)) {
                        return false;
                    }
                    const std::size_t at =
                        static_cast<std::size_t>(py) * static_cast<std::size_t>(layer.width) +
                        static_cast<std::size_t>(px);
                    layer.pixels[at] = palette_colour(atlas->palette, mapped);
                    if (layer.covered[at] == 0) {
                        layer.covered[at] = 1;
                        ++stats.ink_pixels;
                    }
                }
            }
        }
        pen += atlas->advance_of(ch);
    }
    return true;
}

/// The caret of `0x485d40`: a **hollow** frame, not a filled bar.  The helper
/// fills the top run of `width` bytes, then the left and right columns for the
/// `height - 2` middle rows, then the bottom run (`0x485d50`..`0x485dba`).
/// Painted with one palette index, so the exact fade applies to the index, and
/// clipped like every glyph; `stats.ink_pixels` counts glyphs only (the frame
/// is not glyph ink, the same rule `ui/keycfg_render` uses for `MENUARRW`).
bool draw_caret_frame(int x, int y, int width, int height, const FontAtlas& face,
                      const PaletteFade& fade, Layer& layer, std::string& error) {
    const std::size_t palette_entries = face.palette.size() / 4u;
    if (palette_entries == 0) {
        error = std::string(kPrefix) + "font " + std::to_string(face.number) +
                " has no palette";
        return false;
    }
    if (width <= 0 || height <= 0) {
        return true;
    }
    std::uint8_t index = kNewKeysetCaretIndex;
    if (fade.active() &&
        !fade_index(fade, kNewKeysetCaretIndex, palette_entries,
                    "font " + std::to_string(face.number) + " (caret)", index, error)) {
        return false;
    }
    const std::uint32_t colour = palette_colour(face.palette, index);
    const auto put = [&](int px, int py) {
        if (px < layer.clip_x0 || px >= layer.clip_x1 || py < layer.clip_y0 ||
            py >= layer.clip_y1) {
            return;
        }
        layer.pixels[static_cast<std::size_t>(py) * static_cast<std::size_t>(layer.width) +
                     static_cast<std::size_t>(px)] = colour;
    };
    for (int column = 0; column < width; ++column) {
        put(x + column, y);
        put(x + column, y + height - 1);
    }
    for (int row = 1; row < height - 1; ++row) {
        put(x, y + row);
        put(x + width - 1, y + row);
    }
    return true;
}

/// Paint one already-decoded sprite at (0,0), clipped to the canvas.  The panel
/// is a backdrop, so its pixels are not counted as `ink_pixels` (the same rule
/// as `ui/save_name_render`/`ui/play_options_render`).
bool paint_backdrop(const Sprite& sprite, const PaletteFade& fade,
                    std::vector<std::uint32_t>& rgba, int width, int height,
                    std::string& error) {
    const std::size_t entries = sprite.palette_entries();
    const int copy_width = std::min(width, sprite.width);
    const int copy_height = std::min(height, sprite.height);
    for (int y = 0; y < copy_height; ++y) {
        for (int x = 0; x < copy_width; ++x) {
            const std::uint8_t index =
                sprite.indices[static_cast<std::size_t>(y) *
                                   static_cast<std::size_t>(sprite.width) +
                               static_cast<std::size_t>(x)];
            if (index >= entries) {
                error = std::string(kPrefix) + sprite.name + " uses palette index " +
                        std::to_string(index) + " of " + std::to_string(entries);
                return false;
            }
            std::uint8_t mapped = index;
            if (fade.active() &&
                !fade_index(fade, index, entries, sprite.name, mapped, error)) {
                return false;
            }
            rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)] = palette_colour(sprite.palette, mapped);
        }
    }
    return true;
}

} // namespace

bool render_new_keyset(const std::filesystem::path& game,
                       const NewKeysetRenderOptions& options,
                       std::vector<std::uint32_t>& rgba, int& width, int& height,
                       NewKeysetRenderStats* stats, std::string* error) {
    clear(rgba, width, height, stats);
    std::string local_error;
    std::string& err = error != nullptr ? *error : local_error;
    err.clear();

    if (game.empty()) {
        err = std::string(kPrefix) + "no game directory (the atlases and the panel come "
                                      "from FRONTEND/BMP.IDX)";
        return false;
    }

    // The exact tables are resolved before anything is painted, so a missing or
    // broken dump/table is a refusal and not a silently unfaded frame.
    HudLut owned_lut;
    const HudLut* fade_table = nullptr;
    if (options.fade_lut) {
        if (options.fade_lut_table != nullptr) {
            fade_table = options.fade_lut_table;
        } else if (!options.fade_lut_dump.empty()) {
            std::string lut_error;
            if (!load_hud_lut(options.fade_lut_dump, owned_lut, &lut_error)) {
                err = std::string(kPrefix) + lut_error;
                return false;
            }
            fade_table = &owned_lut;
        } else {
            err = std::string(kPrefix) +
                  "fade LUT needs a table (fade_lut_table) or a dump (fade_lut_dump)";
            return false;
        }
    }
    const PaletteFade fade{fade_table, options.fade_lut_level};

    // Faces 5 (title, `0x48ebdc`), 3 (the name, `0x48eca3`) and 0 (the four
    // prompts, `0x48ecba`..`0x48ecff`) — three rows of the FONTS atlases.
    std::vector<FontAtlas> owned;
    owned.reserve(3);
    for (const int number : {kNewKeysetTitleFont, kNewKeysetNameFont, kNewKeysetHintFont}) {
        std::string load_error;
        std::optional<FontAtlas> atlas = load_font(game, number, load_error);
        if (!atlas) {
            err = std::string(kPrefix) + "font " + std::to_string(number) + ": " +
                  detail_of(load_error);
            return false;
        }
        owned.push_back(std::move(*atlas));
    }
    const FontLookup fonts = [&owned](int number) -> const FontAtlas* {
        for (const FontAtlas& atlas : owned) {
            if (atlas.number == number) {
                return &atlas;
            }
        }
        return nullptr;
    };

    // `0x48ebca`: the handler copies the preloaded 196x131 PLATE into
    // `0x6865f8`; without the asset the frame buffer has no content.
    Sprite plate;
    if (!load_sprite(game, kNewKeysetPanelName, kNewKeysetPanelWidth,
                     kNewKeysetPanelHeight, plate, err)) {
        return false;
    }

    width = kNewKeysetCanvasWidth;
    height = kNewKeysetCanvasHeight;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                kNewKeysetBackground);
    if (!options.background.empty()) {
        // `"PLATE"` reuses the copy that was validated above; any other name is
        // a separate BMP.IDX entry (read-only).
        const bool is_plate = same_name_ci(options.background, kNewKeysetPanelName);
        Sprite backdrop;
        const Sprite* layer = &plate;
        if (!is_plate) {
            std::string load_error;
            if (!load_entry(game, options.background, backdrop, load_error)) {
                err = std::string(kPrefix) + "background " + options.background + ": " +
                      detail_of(load_error);
                clear(rgba, width, height, stats);
                return false;
            }
            layer = &backdrop;
        }
        if (!paint_backdrop(*layer, fade, rgba, width, height, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    NewKeysetRenderStats counts;
    std::vector<std::uint8_t> covered(static_cast<std::size_t>(width) *
                                          static_cast<std::size_t>(height),
                                      0);
    Layer canvas{rgba,
                 covered,
                 width,
                 height,
                 0,
                 0,
                 std::min(width, kNewKeysetPanelWidth),
                 std::min(height, kNewKeysetPanelHeight)};

    // `0x48ebcf`..`0x48ebde`: the title, font 5, centred in the 196-px band at
    // y 14.  `0x485dd0` computes `x = (196 - width)/2` (`0x485df1`..`0x485df8`).
    const FontAtlas* title_face = fonts(kNewKeysetTitleFont);
    const int title_x =
        std::max(0, (kNewKeysetPanelWidth - text_width(*title_face, kNewKeysetTitle)) / 2);
    if (!draw_run(fonts, kNewKeysetTitleFont, kNewKeysetTitle, title_x, kNewKeysetTitleY,
                  fade, canvas, counts, err)) {
        clear(rgba, width, height, stats);
        return false;
    }

    // The name and its caret of `0x48ebe3`..`0x48ecaa`.  The width of the run
    // comes from `0x47fa50`, the same measure the handler uses for the centring
    // (`0x48ec51`) and for the caret column (`0x48ec6c`).
    const std::string name = new_keyset_name(options);
    const FontAtlas* name_face = fonts(kNewKeysetNameFont);
    const int name_width = text_width(*name_face, name);
    const int name_x = new_keyset_text_x(name_width);

    // `0x48ec81`..`0x48ec8b`: the caret frame, **before** the name, so glyph
    // pixels overwrite it.  With the cursor at the end of the name the
    // character under it is the NUL (`0x48ec08` leaves `ebp = 8`).
    if (options.caret) {
        const int caret_x = name_x + name_width;
        if (!draw_caret_frame(caret_x, kNewKeysetCaretY,
                              new_keyset_caret_width('\0'), kNewKeysetCaretHeight,
                              *name_face, fade, canvas, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    if (!draw_run(fonts, kNewKeysetNameFont, name, name_x, kNewKeysetNameY, fade, canvas,
                  counts, err)) {
        clear(rgba, width, height, stats);
        return false;
    }

    // `0x48ecad`..`0x48ed01`: the four prompts, font 0, x 26, no centring.
    const struct {
        std::string_view text;
        int y;
    } prompts[] = {
        {kNewKeysetPromptOne, kNewKeysetPromptOneY},
        {kNewKeysetPromptTwo, kNewKeysetPromptTwoY},
        {kNewKeysetEnterHint, kNewKeysetEnterHintY},
        {kNewKeysetEscHint, kNewKeysetEscHintY},
    };
    for (const auto& prompt : prompts) {
        if (!draw_run(fonts, kNewKeysetHintFont, prompt.text, kNewKeysetHintX, prompt.y,
                      fade, canvas, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    counts.rows = kNewKeysetRows; // `0x48ebb3`: `0x67f10c = 1`
    counts.chars = static_cast<int>(name.size());
    const std::size_t nul = options.name.find('\0');
    const std::size_t effective = nul == std::string::npos ? options.name.size() : nul;
    counts.truncated = std::max(0, static_cast<int>(effective) - kNewKeysetMaxChars);
    // `0x48ebb0` has no `MENUARRW` call at all (the map's asset list is
    // `plate` + fonts).
    counts.arrows = 0;
    if (stats != nullptr) {
        *stats = counts;
    }
    err.clear();
    return true;
}

} // namespace gm
