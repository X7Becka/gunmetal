// Briefing frames: one page of `SML/brief/<level>.SME` painted into the
// 320x200 RGBA buffer (`ui/briefing_render`, docs/BRIEFING_RENDER.md).
//
// `ui/briefing` finds and paginates the document, `ui/pager_render` paints a
// page; this layer only chooses the page.  The test therefore pins:
//
//   * level1 page 1: 2 pages in the briefing, 15 lines / 719 glyphs on the
//     page, no missing glyph, a non-empty and non-solid 320x200 frame
//     (`stats.ink_pixels` is the golden of this implementation);
//   * parity: the very same page through `SmlPager` + `render_page` (the
//     building blocks, no briefing layer) is byte for byte identical, and the
//     two stat sets agree — the glue cannot drift from the pager silently;
//   * level0a pages 1/2/3 are different pictures (the exact number of
//     differing pixels is printed and must be > 0), `--background` replaces
//     the black canvas with 64000 `HELPSCRN` pixels without touching the ink,
//     fade 11 is the identity and fade 0 a black frame;
//   * the whole corpus: every page of all 25 briefings renders with
//     `missing_glyphs == 0`, `ink > 0`, 320x200, and the sums are the
//     docs/BRIEFING.md totals 58 pages / 727 lines / 33284 glyphs;
//   * the exact fade of the pager (`PageRenderOptions::fade_lut`, forwarded by
//     `BriefingRenderOptions`): level 11 is byte-identical to the frame without
//     fade, level 0 leaves only the out-of-ramp slots non-black (the RGBA model
//     blackens all 64000), 12..15 are four different frames, the LUT wins over
//     `fade`, and a LUT without a table/dump is a `briefing_render:` refusal;
//   * two renders of one page are byte identical (determinism);
//   * negatives: `level0` has no file, pages 0 and 99 are out of range, an
//     empty/missing game, a game without `SML/brief`, a `.sme` without
//     `FRONTEND/BMP.IDX`, a broken and an empty `.sme` are texts prefixed
//     `briefing_render:`, with the buffer cleared and the stats zeroed.
//
// The installation is only read.  When it is missing the corpus half prints a
// skip line and the process succeeds, like the other corpus tests.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "render/hud_lut.h"
#include "ui/briefing.h"
#include "ui/briefing_render.h"
#include "ui/pager_render.h"
#include "ui/sml_pager.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

void check_eq(long long got, long long want, const std::string& what) {
    if (got != want) {
        ++failures;
        std::cerr << "FAIL: " << what << " (got " << got << ", want " << want << ")\n";
    }
}

bool starts_with(const std::string& text, std::string_view prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool write_file(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

/// Number of positions where two frames differ; -1 when the sizes differ.
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

/// Pixels of a frame that are the opaque black canvas colour
/// (`gm::kPagerBackground`, `0x000000FF`).
long long black_pixels(const std::vector<std::uint32_t>& rgba) {
    long long count = 0;
    for (const std::uint32_t pixel : rgba) {
        if (pixel == gm::kPagerBackground) {
            ++count;
        }
    }
    return count;
}

/// The dump of the exact tables: `$GM_HUD_LUT`, else
/// `~/.cache/gunmetal-re/hud_lut.bin` (same rule as `test_pager_render.cpp`).
fs::path find_hud_lut_dump() {
    if (const char* env = std::getenv("GM_HUD_LUT"); env != nullptr && *env != '\0') {
        return env;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return fs::path(home) / ".cache" / "gunmetal-re" / "hud_lut.bin";
    }
    return {};
}

/// One frame of a briefing page, or a refusal with its text.
struct Frame {
    bool ok = false;
    std::string error;
    std::vector<std::uint32_t> rgba;
    int width = 0;
    int height = 0;
    gm::BriefingRenderStats stats;
};

Frame render(const fs::path& game, const std::string& level,
             const gm::BriefingRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_briefing(game, level, options, frame.rgba, frame.width,
                                   frame.height, &frame.stats, &frame.error);
    return frame;
}

/// Every refusal: false, `"briefing_render: ..."` text, cleared buffer,
/// zeroed stats, no crash.
void check_refused(const fs::path& game, const std::string& level,
                   const gm::BriefingRenderOptions& options, const std::string& what,
                   std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu);  // must be cleared
    int width = 320;
    int height = 200;
    gm::BriefingRenderStats stats;
    stats.pages = 7;
    stats.lines = 7;
    stats.glyphs = 7;
    stats.missing_glyphs = 7;
    stats.ink_pixels = 7;
    stats.background_pixels = 7;
    std::string error;
    const bool ok =
        gm::render_briefing(game, level, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "briefing_render:"),
          what + ": error prefix (got \"" + error + "\")");
    check(error.find(expected) != std::string::npos,
          what + ": error mentions \"" + std::string(expected) + "\" (got \"" + error +
              "\")");
    check(rgba.empty(), what + ": buffer cleared");
    check_eq(width, 0, what + ": width cleared");
    check_eq(height, 0, what + ": height cleared");
    check_eq(stats.pages, 0, what + ": pages cleared");
    check_eq(stats.lines, 0, what + ": lines cleared");
    check_eq(stats.glyphs, 0, what + ": glyphs cleared");
    check_eq(stats.missing_glyphs, 0, what + ": missing cleared");
    check_eq(stats.ink_pixels, 0, what + ": ink cleared");
    check_eq(stats.background_pixels, 0, what + ": background cleared");
}

// Golden `ink_pixels` of level1 page 1 (printed by the test, docs §4).
constexpr long long kLevel1Ink = 7214;

void test_synthetic_negatives() {
    const fs::path root = fs::temp_directory_path() / "gm_briefing_render_negatives";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path game = root / "game";
    const fs::path brief = game / "SML" / "brief";
    fs::create_directories(brief, ec);
    check(!ec, "synthetic: temp tree created");
    if (ec) {
        return;
    }
    fs::create_directories(root / "game-without-brief", ec);

    gm::BriefingRenderOptions options;
    options.page = 1;

    // A valid document but no `FRONTEND/BMP.IDX`: the pager cannot build the
    // atlases, so the frame is refused with the font reason.
    check(write_file(brief / "level96.sme", "<d><r>HELLO</d>"),
          "synthetic: readable briefing written");
    check_refused(game, "level96", options, "synthetic: no BMP.IDX", "BMP.IDX");

    // Undecodable bytes fail before any atlas is needed.
    check(write_file(brief / "level99.sme", "this is not sml markup"),
          "synthetic: broken file written");
    check_refused(game, "level99", options, "synthetic: broken bytes", "sml: no tags");

    // An empty file is refused by the index before parsing.
    check(write_file(brief / "level98.sme", ""), "synthetic: empty file written");
    check_refused(game, "level98", options, "synthetic: empty file", "empty file");

    // Not a directory at all; no `SML/brief`; a path instead of a level.
    check(write_file(root / "not-a-directory.txt", "x"), "synthetic: plain file written");
    check_refused(root / "not-a-directory.txt", "level1", options,
                  "synthetic: game is a file", "game directory not found");
    check_refused(root / "no-game", "level1", options, "synthetic: missing game",
                  "game directory not found");
    check_refused(root / "game-without-brief", "level1", options,
                  "synthetic: no SML/brief", "no SML/brief");
    check_refused(game, "SML/brief/level96.sme", options, "synthetic: level is a path",
                  "not a path");

    fs::remove_all(root, ec);
}

void test_level1(const fs::path& game) {
    gm::BriefingRenderOptions options;
    options.page = 1;
    const Frame frame = render(game, "level1", options);
    check(frame.ok, "level1 page 1 renders: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check(frame.error.empty(), "level1 page 1: no error on success");
    check_eq(frame.width, 320, "level1 page 1: canvas width");
    check_eq(frame.height, 200, "level1 page 1: canvas height");
    check_eq(static_cast<long long>(frame.rgba.size()), 320 * 200,
             "level1 page 1: buffer size");
    check_eq(frame.stats.pages, 2, "level1 page 1: pages of the briefing");
    check_eq(frame.stats.lines, 15, "level1 page 1: lines");
    check_eq(frame.stats.glyphs, 719, "level1 page 1: glyphs");
    check_eq(frame.stats.missing_glyphs, 0, "level1 page 1: no missing glyphs");
    check_eq(frame.stats.ink_pixels, kLevel1Ink, "level1 page 1: ink pixels");
    check(frame.stats.ink_pixels > 0 && frame.stats.ink_pixels < 320 * 200,
          "level1 page 1: picture is neither empty nor a solid fill");
    check_eq(frame.stats.background_pixels, 0, "level1 page 1: black background");
    std::cout << "level1 page 1/2: lines=" << frame.stats.lines
              << " glyphs=" << frame.stats.glyphs
              << " missing=" << frame.stats.missing_glyphs
              << " ink=" << frame.stats.ink_pixels << "\n";

    // Parity with the building blocks: same file, same pager call, same
    // renderer, no briefing layer in between.
    gm::Briefing briefing;
    std::string error;
    check(gm::load_briefing(game, "level1", briefing, &error),
          "level1 parity: briefing loads (" + error + ")");
    gm::SmlPager pager(game);
    check(pager.open(briefing.source, &error), "level1 parity: pager opens (" + error + ")");
    check_eq(pager.page(), 0, "level1 parity: pager on the first page");
    std::vector<std::uint32_t> direct;
    int direct_width = 0;
    int direct_height = 0;
    gm::PageRenderStats direct_stats;
    const gm::PageRenderOptions direct_options{};
    check(gm::render_page(pager, game, direct, direct_width, direct_height,
                          direct_options, &direct_stats, &error),
          "level1 parity: direct render (" + error + ")");
    const long long diff = differing_pixels(frame.rgba, direct);
    check_eq(diff, 0, "level1 parity: differing pixels against render_page");
    check_eq(direct_width, frame.width, "level1 parity: width");
    check_eq(direct_height, frame.height, "level1 parity: height");
    check_eq(direct_stats.lines, frame.stats.lines, "level1 parity: lines");
    check_eq(direct_stats.glyphs, frame.stats.glyphs, "level1 parity: glyphs");
    check_eq(direct_stats.ink_pixels, frame.stats.ink_pixels, "level1 parity: ink");
    std::cout << "level1 page 1 parity: " << diff
              << " differing pixels against SmlPager+render_page ("
              << direct_stats.glyphs << " glyphs, ink " << direct_stats.ink_pixels
              << ")\n";

    // Determinism: a second call is byte for byte the same frame.
    const Frame again = render(game, "level1", options);
    check(again.ok, "level1 page 1: second render");
    check(again.rgba == frame.rgba, "level1 page 1: deterministic buffer");
    check_eq(again.stats.ink_pixels, frame.stats.ink_pixels,
             "level1 page 1: deterministic ink");
    check_eq(again.stats.glyphs, frame.stats.glyphs,
             "level1 page 1: deterministic glyphs");
    std::cout << "level1 page 1 rerun: "
              << (again.rgba == frame.rgba ? "byte-identical" : "DIFFERENT") << "\n";
}

void test_level0a(const fs::path& game) {
    // Page 1 is the cheat/credits screen of the shipment; its pinned
    // lines/glyphs come from docs/BRIEFING.md §4.
    const int want_lines[3] = {10, 6, 6};
    const int want_glyphs[3] = {180, 87, 91};
    const int want_ink[3] = {2214, 1334, 1370};
    std::vector<Frame> pages;
    for (int page = 1; page <= 3; ++page) {
        gm::BriefingRenderOptions options;
        options.page = page;
        Frame frame = render(game, "level0a", options);
        check(frame.ok, "level0a page " + std::to_string(page) + " renders: " +
                            frame.error);
        if (!frame.ok) {
            return;
        }
        check_eq(frame.stats.pages, 5, "level0a page " + std::to_string(page) + ": pages");
        check_eq(frame.stats.lines, want_lines[page - 1],
                 "level0a page " + std::to_string(page) + ": lines");
        check_eq(frame.stats.glyphs, want_glyphs[page - 1],
                 "level0a page " + std::to_string(page) + ": glyphs");
        check_eq(frame.stats.missing_glyphs, 0,
                 "level0a page " + std::to_string(page) + ": no missing glyphs");
        check_eq(frame.stats.ink_pixels, want_ink[page - 1],
                 "level0a page " + std::to_string(page) + ": ink");
        std::cout << "  level0a page " << page << "/5: lines=" << frame.stats.lines
                  << " glyphs=" << frame.stats.glyphs
                  << " missing=" << frame.stats.missing_glyphs
                  << " ink=" << frame.stats.ink_pixels << "\n";
        pages.push_back(std::move(frame));
    }
    const long long diff12 = differing_pixels(pages[0].rgba, pages[1].rgba);
    const long long diff23 = differing_pixels(pages[1].rgba, pages[2].rgba);
    const long long diff13 = differing_pixels(pages[0].rgba, pages[2].rgba);
    check(diff12 > 0, "level0a: page 1 and page 2 differ");
    check(diff23 > 0, "level0a: page 2 and page 3 differ");
    check(diff13 > 0, "level0a: page 1 and page 3 differ");
    std::cout << "level0a page diffs: 1-2=" << diff12 << ", 2-3=" << diff23
              << ", 1-3=" << diff13 << " differing pixels\n";

    // `--background` is the pager layer: 64000 HELPSCRN pixels and a different
    // picture, but exactly the same text ink.
    gm::BriefingRenderOptions background;
    background.page = 1;
    background.background = true;
    const Frame with_background = render(game, "level0a", background);
    check(with_background.ok, "level0a page 1 --background renders: " +
                                  with_background.error);
    if (with_background.ok) {
        check_eq(with_background.stats.background_pixels, 320 * 200,
                 "level0a page 1 --background: non-black screen pixels");
        check_eq(with_background.stats.ink_pixels, pages[0].stats.ink_pixels,
                 "level0a page 1 --background: ink unchanged");
        const long long changed = differing_pixels(pages[0].rgba, with_background.rgba);
        check(changed > 0, "level0a page 1 --background: the frame changes");
        std::cout << "level0a page 1 --background: background="
                  << with_background.stats.background_pixels << " pixels, " << changed
                  << " pixels changed, ink " << with_background.stats.ink_pixels << "\n";
    }

    // Fade is the pager curve: 11 is the identity, 0 a black frame.
    gm::BriefingRenderOptions fade;
    fade.page = 1;
    fade.fade = true;
    fade.fade_level = 11;
    const Frame identity = render(game, "level0a", fade);
    check(identity.ok, "level0a page 1 fade 11 renders: " + identity.error);
    if (identity.ok) {
        check_eq(differing_pixels(pages[0].rgba, identity.rgba), 0,
                 "level0a page 1 fade 11: identity");
    }
    fade.fade_level = 0;
    const Frame black = render(game, "level0a", fade);
    check(black.ok, "level0a page 1 fade 0 renders: " + black.error);
    if (black.ok) {
        long long black_pixels = 0;
        for (const std::uint32_t pixel : black.rgba) {
            if (pixel == gm::kPagerBackground) {
                ++black_pixels;
            }
        }
        check_eq(black_pixels, 320 * 200, "level0a page 1 fade 0: black frame");
        std::cout << "level0a page 1 fade: 11 = identity, 0 = black ("
                  << black_pixels << "/" << 320 * 200 << " pixels)\n";
    }
}

void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT briefing: ";
    constexpr long long kCanvas = 320LL * 200LL;

    // The frame the exact path has to reproduce at level 11: page 1 of level1
    // with the HELPSCRN background, no fade at all.
    gm::BriefingRenderOptions plain_options;
    plain_options.page = 1;
    plain_options.background = true;
    const Frame plain = render(game, "level1", plain_options);
    check(plain.ok, tag + "level1 page 1 --background renders: " + plain.error);
    if (!plain.ok) {
        return;
    }
    check_eq(plain.stats.background_pixels, kCanvas,
             tag + "plain frame carries HELPSCRN");

    // A LUT without a table and without a dump is a refusal, not a silently
    // unfaded frame.  It needs no dump, so it runs on every installation.
    {
        gm::BriefingRenderOptions missing;
        missing.page = 1;
        missing.background = true;
        missing.fade_lut = true;
        missing.fade_lut_level = gm::kHudLutIdentityLevel;
        check_refused(game, "level1", missing, "fade LUT: no table and no dump",
                      "fade LUT needs a table (fade_lut_table) or a dump (fade_lut_dump)");

        gm::BriefingRenderOptions bad_dump = missing;
        bad_dump.fade_lut_dump =
            (fs::temp_directory_path() / "gm_briefing_no_such_hud_lut.bin").string();
        check_refused(game, "level1", bad_dump, "fade LUT: unreadable dump", "hud_lut");
    }

    const fs::path dump = find_hud_lut_dump();
    std::error_code ec;
    if (dump.empty() || !fs::is_regular_file(dump, ec) || ec) {
        std::cout << "  fade LUT briefing: SKIP real dump (not found; set GM_HUD_LUT or "
                     "run tools/win/dump_lut.lua) path="
                  << dump.string() << "\n";
        return;
    }

    gm::HudLut lut;
    std::string error;
    if (!gm::load_hud_lut(dump, lut, &error)) {
        check(false, tag + "dump: " + error);
        return;
    }

    const auto render_lut = [&](int level, const gm::HudLut* table,
                                const std::string& dump_path, const std::string& what) {
        gm::BriefingRenderOptions options;
        options.page = 1;
        options.background = true;
        options.fade_lut = true;
        options.fade_lut_level = level;
        options.fade_lut_table = table;
        options.fade_lut_dump = dump_path;
        Frame frame = render(game, "level1", options);
        check(frame.ok, tag + what + " renders: " + frame.error);
        return frame;
    };

    // 1. Level 11 is the identity: the LUT frame is byte for byte the plain
    // frame, through a pre-loaded table and through the dump file.
    const Frame exact11 = render_lut(gm::kHudLutIdentityLevel, &lut, "",
                                     "level 11 (table)");
    if (!exact11.ok) {
        return;
    }
    const long long diff11 = differing_pixels(exact11.rgba, plain.rgba);
    check_eq(diff11, 0, tag + "level 11 is byte-identical to no fade");
    check_eq(exact11.stats.ink_pixels, plain.stats.ink_pixels,
             tag + "level 11 ink unchanged");
    check_eq(exact11.stats.background_pixels, plain.stats.background_pixels,
             tag + "level 11 background pixels unchanged");
    const Frame loaded11 = render_lut(gm::kHudLutIdentityLevel, nullptr, dump.string(),
                                      "level 11 (dump)");
    check(loaded11.ok, tag + "dump path renders");
    if (loaded11.ok) {
        check_eq(differing_pixels(loaded11.rgba, plain.rgba), 0,
                 tag + "dump path equals no fade");
        check_eq(differing_pixels(loaded11.rgba, exact11.rgba), 0,
                 tag + "dump path equals a pre-loaded table");
    }

    // The table alone works without any file: same frame, no dump path set.
    check_eq(differing_pixels(exact11.rgba, plain.rgba), 0,
             tag + "in-memory table needs no dump file");

    // 2. The LUT wins over the RGBA model: fade 0 + LUT 11 is the plain frame,
    // not a black one.
    {
        gm::BriefingRenderOptions options;
        options.page = 1;
        options.background = true;
        options.fade = true;
        options.fade_level = 0;
        options.fade_lut = true;
        options.fade_lut_level = gm::kHudLutIdentityLevel;
        options.fade_lut_table = &lut;
        const Frame both = render(game, "level1", options);
        check(both.ok, tag + "fade + LUT renders: " + both.error);
        if (both.ok) {
            check_eq(differing_pixels(both.rgba, plain.rgba), 0,
                     tag + "LUT wins over fade 0 (identity frame)");
            check_eq(black_pixels(both.rgba), black_pixels(plain.rgba),
                     tag + "LUT wins: no model blackening");
        }
    }

    // 3. Level 0: the table keeps the out-of-ramp slots, the model blackens
    // every pixel.
    const Frame exact0 = render_lut(0, &lut, "", "level 0 (table)");
    if (!exact0.ok) {
        return;
    }
    gm::BriefingRenderOptions model0_options;
    model0_options.page = 1;
    model0_options.background = true;
    model0_options.fade = true;
    model0_options.fade_level = 0;
    const Frame model0 = render(game, "level1", model0_options);
    check(model0.ok, tag + "level 0 model renders: " + model0.error);
    if (!model0.ok) {
        return;
    }
    const long long black_lut0 = black_pixels(exact0.rgba);
    const long long black_model0 = black_pixels(model0.rgba);
    const long long diff0_model = differing_pixels(exact0.rgba, model0.rgba);
    check_eq(black_model0, kCanvas, tag + "model level 0 blackens every pixel");
    check(black_lut0 > 0, tag + "level 0 frame is almost black");
    check(black_lut0 < kCanvas, tag + "level 0 keeps the out-of-ramp slots");
    check(diff0_model > 0, tag + "level 0 differs from the model");
    check_eq(exact0.stats.ink_pixels, plain.stats.ink_pixels,
             tag + "level 0 ink unchanged (counted before the table)");
    std::cout << "  fade LUT briefing level  0: black " << black_lut0 << "/" << kCanvas
              << " (model " << black_model0 << "), " << diff0_model
              << " px differ from the model, background pixels "
              << exact0.stats.background_pixels << "\n";

    // 4. Levels 12..15: four different frames, while the model clamps to 11.
    std::vector<Frame> exact(4);
    for (int i = 0; i < 4; ++i) {
        const int level = gm::kHudLutIdentityLevel + 1 + i;
        exact[static_cast<std::size_t>(i)] =
            render_lut(level, &lut, "", "level " + std::to_string(level) + " (table)");
        if (!exact[static_cast<std::size_t>(i)].ok) {
            return;
        }
        const std::size_t at = static_cast<std::size_t>(i);
        check(differing_pixels(exact[at].rgba, plain.rgba) > 0,
              tag + "level " + std::to_string(level) + " changes the frame");
        check_eq(exact[at].stats.ink_pixels, plain.stats.ink_pixels,
                 tag + "level " + std::to_string(level) + " ink unchanged");
    }
    const long long pair12_13 = differing_pixels(exact[0].rgba, exact[1].rgba);
    const long long pair13_14 = differing_pixels(exact[1].rgba, exact[2].rgba);
    const long long pair14_15 = differing_pixels(exact[2].rgba, exact[3].rgba);
    const long long to11_12 = differing_pixels(exact[0].rgba, exact11.rgba);
    const long long to11_13 = differing_pixels(exact[1].rgba, exact11.rgba);
    const long long to11_14 = differing_pixels(exact[2].rgba, exact11.rgba);
    const long long to11_15 = differing_pixels(exact[3].rgba, exact11.rgba);
    check(pair12_13 > 0 && pair13_14 > 0 && pair14_15 > 0,
          tag + "levels 12..15 are four different frames");
    check(to11_12 > 0 && to11_13 > 0 && to11_14 > 0 && to11_15 > 0,
          tag + "each of 12..15 differs from the identity");
    std::cout << "  fade LUT briefing level 12..15: adjacent diffs 12/13 " << pair12_13
              << ", 13/14 " << pair13_14 << ", 14/15 " << pair14_15
              << "; vs level 11: " << to11_12 << "/" << to11_13 << "/" << to11_14
              << "/" << to11_15 << " px\n";

    // The RGBA model still clamps 12 to the identity (unchanged by this slice).
    gm::BriefingRenderOptions model12_options;
    model12_options.page = 1;
    model12_options.background = true;
    model12_options.fade = true;
    model12_options.fade_level = 12;
    const Frame model12 = render(game, "level1", model12_options);
    check(model12.ok, tag + "level 12 model renders: " + model12.error);
    if (model12.ok) {
        check_eq(differing_pixels(model12.rgba, plain.rgba), 0,
                 tag + "the model clamps level 12 to the identity");
        check(differing_pixels(exact[0].rgba, model12.rgba) > 0,
              tag + "level 12 differs from the clamped model");
    }

    std::cout << "  fade LUT briefing level 11: " << diff11
              << " differing pixels against the frame without fade (table and dump "
                 "paths agree)\n";
}

void test_corpus(const fs::path& game) {
    const auto levels = gm::briefing_levels(game);
    std::cout << "briefing_render levels: " << levels.size() << "\n";
    check_eq(static_cast<long long>(levels.size()), 25,
             "corpus: 25 shipped .sme briefings");

    int total_pages = 0;
    int total_lines = 0;
    int total_glyphs = 0;
    int total_missing = 0;
    long long total_ink = 0;
    int min_ink = -1;
    int max_ink = 0;
    for (const std::string& level : levels) {
        const std::string tag = "corpus " + level + ": ";
        gm::BriefingRenderOptions options;
        options.page = 1;
        Frame first_page = render(game, level, options);
        check(first_page.ok, tag + "page 1 renders: " + first_page.error);
        if (!first_page.ok) {
            continue;
        }
        const int pages = first_page.stats.pages;
        check(pages > 0, tag + "has pages");
        total_pages += pages;
        for (int page = 1; page <= pages; ++page) {
            Frame frame = page == 1 ? std::move(first_page) : Frame{};
            if (page != 1) {
                options.page = page;
                frame = render(game, level, options);
            }
            const std::string page_tag = tag + "page " + std::to_string(page) + "/" +
                                         std::to_string(pages) + ": ";
            check(frame.ok, page_tag + "renders: " + frame.error);
            if (!frame.ok) {
                break;
            }
            check(frame.error.empty(), page_tag + "no error on success");
            check_eq(frame.width, 320, page_tag + "width");
            check_eq(frame.height, 200, page_tag + "height");
            check_eq(static_cast<long long>(frame.rgba.size()), 320 * 200,
                     page_tag + "buffer size");
            check_eq(frame.stats.pages, pages, page_tag + "page count");
            check_eq(frame.stats.missing_glyphs, 0, page_tag + "no missing glyphs");
            check(frame.stats.ink_pixels > 0, page_tag + "ink > 0");
            check(frame.stats.ink_pixels < 320 * 200, page_tag + "ink < canvas");
            check_eq(frame.stats.background_pixels, 0,
                     page_tag + "no background without --background");
            total_lines += frame.stats.lines;
            total_glyphs += frame.stats.glyphs;
            total_missing += frame.stats.missing_glyphs;
            total_ink += frame.stats.ink_pixels;
            if (min_ink < 0 || frame.stats.ink_pixels < min_ink) {
                min_ink = frame.stats.ink_pixels;
            }
            if (frame.stats.ink_pixels > max_ink) {
                max_ink = frame.stats.ink_pixels;
            }
        }
    }
    check_eq(total_pages, 58, "corpus: total pages");
    check_eq(total_lines, 727, "corpus: total lines");
    check_eq(total_glyphs, 33284, "corpus: total glyphs");
    check_eq(total_missing, 0, "corpus: no missing glyphs");
    check(total_ink > 0, "corpus: total ink > 0");
    std::cout << "briefing_render corpus: " << levels.size() << " levels, " << total_pages
              << " pages, " << total_lines << " lines, " << total_glyphs
              << " glyphs, missing=" << total_missing << ", ink " << total_ink
              << " (min " << min_ink << ", max " << max_ink << " per page)\n";
}

void test_real_negatives(const fs::path& game) {
    gm::BriefingRenderOptions page_one;
    page_one.page = 1;
    // Mission 0 ships as level0a: there is no LEVEL0.SME (`docs/BRIEFING.md` §1).
    check_refused(game, "level0", page_one, "real: level0 has no file", "file not found");

    gm::BriefingRenderOptions page_zero;
    page_zero.page = 0;
    check_refused(game, "level1", page_zero, "real: page 0", "no page 0 (1..2)");

    gm::BriefingRenderOptions page_99;
    page_99.page = 99;
    check_refused(game, "level1", page_99, "real: page 99", "no page 99 (1..2)");

    check_refused(game, "help", page_one, "real: a non-briefing screen",
                  "file not found");
    check_refused(fs::path{}, "level1", page_one, "real: empty game path",
                  "game directory not found");
    check_refused(game / "SML" / "help.sme", "level1", page_one,
                  "real: game is a file", "game directory not found");
}

} // namespace

int main() {
    test_synthetic_negatives();

    const char* env = std::getenv("GUNMETAL_DIR");
    const fs::path game =
        env != nullptr ? fs::path(env) : fs::path("/mnt/d/games/Gunmetal 3dfx");
    std::error_code ec;
    if (!fs::is_directory(game / "SML" / "brief", ec) ||
        !fs::exists(game / "FRONTEND" / "BMP.IDX", ec)) {
        std::cout << "briefing_render: game not installed, corpus skipped\n";
        return failures == 0 ? 0 : 1;
    }

    test_level1(game);
    test_level0a(game);
    test_fade_lut(game);
    test_corpus(game);
    test_real_negatives(game);

    if (failures != 0) {
        std::cerr << "briefing_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "briefing_render: SML/brief page -> 320x200 RGBA through the pager "
                 "(parity with render_page, 25 briefings, negatives); OK\n";
    return 0;
}
