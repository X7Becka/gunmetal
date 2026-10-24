#pragma once

// Mission briefings: which `SML/brief/<level>.SME` belongs to which level and
// what its pages, lines and text are.
//
// What this module is
// -------------------
// The shipped briefings are ordinary `.sme` documents (`docs/UI.md` §2-§3,
// corpus §1).  The pager ([`ui/sml_pager`](SML_PAGER.md)) already knows how to
// decode one and cut it into 267x167 pages, but nothing maps "mission N" to
// "file X" and nothing exposes the result as data.  This module is that thin
// index:
//
//   * `briefing_levels(game)` enumerates the stems of `SML/brief/*.sme`
//     (case-insensitively, like the engine loader at `0x497310`);
//   * `load_briefing(game, level, out, error)` finds the file, decodes it
//     (`parse_sml`, i.e. `decode_best` + plain fallback), paginates it with
//     `SmlPager` and flattens every page into lines of text plus counts.
//
// It does not render, does not touch the menu/runtime/end-of-level screen and
// does not read `GUNMETAL.HI` (the player's best time lives there) or the VM's
// par time (`SET_LEVEL_BEST_TIME` 93 -> `0x80d88c`); neither is part of a
// `.sme`.  Fonts come from `FRONTEND/BMP.IDX` through `SmlPager`, exactly like
// the pager; the fonts, page box and wrapping are therefore the pager's, not a
// second model.
//
// Level names
// -----------
// The engine builds `SML\BRIEF\LEVEL%ld.SML` (`0x4bde48`) and a variant with a
// letter suffix (`0x4bde64`) and then prefers the `.SME` sibling (`0x497310`).
// The shipment has **no** `LEVEL0.SME`: mission 0 ships as `LEVEL0A.SME`
// (`SML/brief/dir.txt` agrees).  Names are returned and stored lowercased, so
// `LEVEL1`, `level1` and `Level1` all denote the same briefing; the `source`
// field keeps the spelling that was actually read.
//
// Text and encodings
// ------------------
// `.sme` is a positional cipher over latin-1 bytes (`docs/UI.md` §3);
// `parse_sml` decodes it and repairs the shipped `brief/level17.sme` CRLF
// anomaly (`decode_best` note `newline-repair`).  All 25 shipped briefings
// decode to plain ASCII (bytes 0x20..0x7e), so the line text is UTF-8-safe as
// well; the comparison in the test is done on those bytes, not on a guessed
// translation.  `plain_text()` joins the per-page lines with `'\n'`.
//
// Glyphs are **laid-out characters**: every byte of a `LayoutRun::text` on the
// page (spaces included, `<k>` already expanded to `Soldier`, `<s>` to a
// space).  When the atlas has a glyph for every character this is exactly
// `PageRenderStats::glyphs` (`docs/PAGER_RENDER.md` §2.3); the test checks
// that equality on three briefings.  It is not an atlas lookup here, so the
// module still works for a metrics-only install.
//
// Invariants
// ----------
// * `load_briefing` clears `out` before anything else: on failure
//   `page_count() == 0`, `line_count() == 0`, `plain_text().empty()`.
// * A failed call always fills `*error` (when `error != nullptr`) with a
//   `"briefing: ..."` text and never throws/aborts: missing game directory,
//   missing `SML/brief`, empty level, a level that is a path, a non-`.sme`
//   name, a missing/empty file, undecodable bytes, a pager/layout failure, or
//   a document without a single laid-out line.
// * `briefing_levels` returns sorted, lowercased, unique stems and an empty
//   vector when the installation/`SML/brief` is absent (`error` is not part of
//   its signature).
// * Both calls are read-only, deterministic and independent of the current
//   working directory (paths are built from `game`).

#include <filesystem>
#include <string>
#include <vector>

namespace gm {

/// One 267x167 pager page of a briefing.
struct BriefingPage {
    int index = 0;                 ///< 0-based page number (`LayoutPage::index`)
    int lines = 0;                 ///< visual lines (`LayoutPage::lines`)
    int glyphs = 0;                ///< laid-out characters of the page's runs
    std::vector<std::string> text; ///< one entry per visual line, trimmed
};

/// A decoded, paginated briefing of one level.
struct Briefing {
    std::string source;  ///< file that was read (spelling as on disk)
    std::string level;   ///< lowercased stem (`level1`, `level0a`, ...)
    std::vector<BriefingPage> pages;

    /// Number of pages (0 when the briefing was not loaded).
    int page_count() const;
    /// Sum of `BriefingPage::lines` over all pages.
    int line_count() const;
    /// Every page's lines joined with `'\n'` (for comparison / previews).
    std::string plain_text() const;
};

/// Load `game/SML/brief/<level>.SME` case-insensitively and paginate it.
///
/// `level` is a file *name*: a stem (`level1`), or the same with a `.sme`
/// suffix in any case.  A path (with `/`, `\`, `.` or `..`), an empty name, a
/// non-`.sme` extension, a missing file or an empty/undecodable document are
/// refusals with a text.
///
/// On success `out.source` is the resolved path, `out.level` the lowercased
/// stem and `out.pages` the pager pages with their text.
bool load_briefing(const std::filesystem::path& game, const std::string& level,
                   Briefing& out, std::string* error = nullptr);

/// Stems of every `SML/brief/*.sme` of the installation, lowercased, sorted
/// and unique.  Empty when the installation or `SML/brief` is missing.
std::vector<std::string> briefing_levels(const std::filesystem::path& game);

} // namespace gm
