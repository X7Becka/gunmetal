#include "ui/keycfg_render.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "input/controls.h"
#include "render/text_layout.h"
#include "res/bigfile.h"
#include "res/bmp.h"

namespace gm {
namespace {

constexpr std::string_view kPrefix = "keycfg_render: ";

/// The 31 display names of the action table `0x4bc558`, in `.key` slot order.
/// Their bytes are the strings `Move Forward` (0x4bc928) … `Linear Right`
/// (0x4bc7a8); `ui/frontend_menu` keeps the same list for screens 30/34/35.
/// `input/controls` only exposes the snake_case names of its binding model, so
/// the display half is repeated here (the table is data, not code).
constexpr std::array<std::string_view, kKeycfgRows> kActionLabels = {
    "Move Forward",   "Move Backward",     "Turn Left",         "Turn Right",
    "Look Center",    "Fire",              "Look Up",           "Look Down",
    "Strafe",         "Strafe Left",       "Strafe Right",      "Throttle Up",
    "Throttle Down",  "Next Weapon",       "Previous Weapon",   "Activate",
    "Next Equipment", "Previous Equip.",   "Toggle Equip. Use", "Headlights",
    "Force Field",    "Mine Detector",     "Targeting",         "Warp Device",
    "Radar",          "Mine Layer",        "Combat Block",      "Auto Sentry",
    "Decoy",          "Linear Left",       "Linear Right",
};

/// Strip the module prefix of a nested error (the atlases come from
/// `render/text_layout`) so the caller sees one `"keycfg_render: ..."` chain
/// instead of a doubled one.
std::string detail_of(std::string_view message) {
    constexpr std::string_view kTextLayout = "text_layout: ";
    if (message.rfind(kTextLayout, 0) == 0) {
        return std::string(message.substr(kTextLayout.size()));
    }
    return std::string(message);
}

/// Failure contract: no stale pixels and no stale numbers for the caller.
void clear(std::vector<std::uint32_t>& rgba, int& width, int& height,
           KeycfgRenderStats* stats) {
    rgba.clear();
    width = 0;
    height = 0;
    if (stats != nullptr) {
        *stats = KeycfgRenderStats{};
    }
}

/// One palette entry as `0xRRGGBBAA`; the BMP reserved byte is not alpha, so
/// it is forced to 255 (same as `ui/menu_render` and `ui/start_game_render`).
std::uint32_t palette_colour(std::span<const std::uint8_t> palette, std::size_t index) {
    const std::size_t base = index * 4u;
    return (static_cast<std::uint32_t>(palette[base + 0]) << 24) |
           (static_cast<std::uint32_t>(palette[base + 1]) << 16) |
           (static_cast<std::uint32_t>(palette[base + 2]) << 8) | 0xFFu;
}

/// The minimal index layer of the exact fade (`0x682f90`): table + level,
/// applied to the palette index of a pixel **before** its colour is looked up
/// (docs/HUD_LUT.md §2b, docs/KEYCFG_RENDER.md §6).  `table == nullptr` is the
/// unfaded path, so the default options keep every byte of slice 7.
/// `ui/pager_render.cpp` has the same layer as `FadeSource` (private there), so
/// it is repeated here instead of reaching into another module's canvas.
struct PaletteFade {
    const HudLut* table = nullptr;
    int level = kHudLutIdentityLevel;

    bool active() const { return table != nullptr; }
    std::uint8_t index(std::uint8_t raw) const {
        return table == nullptr ? raw : map(*table, level, raw);
    }
};

/// `PaletteFade::index` with a bounds check: a mapped index the source palette
/// cannot hold is a text, never a wrapped or dropped pixel.  With the shipped
/// 256-entry BMP palettes this cannot fire (`map` writes 0..255), it is a guard
/// for short palettes.
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

/// The canvas plus the 196x131 write window of `0x485dd0` (the copy of
/// `PLATE`): a glyph pixel outside it cannot reach the screen.
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

/// Width of *text* with the atlas advances (`0x47fa50`), needed for the
/// centring of the title and the hint (`0x485dd0`'s `centered` branch).
int text_width(const FontAtlas& atlas, std::string_view text) {
    int width = 0;
    for (const char raw : text) {
        width += atlas.advance_of(static_cast<unsigned char>(raw));
    }
    return width;
}

/// Paint one run at (`x`, `y`), `y` being the top row of the glyph cell.
///
/// `FontGlyph::pixels` row 0 is the top cell row: the blitter `0x47faf0`
/// writes it at the destination row and goes down (`ui/menu_render`,
/// `ui/save_menu_render` and `ui/start_game_render` use the same order).
/// Palette index 0 is transparent.  Characters without a glyph increment
/// `missing_glyphs` and are skipped instead of failing.
bool draw_run(const FontLookup& fonts, int font, std::string_view text, int x, int y,
              const PaletteFade& fade, Layer& layer, KeycfgRenderStats& stats,
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

/// Paint the `MENUARRW` scroll arrow at (`x`, `y`) of the band.
///
/// `0x485cc0` blits the 128 sprite bytes verbatim (16 per row, eight rows,
/// `lea ecx,[ecx+ebx-0x10]` = stride 196, zero bytes skipped at `0x485ce0`);
/// `0x485d00` blits the same bytes backwards (`mov al,[edx-1]; dec edx` at
/// `0x485d1a`), i.e. the up arrow is the down arrow rotated by 180 degrees.
/// The loader `0x47efe0` stores the file rows as they are (`fread` of
/// `width * height` bytes at `0x47f100`), while `parse_bmp` returns them
/// bottom-up, so sprite row `r` is `pixels[height-1-r]`.
bool paint_arrow(const BmpImage& sprite, bool up, int x, int y, const PaletteFade& fade,
                 Layer& layer, std::string& error) {
    const std::size_t palette_entries = sprite.palette.size() / 4u;
    for (int row = 0; row < kKeycfgArrowHeight; ++row) {
        for (int column = 0; column < kKeycfgArrowWidth; ++column) {
            const int source_row = up ? kKeycfgArrowHeight - 1 - row : row;
            const int source_column = up ? kKeycfgArrowWidth - 1 - column : column;
            const std::size_t index =
                static_cast<std::size_t>(sprite.height - 1 - source_row) *
                    static_cast<std::size_t>(sprite.width) +
                static_cast<std::size_t>(source_column);
            const std::uint8_t value = sprite.pixels[index];
            // `0x485ce0`/`0x485d22`: a zero byte leaves the destination alone.
            if (value == 0) {
                continue;
            }
            if (value >= palette_entries) {
                error = std::string(kPrefix) + "arrow " + std::string(kKeycfgArrowName) +
                        " uses palette index " + std::to_string(value) + " of " +
                        std::to_string(palette_entries);
                return false;
            }
            const int px = x + column;
            const int py = y + row;
            if (px < layer.clip_x0 || px >= layer.clip_x1 || py < layer.clip_y0 ||
                py >= layer.clip_y1) {
                continue;
            }
            std::uint8_t mapped = value;
            if (fade.active() &&
                !fade_index(fade, value, palette_entries,
                            "arrow " + std::string(kKeycfgArrowName), mapped, error)) {
                return false;
            }
            layer.pixels[static_cast<std::size_t>(py) * static_cast<std::size_t>(layer.width) +
                         static_cast<std::size_t>(px)] =
                palette_colour(sprite.palette, mapped);
        }
    }
    return true;
}

/// Case-insensitive lookup of one `FRONTEND/BMP.IDX` entry and its parsed BMP,
/// exactly like `ui/menu_render::read_bmp_entry` / `ui/start_game_render`
/// (read-only, index reopened per asset).
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
        if (candidate.name.size() != name.size()) {
            continue;
        }
        bool same = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
            const auto upper = [](char c) {
                return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
            };
            if (upper(candidate.name[i]) != upper(name[i])) {
                same = false;
                break;
            }
        }
        if (same) {
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

/// The `MENUARRW` sprite of `FRONTEND/BMP.IDX` as the blitter expects it: an
/// 8 bpp 16x8 image.  A missing or differently sized entry is a text.
bool load_arrow(const std::filesystem::path& game, BmpImage& image, std::string& error) {
    std::string load_error;
    if (!read_bmp_entry(game, kKeycfgArrowName, image, load_error)) {
        error = std::string(kPrefix) + "arrow " + std::string(kKeycfgArrowName) + ": " +
                detail_of(load_error);
        return false;
    }
    if (image.width != kKeycfgArrowWidth || image.height != kKeycfgArrowHeight) {
        error = std::string(kPrefix) + "arrow " + std::string(kKeycfgArrowName) + " is " +
                std::to_string(image.width) + "x" + std::to_string(image.height) + ", want " +
                std::to_string(kKeycfgArrowWidth) + "x" +
                std::to_string(kKeycfgArrowHeight);
        return false;
    }
    return true;
}

/// Paint `options.background` at (0,0), clipped to the canvas.  A missing,
/// broken or non-8-bpp entry is a text, never a silently black frame.
bool paint_background(const std::filesystem::path& game, std::string_view name,
                      const PaletteFade& fade, std::vector<std::uint32_t>& rgba,
                      int width, int height, std::string& error) {
    BmpImage image;
    std::string load_error;
    if (!read_bmp_entry(game, name, image, load_error)) {
        error = std::string(kPrefix) + "background " + std::string(name) + ": " +
                detail_of(load_error);
        return false;
    }
    const std::size_t entries = image.palette.size() / 4u;
    const int copy_width = std::min(width, image.width);
    const int copy_height = std::min(height, image.height);
    for (int y = 0; y < copy_height; ++y) {
        for (int x = 0; x < copy_width; ++x) {
            const std::uint8_t index =
                image.pixels[static_cast<std::size_t>(y) *
                                 static_cast<std::size_t>(image.width) +
                             static_cast<std::size_t>(x)];
            if (index >= entries) {
                error = std::string(kPrefix) + "background " + std::string(name) +
                        " uses palette index " + std::to_string(index) + " of " +
                        std::to_string(entries);
                return false;
            }
            std::uint8_t mapped = index;
            if (fade.active() &&
                !fade_index(fade, index, entries, "background " + std::string(name),
                            mapped, error)) {
                return false;
            }
            rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)] = palette_colour(image.palette, mapped);
        }
    }
    return true;
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

/// One parsed profile: the fields screen 30 shows.
struct KeySet {
    std::string name;
    std::array<std::uint32_t, kKeycfgRows> codes{};
    std::array<std::uint32_t, kKeycfgRows> seconds{};
};

/// Read a `.key` profile.  The size check is exact (`0x178`): the loader
/// `0x48535a` does `fread(buf, 0x178, 1, f)` and rejects anything else, and a
/// truncated/grown file would silently shift or invent rows.
bool read_key_set(const std::filesystem::path& key_file, KeySet& set, std::string& error) {
    if (key_file.empty()) {
        error = std::string(kPrefix) + "no key file";
        return false;
    }
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(key_file, ec);
    if (ec) {
        error = std::string(kPrefix) + "cannot read " + key_file.string() + ": " + ec.message();
        return false;
    }
    if (size != static_cast<std::uintmax_t>(kKeyFileSize)) {
        error = std::string(kPrefix) + key_file.string() + " is " + std::to_string(size) +
                " bytes, want " + std::to_string(kKeyFileSize);
        return false;
    }
    std::ifstream in(key_file, std::ios::binary);
    if (!in) {
        error = std::string(kPrefix) + "cannot open " + key_file.string();
        return false;
    }
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>()};
    if (bytes.size() != static_cast<std::size_t>(kKeyFileSize)) {
        error = std::string(kPrefix) + key_file.string() + " changed while reading (" +
                std::to_string(bytes.size()) + " bytes)";
        return false;
    }

    // `name[16]` at +0 (`strncpy(..., 16)` at `0x485376`), NUL padded.
    std::size_t length = 0;
    while (length < 16 && bytes[length] != 0) {
        ++length;
    }
    while (length > 0 && (bytes[length - 1] == ' ' || bytes[length - 1] == '\t')) {
        --length;
    }
    set.name.assign(reinterpret_cast<const char*>(bytes.data()), length);

    // The first 31 pairs: `pair.a` -> codes, `pair.b` -> seconds.  The loader
    // `0x4853c1`..`0x4853d7` unpacks both halves into the record arrays at
    // +0x00 and +0x7c (31 u32 each), which is exactly what the handler reads.
    for (int slot = 0; slot < kKeycfgRows; ++slot) {
        const std::size_t at = 16u + static_cast<std::size_t>(slot) * 8u;
        set.codes[static_cast<std::size_t>(slot)] = read_u32(bytes, at);
        set.seconds[static_cast<std::size_t>(slot)] = read_u32(bytes, at + 4u);
    }
    return true;
}

/// `Controls::key_name` maps negative codes to `NONE`; a `.key` code is an
/// unsigned `u32` and a value with the top bit set would index far outside the
/// 123-entry table, so it is shown as `NONE` as well instead of addressing
/// memory the engine never reads.  Codes 123..0x7FFFFFFF keep `KEY_<n>`.
std::string key_label(std::uint32_t code) {
    if (code > 0x7FFFFFFFu) {
        return "NONE";
    }
    return Controls::key_name(static_cast<int>(code));
}

} // namespace

std::string_view keycfg_action_label(int slot) {
    if (slot < 0 || slot >= kKeycfgRows) {
        return {};
    }
    return kActionLabels[static_cast<std::size_t>(slot)];
}

bool render_keycfg(const std::filesystem::path& game,
                   const std::filesystem::path& key_file,
                   const KeycfgRenderOptions& options,
                   std::vector<std::uint32_t>& rgba, int& width, int& height,
                   KeycfgRenderStats* stats, std::string* error) {
    clear(rgba, width, height, stats);
    std::string local_error;
    std::string& err = error != nullptr ? *error : local_error;
    err.clear();

    // The profile is the subject of the frame: refuse before touching the
    // installation, so a caller sees the real cause of a missing screen.
    KeySet set;
    if (!read_key_set(key_file, set, err)) {
        return false;
    }
    if (game.empty()) {
        err = std::string(kPrefix) + "no game directory (the atlases come from "
                                      "FRONTEND/BMP.IDX)";
        return false;
    }

    // The exact tables are resolved before anything is painted, so a missing or
    // broken dump/table is a refusal and not a silently unfaded frame
    // (docs/KEYCFG_RENDER.md §6).
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

    // §3 faces: title 5, rows 0, cursor 4 (three FONTS4 atlases of BMP.IDX).
    std::vector<FontAtlas> owned;
    owned.reserve(3);
    for (const int number :
         {kKeycfgTitleFont, kKeycfgRowFont, kKeycfgSelectedFont}) {
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

    width = kKeycfgCanvasWidth;
    height = kKeycfgCanvasHeight;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                kKeycfgBackground);
    if (!options.background.empty() &&
        !paint_background(game, options.background, fade, rgba, width, height, err)) {
        clear(rgba, width, height, stats);
        return false;
    }

    KeycfgRenderStats counts;
    std::vector<std::uint8_t> covered(static_cast<std::size_t>(width) *
                                          static_cast<std::size_t>(height),
                                      0);
    Layer layer{rgba,
                covered,
                width,
                height,
                0,
                0,
                std::min(width, kKeycfgPanelWidth),
                std::min(height, kKeycfgPanelHeight)};

    // 0x4898a8..0x4898d4: `top = max(0, cursor - 5)`, then `top <= 21` when
    // `top + 10 >= 31` and an explicit `top >= 0` via the `cmp esi,ebp`/`jge`
    // pair whose `ebp` is zeroed at `0x489893`.  The cursor itself is clamped to
    // the list (0..30, the input side of `0x48bb22`/`0x48bcb7`): the original
    // render never validates it, so a caller's out-of-range value only shifts
    // the window and is documented, not an error.  The two arrow blits come
    // before the title and the rows; the up arrow is `top > 0`, the down arrow
    // `top + 10 < 31` (`0x4898f8`).
    const int cursor = std::clamp(options.cursor, 0, kKeycfgLastRow);
    int top = cursor >= kKeycfgScrollBand ? cursor - kKeycfgScrollBand : 0;
    if (top + kKeycfgVisibleRows >= kKeycfgRows) {
        top = kKeycfgMaxFirstRow;
    }
    top = std::max(top, 0);
    const bool arrow_up = top > 0;
    const bool arrow_down = top + kKeycfgVisibleRows < kKeycfgRows;
    counts.cursor = cursor;
    counts.first_row = top;
    counts.arrows = (arrow_up ? 1 : 0) + (arrow_down ? 1 : 0);
    if (arrow_up || arrow_down) {
        BmpImage sprite;
        if (!load_arrow(game, sprite, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
        if (arrow_up) {
            if (!paint_arrow(sprite, true, kKeycfgArrowX, kKeycfgArrowUpY, fade, layer,
                             err)) {
                clear(rgba, width, height, stats);
                return false;
            }
        }
        if (arrow_down) {
            if (!paint_arrow(sprite, false, kKeycfgArrowX, kKeycfgArrowDownY, fade, layer,
                             err)) {
                clear(rgba, width, height, stats);
                return false;
            }
        }
    }

    // 0x489872..0x48988b: the profile name, font 5, centred, y = 11.
    {
        const FontAtlas* title = fonts(kKeycfgTitleFont);
        const int title_x =
            std::max(0, (kKeycfgPanelWidth - text_width(*title, set.name)) / 2);
        if (!draw_run(fonts, kKeycfgTitleFont, set.name, title_x, kKeycfgTitleY, fade,
                      layer, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    // 0x489893..0x4898a3: `Press Esc to close` (0x4bd7c8), font 0, centred,
    // y = 20.  The editor 0x489317..0x489325 differs only in the text
    // (`Enter changes, Delete clears`, 0x4bd7a8) and in the font register: the
    // handler pushes `ebp` there, which is 0 (`xor ebp,ebp` at 0x489315), so
    // font 0 as well.
    {
        const bool edit = options.mode == KeycfgMode::edit;
        const std::string_view hint = edit ? kKeycfgHintEdit : kKeycfgHint;
        const FontAtlas* face = fonts(kKeycfgRowFont);
        const int hint_x = std::max(0, (kKeycfgPanelWidth - text_width(*face, hint)) / 2);
        if (!draw_run(fonts, kKeycfgRowFont, hint, hint_x, kKeycfgHintY, fade, layer,
                      counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    // 0x489928..0x489a02: ten bands, y = 29 + 8*i.  The action label is font 0;
    // the primary key is font 4 only on the cursor row (`top + i == cursor`,
    // `lea ecx,[esi+ebp]`/`cmp ecx,[0x68ccf4]` at `0x489942`..`0x489948`), font
    // 0 otherwise; the second code (`pair.b`) is font 0 in both branches.
    //
    // The editor 0x4893bf..0x4894c9 highlights *one* cell of that row instead:
    // `[0x68cd80]` zero -> the `x 108` column (the other cell stays font 0),
    // non-zero -> the `x 154` column (`push 0x6c` at `0x4893e5`, `push 0x9a` at
    // `0x48941d`), and the face is font 5 on screen 35 (`0x489431`/`0x48947b`)
    // but font 4 on the one-frame alias screen 34 (`0x489443`/`0x4894a4`).
    const bool edit = options.mode == KeycfgMode::edit;
    const bool edit_second = edit && options.edit_cell == KeycfgCell::second;
    const int edit_cursor_font =
        options.edit_alias_frame ? kKeycfgEditAliasCursorFont : kKeycfgEditCursorFont;
    for (int i = 0; i < kKeycfgVisibleRows; ++i) {
        const int slot = top + i;
        const int y = kKeycfgFirstRowY + kKeycfgRowStep * i;
        if (!draw_run(fonts, kKeycfgRowFont, keycfg_action_label(slot), kKeycfgActionX, y,
                      fade, layer, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
        const bool on_cursor = slot == cursor;
        const int primary_font =
            !on_cursor ? kKeycfgRowFont
                       : (!edit ? kKeycfgSelectedFont
                                : (edit_second ? kKeycfgRowFont : edit_cursor_font));
        if (!draw_run(fonts, primary_font,
                      key_label(set.codes[static_cast<std::size_t>(slot)]), kKeycfgKeyX,
                      y, fade, layer, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
        const int second_font =
            on_cursor && edit && edit_second ? edit_cursor_font : kKeycfgRowFont;
        if (!draw_run(fonts, second_font,
                      key_label(set.seconds[static_cast<std::size_t>(slot)]),
                      kKeycfgSecondKeyX, y, fade, layer, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    // 0x489539..0x48957f: the duplicate checker `0x4895e0` builds the text and
    // the handler paints it as the eleventh band: row `-29 - 8*10 = -109`
    // (`0x489563`/`0x489573`), font 5 (`0x48957d`), centred (`push 1` at
    // `0x48957a`), so `0x485dd0` turns the negative row into y = 109
    // (`0x485e2b`..`0x485e38`).  Only the editor has this call, and only when
    // the scan found something (return 1/2 at `0x48954a`/`0x489592`); the text
    // itself (`%s has been used twice`, `%s is used by other player`) is the
    // caller's business, the scan is the input side.
    if (edit && !options.conflict.empty()) {
        const FontAtlas* face = fonts(kKeycfgConflictFont);
        const int conflict_x =
            std::max(0, (kKeycfgPanelWidth - text_width(*face, options.conflict)) / 2);
        if (!draw_run(fonts, kKeycfgConflictFont, options.conflict, conflict_x,
                      kKeycfgConflictY, fade, layer, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
        counts.conflict_rows = 1;
    }

    counts.mode = options.mode;

    counts.rows = kKeycfgRows;
    counts.named_actions = 0;
    for (const std::uint32_t code : set.codes) {
        counts.named_actions += code != 0 ? 1 : 0;
    }
    if (stats != nullptr) {
        *stats = counts;
    }
    err.clear();
    return true;
}

} // namespace gm
