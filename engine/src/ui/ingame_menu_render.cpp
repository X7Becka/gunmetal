#include "ui/ingame_menu_render.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "res/bigfile.h"
#include "res/bmp.h"

namespace gm {

std::string ingame_menu_background_for_start_arg(int argument) {
    // 0x49720c `test eax,eax; jne 0x497238` — 0 keeps `0x4be8a8` (DEMOTION),
    // everything else copies `0x4be8a0` (DISBAND).
    return std::string(argument == 0 ? kIngameMenuDemotion : kIngameMenuDisband);
}

int ingame_menu_highlighted_item(const IngameMenuRenderOptions& options) {
    // No item list exists on this screen, so `0x485dd0`'s `item == [0x68ccf4]`
    // test cannot match anything.  The cursor is deliberately not consulted.
    (void)options;
    return kIngameMenuNoHighlight;
}

namespace {

constexpr std::string_view kPrefix = "ingame_menu_render: ";

/// Strip the module prefix of a nested error (the archive comes from
/// `res/bigfile`, the BMP from `res/bmp`) so the caller sees one
/// `"ingame_menu_render: ..."` chain instead of a doubled one.
std::string detail_of(std::string_view message) {
    constexpr std::string_view kBigFile = "bigfile: ";
    constexpr std::string_view kBmp = "bmp: ";
    if (message.rfind(kBigFile, 0) == 0) {
        return std::string(message.substr(kBigFile.size()));
    }
    if (message.rfind(kBmp, 0) == 0) {
        return std::string(message.substr(kBmp.size()));
    }
    return std::string(message);
}

/// Failure contract: no stale pixels and no stale numbers for the caller.
void clear(std::vector<std::uint32_t>& rgba, int& width, int& height,
           IngameMenuRenderStats* stats) {
    rgba.clear();
    width = 0;
    height = 0;
    if (stats != nullptr) {
        *stats = IngameMenuRenderStats{};
    }
}

/// One palette entry as `0xRRGGBBAA`; the BMP reserved byte is not alpha, so it
/// is forced to 255 (same as `ui/pager_render` and `ui/menu_render`).
std::uint32_t palette_colour(std::span<const std::uint8_t> palette, std::size_t index) {
    const std::size_t base = index * 4u;
    return (static_cast<std::uint32_t>(palette[base + 0]) << 24) |
           (static_cast<std::uint32_t>(palette[base + 1]) << 16) |
           (static_cast<std::uint32_t>(palette[base + 2]) << 8) | 0xFFu;
}

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

/// Case-insensitive lookup of one `FRONTEND/BMP.IDX` entry and its parsed BMP
/// (read-only; the index is reopened per call, so no archive object outlives
/// the call).
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

/// Paint one 320x200 background over the whole canvas, exactly as `0x47efe0`
/// fills the state-9 canvas: raw row order of the file (= `flipV` of the
/// top-down `parse_bmp` result), one palette index per pixel, no key, no
/// palette swap.  A mapped index the source palette cannot hold is a text,
/// never a wrapped or dropped pixel.
bool paint_background(const BmpImage& image, const HudLut* fade_table, int fade_level,
                      std::vector<std::uint32_t>& rgba, std::string& error) {
    const std::size_t entries = image.palette.size() / 4u;
    const std::size_t width = static_cast<std::size_t>(kIngameMenuCanvasWidth);
    const std::size_t height = static_cast<std::size_t>(kIngameMenuCanvasHeight);
    for (std::size_t y = 0; y < height; ++y) {
        // The loader copies the file rows straight into the buffer and the
        // canvas is addressed top-down (`0x487500`: `canvas + y*320 + x`), so
        // the visible row `y` is file row `height - 1 - y`.
        const std::size_t source_row = height - 1 - y;
        for (std::size_t x = 0; x < width; ++x) {
            const std::uint8_t index =
                image.pixels[source_row * width + x];
            std::uint8_t mapped = index;
            if (fade_table != nullptr) {
                mapped = map(*fade_table, fade_level, index);
            }
            if (mapped >= entries) {
                error = std::string(kPrefix) + "fade LUT level " +
                        std::to_string(fade_level) + " maps index " +
                        std::to_string(index) + " to " + std::to_string(mapped) +
                        ", outside the " + std::to_string(entries) +
                        "-entry palette of the background";
                return false;
            }
            rgba[y * width + x] = palette_colour(image.palette, mapped);
        }
    }
    return true;
}

} // namespace

bool render_ingame_menu(const std::filesystem::path& game,
                        const IngameMenuRenderOptions& options,
                        std::vector<std::uint32_t>& rgba, int& width, int& height,
                        IngameMenuRenderStats* stats, std::string* error) {
    clear(rgba, width, height, stats);
    std::string local_error;
    std::string& err = error != nullptr ? *error : local_error;
    err.clear();

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

    BmpImage image;
    if (!options.background.empty()) {
        // `0x4876d9`..`0x4876f0`: the entry named by `[0x4bc1b8]`, 320x200, into
        // the canvas.  `read_bmp_entry` needs no font: this screen has no text.
        std::string load_error;
        if (!read_bmp_entry(game, options.background, image, load_error)) {
            err = std::string(kPrefix) + "background " + options.background + ": " +
                  detail_of(load_error);
            return false;
        }
        if (image.width != kIngameMenuBackgroundWidth ||
            image.height != kIngameMenuBackgroundHeight) {
            err = std::string(kPrefix) + "background " + options.background + " is " +
                  std::to_string(image.width) + "x" + std::to_string(image.height) +
                  ", expected " + std::to_string(kIngameMenuBackgroundWidth) + "x" +
                  std::to_string(kIngameMenuBackgroundHeight);
            return false;
        }
    }

    width = kIngameMenuCanvasWidth;
    height = kIngameMenuCanvasHeight;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                kIngameMenuBackgroundColour);

    if (!options.background.empty()) {
        if (!paint_background(image, fade_table, options.fade_lut_level, rgba, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    // No item list, no glyph, no sprite, no arrow: see the header comment.
    IngameMenuRenderStats counts;
    counts.rows = kIngameMenuRows;
    counts.ink_pixels = 0;
    counts.missing_glyphs = 0;
    counts.arrows = 0;
    if (stats != nullptr) {
        *stats = counts;
    }
    err.clear();
    return true;
}

} // namespace gm
