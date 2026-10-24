// One frame of screen 9, the in-game screen (`M8.8` slice 12,
// `ui/ingame_menu_render`, docs/INGAME_MENU_RENDER.md).
//
// The screen has no menu: docs/UI_SCREENS.md §2 lists rows/step/title as "—"
// for id 9, the id 9 slot of the dispatcher at `0x4883d8` is the no-op stub
// `0x487f64`, no `0x485dd0` (item blitter) call exists in the level code, and
// the state-9 branch `0x4876af` only loads one 320x200 picture into the canvas
// and presents it.  The test therefore pins a *negative* menu model as hard as
// the sibling tests pin theirs:
//
//   * model: no items, no rows, no title, no plate, no arrow; the background
//     name comes from `0x497210` (argument 0 -> `DEMOTION`, non-zero ->
//     `DISBAND`), the `.data` default of `[0x4bc1b8]` is `DEMOTION`, both
//     entries are 320x200 / 8 bpp / 65078 bytes;
//   * the shipped install: the frame is the asset in **engine row order**
//     (`flipV` of the decoded BMP, `0x47efe0` copies file rows and `0x487500`
//     addresses the canvas top-down) — 0/64000 mismatches against an
//     independent blit for both pictures, every pixel opaque, the non-black
//     bounding box is the whole 320x200 canvas, the palettes equal the
//     `GAMEPAL` entry on the indices the pictures use (255/256 entries, the
//     unused index 0 differs);
//   * `DEMOTION` vs `DISBAND` vs the bare black canvas: the three diff counts
//     are printed; the two pictures differ on 49323 of 64000 pixels and neither
//     contains a single black pixel, so both differ from black on all 64000;
//   * the cursor `[0x68ccf4]` is a no-op here: -1/0/3 give byte-identical
//     frames (0/64000) and `ingame_menu_highlighted_item` is -1 for all of them;
//   * stats: rows 0, ink 0, missing 0, arrows 0 — the shape of this screen;
//   * exact fade (`fade_lut`): level 11 is byte-identical to the unfaded frame
//     (0/64000), levels 12..15 are four different frames with the counts an
//     independent index lookup predicts;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, an
//     archive without the requested picture, one with a 196x131 `DEMOTION`
//     (size mismatch), a missing `DISBAND`, `fade_lut` without a table and a
//     dump, a missing dump file — all `ingame_menu_render:` texts with a
//     cleared buffer; an archive with **only** `DEMOTION` renders (no font, no
//     plate, no slider is needed — the proof that this screen needs nothing
//     but the picture); the name is matched case-insensitively like the file
//     system of the loader; two renders are deterministic.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/INGAME_MENU_RENDER.md §3 pins them.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "render/hud_lut.h"
#include "res/bigfile.h"
#include "res/bmp.h"
#include "ui/ingame_menu_render.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

void check_eq(long long got, long long want, const std::string& what) {
    ++checks;
    if (got != want) {
        ++failures;
        std::cerr << "FAIL: " << what << " (got " << got << ", want " << want << ")\n";
    }
}

void check_eq(const std::string& got, const std::string& want, const std::string& what) {
    ++checks;
    if (got != want) {
        ++failures;
        std::cerr << "FAIL: " << what << " (got \"" << got << "\", want \"" << want
                  << "\")\n";
    }
}

bool starts_with(const std::string& text, std::string_view prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

/// One frame (or one refusal) of the renderer.
struct Frame {
    bool ok = false;
    std::string error;
    std::vector<std::uint32_t> rgba;
    int width = 0;
    int height = 0;
    gm::IngameMenuRenderStats stats;
};

Frame run(const fs::path& game, const gm::IngameMenuRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_ingame_menu(game, options, frame.rgba, frame.width, frame.height,
                                      &frame.stats, &frame.error);
    return frame;
}

long long differing_pixels(const std::vector<std::uint32_t>& a,
                           const std::vector<std::uint32_t>& b) {
    if (a.size() != b.size()) {
        return -1;
    }
    long long count = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            ++count;
        }
    }
    return count;
}

const std::uint32_t& at(const Frame& frame, int x, int y) {
    return frame.rgba[static_cast<std::size_t>(y) *
                          static_cast<std::size_t>(frame.width) +
                      static_cast<std::size_t>(x)];
}

bool is_black(std::uint32_t pixel) { return pixel == gm::kIngameMenuBackgroundColour; }

/// Bounding box of the non-black pixels of the whole canvas.
struct Box {
    int x0 = -1;
    int y0 = -1;
    int x1 = -1;
    int y1 = -1;
    int count = 0;

    bool empty() const { return count == 0; }
};

Box ink_box(const Frame& frame) {
    Box box;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            if (is_black(at(frame, x, y))) {
                continue;
            }
            ++box.count;
            if (box.x0 < 0 || x < box.x0) {
                box.x0 = x;
            }
            if (box.y0 < 0 || y < box.y0) {
                box.y0 = y;
            }
            if (x > box.x1) {
                box.x1 = x;
            }
            if (y > box.y1) {
                box.y1 = y;
            }
        }
    }
    return box;
}

std::string box_text(const Box& box) {
    if (box.empty()) {
        return "empty";
    }
    return "(" + std::to_string(box.x0) + "," + std::to_string(box.y0) + ")-(" +
           std::to_string(box.x1) + "," + std::to_string(box.y1) + ") " +
           std::to_string(box.x1 - box.x0 + 1) + "x" +
           std::to_string(box.y1 - box.y0 + 1) + ", " + std::to_string(box.count) +
           " px";
}

// --- the asset, decoded independently ---------------------------------------

/// One 8-bpp `FRONTEND/BMP.IDX` entry as `parse_bmp` returns it (top-down rows,
/// RGBA palette forced opaque).
struct Asset {
    bool ok = false;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> indices;
    std::vector<std::uint32_t> palette;

    std::uint8_t index_at(int x, int y) const {
        return indices[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(x)];
    }
    std::uint32_t colour(std::uint8_t index) const { return palette[index]; }
};

Asset load_asset(const fs::path& game, const std::string& name) {
    Asset asset;
    std::string error;
    std::optional<gm::BigFile> archive =
        gm::BigFile::open(game / "FRONTEND" / "BMP.IDX", error);
    if (!archive) {
        return asset;
    }
    const gm::BigEntry* entry = nullptr;
    for (const gm::BigEntry& candidate : archive->entries()) {
        if (candidate.name == name) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) {
        return asset;
    }
    std::optional<std::vector<std::uint8_t>> bytes = archive->read(*entry, error);
    if (!bytes) {
        return asset;
    }
    std::optional<gm::BmpImage> image = gm::parse_bmp(*bytes, error);
    if (!image || image->bpp != 8 || image->palette.size() < 4) {
        return asset;
    }
    asset.width = image->width;
    asset.height = image->height;
    asset.indices = image->pixels;
    asset.palette.resize(image->palette.size() / 4u);
    for (std::size_t i = 0; i < asset.palette.size(); ++i) {
        const std::size_t base = i * 4u;
        asset.palette[i] = (static_cast<std::uint32_t>(image->palette[base + 0]) << 24) |
                           (static_cast<std::uint32_t>(image->palette[base + 1]) << 16) |
                           (static_cast<std::uint32_t>(image->palette[base + 2]) << 8) |
                           0xFFu;
    }
    asset.ok = true;
    return asset;
}

/// Independent engine-order blit: row `y` of the canvas is file row
/// `height - 1 - y` (`0x47efe0` raw copy + `0x487500`'s top-down canvas), the
/// palette index is looked up in the asset's own palette, alpha 255.
std::vector<std::uint32_t> compose_engine_order(const Asset& asset) {
    std::vector<std::uint32_t> out(static_cast<std::size_t>(asset.width) *
                                       static_cast<std::size_t>(asset.height),
                                   0u);
    for (int y = 0; y < asset.height; ++y) {
        const int source_row = asset.height - 1 - y;
        for (int x = 0; x < asset.width; ++x) {
            const std::uint8_t index = asset.index_at(x, source_row);
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(asset.width) +
                static_cast<std::size_t>(x)] = asset.colour(index);
        }
    }
    return out;
}

/// Mismatches between a frame and the independent blit (0 = identical).
long long mismatches(const Frame& frame, const std::vector<std::uint32_t>& want) {
    if (frame.rgba.size() != want.size()) {
        return -1;
    }
    long long wrong = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        wrong += frame.rgba[i] == want[i] ? 0 : 1;
    }
    return wrong;
}

/// Number of pixels that are not opaque.
long long non_opaque(const Frame& frame) {
    long long count = 0;
    for (const std::uint32_t pixel : frame.rgba) {
        count += (pixel & 0xFFu) == 0xFFu ? 0 : 1;
    }
    return count;
}

/// Pixels whose colour is not `#000` (index-independent black count).
long long black_pixels(const Frame& frame) {
    long long count = 0;
    for (const std::uint32_t pixel : frame.rgba) {
        // `is_black` compares the full RGBA word; a black picture index with
        // alpha 255 is exactly `kIngameMenuBackgroundColour`.
        count += is_black(pixel) ? 1 : 0;
    }
    return count;
}

// --- synthetic archives ------------------------------------------------------

fs::path temp_root() {
    std::error_code ec;
    const fs::path approved = "/tmp/opencode";
    if (fs::is_directory(approved, ec)) {
        return approved / "gm_ingame_menu_render";
    }
    return fs::temp_directory_path() / "gm_ingame_menu_render";
}

bool write_bytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

/// A `BIGFILE` index with `names` packed back to back at offsets 0, size, ...;
/// `dat` gets the bytes in the same order.
bool write_archive(const fs::path& dir, const std::vector<std::string>& names,
                   const std::vector<std::vector<std::uint8_t>>& payloads) {
    std::error_code ec;
    fs::create_directories(dir / "FRONTEND", ec);
    if (ec || names.size() != payloads.size()) {
        return false;
    }
    std::vector<std::uint8_t> idx = {'B', 'I', 'G', 'F', 'I', 'L', 'E',
                                     static_cast<std::uint8_t>(0x1A)};
    put32(idx, static_cast<std::uint32_t>(names.size()));
    std::vector<std::uint8_t> dat;
    for (std::size_t i = 0; i < names.size(); ++i) {
        std::string name = names[i];
        name.resize(8, '\0');
        idx.insert(idx.end(), name.begin(), name.end());
        put32(idx, static_cast<std::uint32_t>(dat.size()));
        dat.insert(dat.end(), payloads[i].begin(), payloads[i].end());
    }
    return write_bytes(dir / "FRONTEND" / "BMP.IDX", idx) &&
           write_bytes(dir / "FRONTEND" / "BMP.DAT", dat);
}

/// Raw bytes of one entry of the installed `FRONTEND/BMP.IDX`.
std::optional<std::vector<std::uint8_t>> installed_entry(const fs::path& game,
                                                         const std::string& name) {
    std::string error;
    std::optional<gm::BigFile> archive =
        gm::BigFile::open(game / "FRONTEND" / "BMP.IDX", error);
    if (!archive) {
        return std::nullopt;
    }
    for (const gm::BigEntry& entry : archive->entries()) {
        if (entry.name == name) {
            return archive->read(entry, error);
        }
    }
    return std::nullopt;
}

// --- refusals ----------------------------------------------------------------

void check_refused(const fs::path& game, const gm::IngameMenuRenderOptions& options,
                   const std::string& what, std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::IngameMenuRenderStats stats;
    stats.rows = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    stats.arrows = 7;
    std::string error;
    const bool ok = gm::render_ingame_menu(game, options, rgba, width, height, &stats,
                                           &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "ingame_menu_render:"),
          what + ": error prefix (got \"" + error + "\")");
    check(contains(error, expected),
          what + ": error mentions \"" + std::string(expected) + "\" (got \"" + error +
              "\")");
    check(rgba.empty(), what + ": buffer cleared");
    check_eq(width, 0, what + ": width cleared");
    check_eq(height, 0, what + ": height cleared");
    check_eq(stats.rows, 0, what + ": rows cleared");
    check_eq(stats.ink_pixels, 0, what + ": ink cleared");
    check_eq(stats.missing_glyphs, 0, what + ": missing cleared");
    check_eq(stats.arrows, 0, what + ": arrows cleared");
}

// --- model -------------------------------------------------------------------

void test_model() {
    const std::string tag = "model: ";

    check_eq(gm::kIngameMenuCanvasWidth, 320, tag + "canvas 320 wide");
    check_eq(gm::kIngameMenuCanvasHeight, 200, tag + "canvas 200 high");
    check_eq(gm::kIngameMenuBackgroundWidth, 320, tag + "background 320 wide (0x4876de)");
    check_eq(gm::kIngameMenuBackgroundHeight, 200,
             tag + "background 200 high (0x4876d9)");
    check_eq(static_cast<long long>(gm::kIngameMenuBackgroundBytes), 65078,
             tag + "background 65078 bytes (1078 + 64000)");
    check_eq(gm::kIngameMenuRows, 0, tag + "rows 0 (id 9 slot 0x487f64 is a stub)");
    check_eq(gm::kIngameMenuNoHighlight, -1, tag + "no item can be highlighted");
    check_eq(gm::kIngameMenuBackgroundColour, 0x000000FFu, tag + "black canvas #000");

    check_eq(std::string(gm::kIngameMenuDemotion), std::string("DEMOTION"),
             tag + "DEMOTION (0x4be8a8)");
    check_eq(std::string(gm::kIngameMenuDisband), std::string("DISBAND"),
             tag + "DISBAND (0x4be8a0)");
    check_eq(std::string(gm::kIngameMenuDefaultBackground), std::string("DEMOTION"),
             tag + "default background = .data 0x4bc1b8");

    // 0x49720c: `test eax,eax` — only 0 selects DEMOTION.
    check_eq(gm::ingame_menu_background_for_start_arg(0), std::string("DEMOTION"),
             tag + "start arg 0 -> DEMOTION (0x497210)");
    for (const int arg : {1, 2, -1, 0x7fff}) {
        check_eq(gm::ingame_menu_background_for_start_arg(arg), std::string("DISBAND"),
                 tag + "start arg " + std::to_string(arg) + " -> DISBAND (0x497238)");
    }

    // The default-constructed options are the shipped default, not black.
    const gm::IngameMenuRenderOptions defaults;
    check_eq(defaults.background, std::string("DEMOTION"),
             tag + "options default background DEMOTION");
    check_eq(defaults.cursor, 0, tag + "options default cursor 0");
    check(!defaults.fade_lut, tag + "options default fade off");
    check_eq(defaults.fade_lut_level, gm::kHudLutIdentityLevel, tag + "default fade level 11");

    // No item can be highlighted, whatever the cursor says.
    for (const int cursor : {-1, 0, 3, 99}) {
        gm::IngameMenuRenderOptions options;
        options.cursor = cursor;
        check_eq(gm::ingame_menu_highlighted_item(options), -1,
                 tag + "cursor " + std::to_string(cursor) + " highlights nothing");
    }
}

// --- frame -------------------------------------------------------------------

void test_frames(const fs::path& game) {
    const std::string tag = "frame: ";

    const Asset demotion = load_asset(game, "DEMOTION");
    const Asset disband = load_asset(game, "DISBAND");
    check(demotion.ok && demotion.width == 320 && demotion.height == 200,
          tag + "DEMOTION decoded 320x200");
    check(disband.ok && disband.width == 320 && disband.height == 200,
          tag + "DISBAND decoded 320x200");
    if (!demotion.ok || !disband.ok) {
        return;
    }

    // Index 0 is not used by either picture, so "no transparency key" and
    // "no black pixels" are data facts, not assumptions.
    for (const Asset* asset : {&demotion, &disband}) {
        const long long zero = static_cast<long long>(
            std::count(asset->indices.begin(), asset->indices.end(),
                       static_cast<std::uint8_t>(0)));
        check_eq(zero, 0, tag + "no pixel with index 0 in the picture");
    }

    const gm::IngameMenuRenderOptions defaults;
    const Frame frame = run(game, defaults);
    check(frame.ok, tag + "default frame (DEMOTION): " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.width, 320, tag + "width 320");
    check_eq(frame.height, 200, tag + "height 200");
    check_eq(static_cast<long long>(frame.rgba.size()), 64000, tag + "64000 pixels");
    check_eq(non_opaque(frame), 0, tag + "every pixel opaque");

    // The whole frame is the picture in engine row order.
    check_eq(mismatches(frame, compose_engine_order(demotion)), 0,
             tag + "DEMOTION frame matches the independent flipV blit");
    const Box demotion_box = ink_box(frame);
    check_eq(demotion_box.count, 64000, tag + "DEMOTION covers all 64000 pixels");
    check_eq(demotion_box.x0, 0, tag + "DEMOTION bbox x0");
    check_eq(demotion_box.y0, 0, tag + "DEMOTION bbox y0");
    check_eq(demotion_box.x1, 319, tag + "DEMOTION bbox x1");
    check_eq(demotion_box.y1, 199, tag + "DEMOTION bbox y1");
    check_eq(black_pixels(frame), 0, tag + "DEMOTION has no black pixel");

    // The menu model is empty, with the stats shape of the screen.
    check_eq(frame.stats.rows, 0, tag + "rows 0");
    check_eq(frame.stats.ink_pixels, 0, tag + "ink 0 (no glyph, no sprite)");
    check_eq(frame.stats.missing_glyphs, 0, tag + "missing glyphs 0 (no text)");
    check_eq(frame.stats.arrows, 0, tag + "arrows 0 (no MENUARRW)");

    gm::IngameMenuRenderOptions disband_options;
    disband_options.background = "DISBAND";
    const Frame disband_frame = run(game, disband_options);
    check(disband_frame.ok, tag + "DISBAND frame: " + disband_frame.error);
    if (!disband_frame.ok) {
        return;
    }
    check_eq(mismatches(disband_frame, compose_engine_order(disband)), 0,
             tag + "DISBAND frame matches the independent flipV blit");
    const Box disband_box = ink_box(disband_frame);
    check_eq(disband_box.count, 64000, tag + "DISBAND covers all 64000 pixels");
    check_eq(black_pixels(disband_frame), 0, tag + "DISBAND has no black pixel");

    // The bare canvas: what the frame is without the picture.
    gm::IngameMenuRenderOptions black_options;
    black_options.background.clear();
    const Frame black = run(game, black_options);
    check(black.ok, tag + "black canvas: " + black.error);
    if (!black.ok) {
        return;
    }
    check_eq(black.stats.rows, 0, tag + "black canvas rows 0");
    check_eq(black_pixels(black), 64000, tag + "black canvas is 64000 black pixels");

    const long long demotion_vs_disband = differing_pixels(frame.rgba, disband_frame.rgba);
    check_eq(demotion_vs_disband, 49323,
             tag + "DEMOTION vs DISBAND differs on 49323 pixels");
    check_eq(differing_pixels(frame.rgba, black.rgba), 64000,
             tag + "DEMOTION vs black differs on all 64000");
    check_eq(differing_pixels(disband_frame.rgba, black.rgba), 64000,
             tag + "DISBAND vs black differs on all 64000");

    // The palette of both pictures is the global `GAMEPAL` on every index they
    // use (they differ only in the unused index 0), so a caller that colours
    // the frame from GAMEPAL gets the same picture — checked, not assumed.
    std::string palette_error;
    const std::optional<std::vector<std::uint8_t>> gamepal =
        gm::installed_palette(game, palette_error);
    if (gamepal && gamepal->size() == gm::kHudLutPaletteBytes) {
        long long palette_mismatch = 0;
        for (int y = 0; y < 200; ++y) {
            for (int x = 0; x < 320; ++x) {
                const std::uint8_t index = demotion.index_at(x, 200 - 1 - y);
                const std::size_t base = static_cast<std::size_t>(index) * 4u;
                const std::uint32_t want =
                    (static_cast<std::uint32_t>((*gamepal)[base + 0]) << 24) |
                    (static_cast<std::uint32_t>((*gamepal)[base + 1]) << 16) |
                    (static_cast<std::uint32_t>((*gamepal)[base + 2]) << 8) | 0xFFu;
                palette_mismatch += at(frame, x, y) == want ? 0 : 1;
            }
        }
        check_eq(palette_mismatch, 0,
                 tag + "DEMOTION pixels equal GAMEPAL colours (index 0 unused)");
        const long long asset_vs_gamepal = [&] {
            long long same = 0;
            for (std::size_t i = 1; i < 256; ++i) {
                const std::size_t base = i * 4u;
                const std::uint32_t asset = demotion.palette[i] & 0xFFFFFF00u;
                const std::uint32_t global =
                    (static_cast<std::uint32_t>((*gamepal)[base + 0]) << 24) |
                    (static_cast<std::uint32_t>((*gamepal)[base + 1]) << 16) |
                    (static_cast<std::uint32_t>((*gamepal)[base + 2]) << 8);
                same += asset == global ? 1 : 0;
            }
            return same;
        }();
        check_eq(asset_vs_gamepal, 255,
                 tag + "asset palette equals GAMEPAL on indices 1..255");
    } else {
        std::cout << "  frame: SKIP GAMEPAL cross-check (" << palette_error << ")\n";
    }

    std::printf("  ingame_menu frame: rows=%d ink=%d missing=%d arrows=%d, "
                "DEMOTION %s, DISBAND %s, black %s\n",
                frame.stats.rows, frame.stats.ink_pixels, frame.stats.missing_glyphs,
                frame.stats.arrows, box_text(demotion_box).c_str(),
                box_text(disband_box).c_str(), box_text(ink_box(black)).c_str());
    std::printf("  ingame_menu diffs: DEMOTION vs DISBAND %lld/64000, "
                "DEMOTION vs black 64000, DISBAND vs black 64000\n",
                demotion_vs_disband);
}

// --- cursor / items ----------------------------------------------------------

void test_cursor(const fs::path& game) {
    const std::string tag = "cursor: ";
    const auto frame_with = [&](int cursor) {
        gm::IngameMenuRenderOptions options;
        options.cursor = cursor;
        return run(game, options);
    };
    const Frame none = frame_with(-1);
    const Frame first = frame_with(0);
    const Frame third = frame_with(3);
    check(none.ok && first.ok && third.ok, tag + "frames render");
    if (!none.ok || !first.ok || !third.ok) {
        return;
    }
    check_eq(differing_pixels(none.rgba, first.rgba), 0,
             tag + "cursor -1 vs 0: no pixel changes");
    check_eq(differing_pixels(first.rgba, third.rgba), 0,
             tag + "cursor 0 vs 3: no pixel changes");
    check_eq(first.stats.ink_pixels, none.stats.ink_pixels,
             tag + "ink does not depend on the cursor");
    std::printf("  ingame_menu cursor: -1/0/3 -> 0 differing pixels "
                "(no item list, 0x485dd0 is never called in state 9)\n");
}

// --- fade LUT ----------------------------------------------------------------

void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    std::string palette_error;
    const std::optional<std::vector<std::uint8_t>> palette =
        gm::installed_palette(game, palette_error);
    if (!palette || palette->size() != gm::kHudLutPaletteBytes) {
        std::cout << "  fade LUT: SKIP (no 1024-byte GAMEPAL: " << palette_error << ")\n";
        return;
    }
    gm::HudLut lut;
    std::string error;
    if (!gm::build_hud_lut(*palette, lut, &error)) {
        check(false, tag + "build_hud_lut: " + error);
        return;
    }
    std::printf("  fade LUT: 16x256 built in memory from FRONTEND/GAMEPAL, no dump\n");

    const Asset asset = load_asset(game, "DEMOTION");
    check(asset.ok, tag + "DEMOTION decoded for the independent fade prediction");
    if (!asset.ok) {
        return;
    }

    const auto frame_at = [&](bool fade, int level) {
        gm::IngameMenuRenderOptions options;
        options.fade_lut = fade;
        options.fade_lut_level = level;
        if (fade) {
            options.fade_lut_table = &lut;
        }
        return run(game, options);
    };

    const Frame plain = frame_at(false, gm::kHudLutIdentityLevel);
    check(plain.ok, tag + "unfaded frame: " + plain.error);
    if (!plain.ok) {
        return;
    }
    const Frame identity = frame_at(true, gm::kHudLutIdentityLevel);
    check(identity.ok, tag + "level 11 frame: " + identity.error);
    if (!identity.ok) {
        return;
    }
    check_eq(differing_pixels(identity.rgba, plain.rgba), 0,
             tag + "level 11 is byte-identical to no fade");
    check_eq(identity.stats.ink_pixels, plain.stats.ink_pixels, tag + "level 11 ink unchanged");

    // Independent prediction of the diff count of each level: map the index of
    // every pixel through the same table and compare the resulting colours.
    const auto predicted = [&](int level) {
        long long count = 0;
        for (int y = 0; y < 200; ++y) {
            for (int x = 0; x < 320; ++x) {
                const std::uint8_t index = asset.index_at(x, 200 - 1 - y);
                const std::uint8_t mapped = gm::map(lut, level, index);
                count += asset.colour(mapped) == asset.colour(index) ? 0 : 1;
            }
        }
        return count;
    };

    long long previous_diffs[4] = {0, 0, 0, 0};
    std::vector<Frame> levels;
    for (const int level : {12, 13, 14, 15}) {
        const Frame faded = frame_at(true, level);
        check(faded.ok, tag + "level " + std::to_string(level) + ": " + faded.error);
        if (!faded.ok) {
            return;
        }
        const long long diffs = differing_pixels(faded.rgba, plain.rgba);
        check(diffs > 0, tag + "level " + std::to_string(level) + " differs from no fade");
        const long long want = predicted(level);
        check_eq(diffs, want,
                 tag + "level " + std::to_string(level) +
                     " matches the independent index prediction");
        previous_diffs[levels.size()] = diffs;
        for (std::size_t i = 0; i < levels.size(); ++i) {
            check(differing_pixels(levels[i].rgba, faded.rgba) > 0,
                  tag + "level " + std::to_string(level) + " differs from every earlier level");
        }
        levels.push_back(faded);
    }
    std::printf("  ingame_menu fade: 11 -> 0/64000, 12 -> %lld, 13 -> %lld, "
                "14 -> %lld, 15 -> %lld\n",
                previous_diffs[0], previous_diffs[1], previous_diffs[2],
                previous_diffs[3]);

    // A dump path works as well as the pre-loaded table.
    const fs::path root = temp_root();
    std::error_code ec;
    fs::create_directories(root, ec);
    const fs::path dump = root / "hud_lut.bin";
    std::vector<std::uint8_t> bytes;
    bytes.reserve(gm::kHudLutBytes);
    for (int level = 0; level < gm::kHudLutLevels; ++level) {
        for (int i = 0; i < gm::kHudLutEntries; ++i) {
            bytes.push_back(gm::map(lut, level, static_cast<std::uint8_t>(i)));
        }
    }
    check(write_bytes(dump, bytes), tag + "dump written");
    gm::IngameMenuRenderOptions from_dump;
    from_dump.fade_lut = true;
    from_dump.fade_lut_level = 13;
    from_dump.fade_lut_dump = dump.string();
    const Frame dump_frame = run(game, from_dump);
    check(dump_frame.ok, tag + "frame from a dump: " + dump_frame.error);
    if (dump_frame.ok && levels.size() > 1) {
        check_eq(differing_pixels(dump_frame.rgba, levels[1].rgba), 0,
                 tag + "dump level 13 equals the pre-loaded table frame");
    }
    fs::remove_all(root, ec);
}

// --- negatives ---------------------------------------------------------------

void test_negatives(const fs::path& game) {
    const std::string root_tag = "negatives: ";
    std::error_code ec;

    // 1. No game directory at all.
    check_refused(fs::path{}, gm::IngameMenuRenderOptions{},
                  "empty game path", "no game directory");

    if (!game.empty()) {
        const std::optional<std::vector<std::uint8_t>> demotion =
            installed_entry(game, "DEMOTION");
        const std::optional<std::vector<std::uint8_t>> plate =
            installed_entry(game, "PLATE");
        check(demotion.has_value() && plate.has_value(),
              root_tag + "installed DEMOTION and PLATE read");
        if (demotion && plate) {
            const fs::path root = temp_root();
            fs::remove_all(root, ec);

            // 2. No FRONTEND/BMP.IDX.
            fs::create_directories(root / "game-no-archive" / "FRONTEND", ec);
            check_refused(root / "game-no-archive", gm::IngameMenuRenderOptions{},
                          "no BMP.IDX", "BMP.IDX");

            // 3. An archive without DEMOTION.
            check(write_archive(root / "game-no-demotion", {"PLATE"}, {*plate}),
                  root_tag + "archive without DEMOTION written");
            check_refused(root / "game-no-demotion", gm::IngameMenuRenderOptions{},
                          "archive without DEMOTION", "DEMOTION");

            // 4. A DEMOTION of the wrong size (the 196x131 PLATE renamed).
            check(write_archive(root / "game-wrong-size", {"DEMOTION"}, {*plate}),
                  root_tag + "wrong-size DEMOTION written");
            check_refused(root / "game-wrong-size", gm::IngameMenuRenderOptions{},
                          "196x131 DEMOTION", "expected 320x200");

            // 5. DISBAND requested from an archive that only has DEMOTION.
            check(write_archive(root / "game-only-demotion", {"DEMOTION"}, {*demotion}),
                  root_tag + "DEMOTION-only archive written");
            gm::IngameMenuRenderOptions disband;
            disband.background = "DISBAND";
            check_refused(root / "game-only-demotion", disband, "missing DISBAND",
                          "DISBAND");

            // 6. The same minimal archive (only DEMOTION, no font/plate/slider)
            //    must render: this screen needs nothing else.
            const Frame minimal = run(root / "game-only-demotion",
                                      gm::IngameMenuRenderOptions{});
            check(minimal.ok,
                  root_tag + "DEMOTION-only archive renders: " + minimal.error);
            if (minimal.ok) {
                check_eq(minimal.stats.rows, 0, root_tag + "minimal archive rows 0");
                check_eq(minimal.stats.ink_pixels, 0, root_tag + "minimal archive ink 0");
                check_eq(black_pixels(minimal), 0,
                         root_tag + "minimal archive has the picture");
                // The entry name is matched case-insensitively, like the
                // loader's `frontend\NAME.BMP` path.
                gm::IngameMenuRenderOptions lower;
                lower.background = "demotion";
                const Frame lower_frame = run(root / "game-only-demotion", lower);
                check(lower_frame.ok, root_tag + "lower-case name renders");
                if (lower_frame.ok) {
                    check_eq(differing_pixels(lower_frame.rgba, minimal.rgba), 0,
                             root_tag + "lower-case name gives the same frame");
                }
            }
            fs::remove_all(root, ec);
        }
    }

    // 7. The exact fade needs a table or a dump.
    gm::IngameMenuRenderOptions no_lut;
    no_lut.fade_lut = true;
    check_refused(game, no_lut, "fade_lut without a table or dump", "fade_lut_table");

    gm::IngameMenuRenderOptions bad_dump;
    bad_dump.fade_lut = true;
    bad_dump.fade_lut_dump = (temp_root() / "no-such-dump.bin").string();
    check_refused(game, bad_dump, "missing fade dump", "hud_lut");

    // 8. The shipped install renders and is deterministic.
    if (!game.empty()) {
        const Frame first = run(game, gm::IngameMenuRenderOptions{});
        const Frame second = run(game, gm::IngameMenuRenderOptions{});
        check(first.ok && second.ok, root_tag + "shipped install renders");
        if (first.ok && second.ok) {
            check_eq(differing_pixels(first.rgba, second.rgba), 0,
                     root_tag + "two renders are byte-identical");
            check_eq(first.stats.rows, second.stats.rows,
                     root_tag + "stats are deterministic");
            std::printf("  ingame_menu negatives: rows=%d ink=%d missing=%d arrows=%d\n",
                        first.stats.rows, first.stats.ink_pixels,
                        first.stats.missing_glyphs, first.stats.arrows);
        }
    }
}

} // namespace

int main() {
    const char* env = std::getenv("GUNMETAL_DIR");
    std::vector<fs::path> roots;
    if (env != nullptr && *env != '\0') {
        roots.emplace_back(env);
    }
    roots.emplace_back("/mnt/d/games/Gunmetal 3dfx");
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        roots.emplace_back(fs::path(home) / ".cache" / "gunmetal-re" / "game");
    }
    fs::path game;
    std::error_code ec;
    for (const fs::path& root : roots) {
        if (fs::exists(root / "FRONTEND" / "BMP.IDX", ec)) {
            game = root;
            break;
        }
    }

    test_model();
    if (game.empty()) {
        std::cout << "ingame_menu_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    test_frames(game);
    test_cursor(game);
    test_negatives(game);
    test_fade_lut(game);
    if (failures != 0) {
        std::cerr << "ingame_menu_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "ingame_menu_render: ok (" << checks << " checks)\n";
    return 0;
}
