// HUD binding: SML screen -> film family/name/line, and <I x y a> -> placement.
//
// Golden data (game install, read-only):
//   * the whole 300-file SML corpus: 299 parse (prelevel/dm4.sml is the known
//     missing-</d> anomaly), 72 <I> images on 24 screens, 15 unique BMP names,
//     50 left / 22 right anchors, all x=y=64 (docs/UI.md §5.2);
//   * design screens: 20 WEAPON, 12 AMMO, 4 TANK, 1 SHIELD, 9 EQUIP, 1 TEMP0;
//   * help.sme has no images -> one overlay placement at (14,125).
//
// Synthetic fixtures pin the API without the game: a temporary Film/ tree with
// tiny .sof/.fdx files exercises name/state choice, missing films, line-outside
// and the Default fallback; hand-built documents pin the malformed cases.
//
// Goldens and the proven/hypothesis split: docs/HUD_BIND.md.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_bind.h"
#include "render/hud_player.h"
#include "render/render.h"
#include "res/sml.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

template <typename T, typename U>
void check_eq(const T& got, const U& want, const std::string& what) {
    if (!(got == want)) {
        ++failures;
        std::cerr << "FAIL: " << what << " (got " << got << ", want " << want << ")\n";
    }
}

void check_eq(const std::string& got, const char* want, const std::string& what) {
    check_eq<std::string, std::string>(got, want, what);
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

void write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

std::optional<gm::SmlDocument> parse_plain(const std::string& text) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
    std::string error;
    return gm::parse_sml(std::span<const std::uint8_t>(bytes, text.size()),
                         gm::SmlSource::Plain, error);
}

gm::RenderResult make_target(int width, int height, int fill) {
    gm::RenderResult target;
    target.width = width;
    target.height = height;
    target.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3,
                      static_cast<std::uint8_t>(fill));
    return target;
}

gm::HudFilm synthetic_film(std::size_t tiles, std::uint32_t count, std::uint32_t fps) {
    gm::HudFilm film;
    film.sof.data.assign(tiles * gm::kSofTileBytes, 0);
    film.line = 0;
    film.count = count;
    film.fps = fps;
    film.start_frame = 0;
    film.loop = true;
    return film;
}

std::optional<gm::HudBinding> bind_screen(const std::string& markup, const std::string& screen,
                                   std::string& error) {
    const auto doc = parse_plain(markup);
    if (!doc) {
        error = "synthetic document did not parse";
        return std::nullopt;
    }
    return gm::parse_binding(*doc, screen, error);
}

/// Minimal game tree for resolve_film: only the films the tests ask for.
fs::path make_game_fixture() {
    const fs::path root = fs::temp_directory_path() / "gm_hud_bind_game";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Film", ec);

    const std::vector<std::uint8_t> tiles3(gm::kSofTileBytes * 3, 0);
    const std::vector<std::uint8_t> tiles2(gm::kSofTileBytes * 2, 0);
    write_file(root / "Film" / "WEAPON4.sof", tiles3);
    write_file(root / "Film" / "WEAPON30.sof", tiles2); // file, but no index line 30
    write_file(root / "Film" / "AMMO3.sof", tiles3);
    write_file(root / "Film" / "TANK2.sof", tiles3);
    write_file(root / "Film" / "Default.sof", tiles2);

    std::string weapon;
    for (int line = 0; line < 24; ++line) {
        weapon += "2, 15\r\n"; // count 2, fps 15
    }
    write_file(root / "Film" / "weapon.fdx",
               std::vector<std::uint8_t>(weapon.begin(), weapon.end()));
    std::string ammo;
    for (int line = 0; line < 16; ++line) {
        ammo += "2, 15\r\n";
    }
    write_file(root / "Film" / "ammo.fdx",
               std::vector<std::uint8_t>(ammo.begin(), ammo.end()));
    std::string tank;
    for (int line = 0; line < 8; ++line) {
        tank += "2, 15\r\n";
    }
    write_file(root / "Film" / "tank.fdx",
               std::vector<std::uint8_t>(tank.begin(), tank.end()));
    const std::string def = "1, 15\r\n";
    write_file(root / "Film" / "default.fdx",
               std::vector<std::uint8_t>(def.begin(), def.end()));
    return root;
}

// ------------------------------------------------------------ classification

void test_classify() {
    struct Case {
        const char* screen;
        gm::HudFamily family;
        bool has_id;
        int id;
        gm::HudBasis basis;
        const char* stem;
    };
    const Case cases[] = {
        {"DESIGN/WEAPON15.SML", gm::HudFamily::Weapon, true, 15, gm::HudBasis::Proven,
         "WEAPON15"},
        {"ammo0.sml", gm::HudFamily::Ammo, true, 0, gm::HudBasis::Proven, "AMMO0"},
        {"TANK2.SME", gm::HudFamily::Tank, true, 2, gm::HudBasis::Proven, "TANK2"},
        {"shield0.sml", gm::HudFamily::Shield, true, 0, gm::HudBasis::Proven, "SHIELD0"},
        {"EQUIP5.SME", gm::HudFamily::Equip, true, 5, gm::HudBasis::Proven, "EQUIP5"},
        {"Film/Default.sof", gm::HudFamily::Default, true, 0, gm::HudBasis::Proven,
         "DEFAULT"},
        {"WEAPON", gm::HudFamily::Weapon, false, 0, gm::HudBasis::Proven, "WEAPON"},
        {"TEMP0.SML", gm::HudFamily::Default, false, 0, gm::HudBasis::Hypothesis,
         "TEMP0"},
        {"brief/LEVEL1.SME", gm::HudFamily::Default, false, 0,
         gm::HudBasis::Hypothesis, "LEVEL1"},
        {"WEAPONS", gm::HudFamily::Default, false, 0, gm::HudBasis::Hypothesis,
         "WEAPONS"},
        {"weapon9x", gm::HudFamily::Default, false, 0, gm::HudBasis::Hypothesis,
         "WEAPON9X"},
        {"", gm::HudFamily::Default, false, 0, gm::HudBasis::Hypothesis, ""},
    };
    for (const auto& item : cases) {
        const auto got = gm::classify_screen(item.screen);
        check_eq(got.stem, item.stem, std::string("stem of ") + item.screen);
        check(got.family == item.family, std::string("family of ") + item.screen);
        check_eq(got.has_id, item.has_id, std::string("has_id of ") + item.screen);
        check_eq(got.id, item.id, std::string("id of ") + item.screen);
        check(got.basis == item.basis, std::string("basis of ") + item.screen);
    }
}

// ------------------------------------------------------------ shipped corpus

struct CorpusStats {
    std::size_t files = 0;
    std::size_t parsed = 0;
    std::size_t malformed = 0;
    std::size_t images = 0;
    std::size_t screens_with_images = 0;
    std::size_t left = 0;
    std::size_t right = 0;
    std::set<std::string> unique_images;
    std::map<std::string, std::size_t> design_families;
    std::size_t design_screens = 0;
    std::size_t design_temp = 0;
};

bool test_corpus(const fs::path& game) {
    const fs::path root = game / "SML";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        std::cout << "hud_bind: SML corpus not installed, golden sweep skipped\n";
        return false;
    }

    CorpusStats stats;
    std::set<std::string> screens_with_images;
    std::set<std::string> design_seen;
    std::vector<std::pair<fs::path, fs::path>> files;
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string ext = lower(entry.path().extension().string());
        if (ext == ".sml" || ext == ".sme") {
            files.emplace_back(entry.path(), entry.path().parent_path());
        }
    }
    std::sort(files.begin(), files.end());

    for (const auto& [path, parent] : files) {
        const std::string relative = fs::relative(path, root, ec).generic_string();
        const auto bytes = read_file(path);
        std::string error;
        const auto doc = gm::parse_sml(bytes, error);
        ++stats.files;
        if (!doc) {
            ++stats.malformed;
            check(relative == "prelevel/dm4.sml",
                  "only dm4.sml is expected to fail, got " + relative + ": " + error);
            continue;
        }
        ++stats.parsed;

        const auto binding = gm::parse_binding(*doc, relative, error);
        if (!binding) {
            check(false, "binding failed for " + relative + ": " + error);
            continue;
        }

        if (lower(parent.filename().string()) == "design" &&
            design_seen.insert(lower(path.stem().string())).second) {
            ++stats.design_screens;
            if (!binding->has_id()) {
                ++stats.design_temp;
                check_eq(binding->screen_class.stem, "TEMP0", "only TEMP0 lacks an id");
            } else {
                ++stats.design_families[std::string(gm::hud_family_name(binding->family()))];
            }
        }

        if (!binding->images.empty()) {
            screens_with_images.insert(lower(parent.filename().string()) + "/" +
                                       lower(path.stem().string()));
        }
        stats.images += binding->images.size();
        for (const auto& image : binding->images) {
            stats.unique_images.insert(lower(image.image));
            check_eq(image.x, 64, relative + ": shipped <I> x");
            check_eq(image.y, 64, relative + ": shipped <I> y");
            check(image.anchor != gm::HudAnchor::None,
                  relative + ": shipped <I> has an anchor");
            check(image.x_reserved, relative + ": shipped <I> x is reserved");
            check(image.position_basis == gm::HudBasis::Hypothesis,
                  relative + ": anchored slot is a hypothesis");
            if (image.anchor == gm::HudAnchor::Left) {
                ++stats.left;
            } else if (image.anchor == gm::HudAnchor::Right) {
                ++stats.right;
            }
        }
    }

    stats.screens_with_images = screens_with_images.size();

    check_eq(stats.files, std::size_t{300}, "corpus file count");
    check_eq(stats.parsed, std::size_t{299}, "corpus parsed count");
    check_eq(stats.malformed, std::size_t{1}, "corpus malformed count");
    check_eq(stats.images, std::size_t{72}, "corpus image count");
    check_eq(stats.unique_images.size(), std::size_t{15}, "corpus unique image count");
    check_eq(stats.screens_with_images, std::size_t{24}, "corpus screens with images");
    check_eq(stats.left, std::size_t{50}, "left anchors");
    check_eq(stats.right, std::size_t{22}, "right anchors");
    check_eq(stats.design_screens, std::size_t{47}, "design screen count");
    check_eq(stats.design_temp, std::size_t{1}, "design template count");
    check_eq(stats.design_families["weapon"], std::size_t{20}, "design weapon screens");
    check_eq(stats.design_families["ammo"], std::size_t{12}, "design ammo screens");
    check_eq(stats.design_families["tank"], std::size_t{4}, "design tank screens");
    check_eq(stats.design_families["shield"], std::size_t{1}, "design shield screens");
    check_eq(stats.design_families["equip"], std::size_t{9}, "design equip screens");
    check_eq(stats.design_families["default"], std::size_t{0},
             "design screens never fall back to Default");

    // The 15 loose BMP names from docs/UI.md §7.2.
    const std::set<std::string> expected = {
        "drilhed", "hotcargo", "jmpshld", "ltmills",  "ltstrong", "lv1swtch",
        "lv6idol", "oilfile",  "pamelal",  "prnguyen", "sensaray", "sparnuke",
        "virsampl", "viruscor", "vpishii",
    };
    check(stats.unique_images == expected, "shipped image names");

    std::cout << "hud_bind: corpus " << stats.files << " files (" << stats.parsed
              << " bound, " << stats.malformed << " malformed), " << stats.images
              << " images (" << stats.unique_images.size() << " unique) on "
              << stats.screens_with_images << " screens, anchors l/r " << stats.left << "/"
              << stats.right << ", design screens " << stats.design_screens << "\n";
    return true;
}

// ------------------------------------------------------- real single screens

bool test_shipped_screens(const fs::path& game) {
    const fs::path root = game / "SML";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return false;
    }
    bool ran = false;

    // help.sme: the corrected edition, no <I> -> the film is the overlay.
    {
        const auto bytes = read_file(root / "help.sme");
        std::string error;
        const auto doc = gm::parse_sml(bytes, error);
        check(doc.has_value(), "help.sme parses: " + error);
        if (doc) {
            const auto binding = gm::parse_binding(*doc, "help.sme", error);
            check(binding.has_value(), "help.sme binds: " + error);
            if (binding) {
                check(binding->images.empty(), "help.sme has no image slots");
                check(binding->family() == gm::HudFamily::Default,
                      "help.sme is not a film family page");
                ran = true;
            }
        }
    }

    // brief/level1.sml: two slots, left and right.
    {
        const auto bytes = read_file(root / "brief" / "level1.sml");
        std::string error;
        const auto doc = gm::parse_sml(bytes, error);
        check(doc.has_value(), "brief/level1.sml parses: " + error);
        if (doc) {
            const auto binding = gm::parse_binding(*doc, "brief/level1.sml", error);
            check(binding.has_value(), "brief/level1.sml binds: " + error);
            if (binding) {
                check_eq(binding->images.size(), std::size_t{2}, "level1 image slots");
                check_eq(binding->images[0].image, "ltmills", "level1 slot 0 name");
                check(binding->images[0].anchor == gm::HudAnchor::Left,
                      "level1 slot 0 is left");
                check_eq(binding->images[1].image, "lv1swtch", "level1 slot 1 name");
                check(binding->images[1].anchor == gm::HudAnchor::Right,
                      "level1 slot 1 is right");

                const auto film = synthetic_film(2, 2, 15);
                const auto target = make_target(320, 200, 5);
                const auto placements = gm::layout(*binding, film, target, error);
                check(placements.has_value(), "level1 layout: " + error);
                if (placements) {
                    check_eq(placements->size(), std::size_t{2}, "level1 placement count");
                    check_eq((*placements)[0].x, 0, "left anchor x");
                    check_eq((*placements)[0].y, 64, "left anchor y");
                    check_eq((*placements)[1].x, 192, "right anchor x (320-128)");
                    check_eq((*placements)[1].y, 64, "right anchor y");
                    check((*placements)[0].visible && !(*placements)[0].clipped,
                          "left slot fully visible");
                    check((*placements)[1].visible && !(*placements)[1].clipped,
                          "right slot fully visible");
                    ran = true;
                }
            }
        }
    }
    return ran;
}

// ------------------------------------------------------------ synthetic docs

const char* kSyntheticMarkup =
    "<d><p>Test</p>"
    "<I f=\"alpha\" x=\"10\" y=\"20\">"
    "<I f=\"beta\" x=\"64\" y=\"64\" a=\"l\">"
    "<I f=\"gamma\" x=\"64\" y=\"64\" a=\"R\">"
    "</d>";

void test_synthetic_layout() {
    std::string error;
    const auto binding = bind_screen(kSyntheticMarkup, "design/WEAPON4.sml", error);
    check(binding.has_value(), "synthetic binding: " + error);
    if (!binding) {
        return;
    }
    check(binding->family() == gm::HudFamily::Weapon, "synthetic family WEAPON");
    check(binding->has_id(), "synthetic screen has an id");
    check_eq(binding->id(), 4, "synthetic screen id");
    check_eq(binding->images.size(), std::size_t{3}, "synthetic image slots");

    check(binding->images[0].anchor == gm::HudAnchor::None, "slot 0 has no anchor");
    check(binding->images[0].position_basis == gm::HudBasis::Proven,
          "slot 0 position is proven");
    check(!binding->images[0].x_reserved, "slot 0 x is authoritative");
    check(binding->images[1].anchor == gm::HudAnchor::Left, "slot 1 left");
    check(binding->images[1].x_reserved, "slot 1 x is reserved");
    check(binding->images[2].anchor == gm::HudAnchor::Right, "slot 2 right");

    const auto film = synthetic_film(3, 3, 15);
    const auto target = make_target(320, 200, 9);
    const auto placements = gm::layout(*binding, film, target, error);
    check(placements.has_value(), "synthetic layout: " + error);
    if (placements) {
        check_eq(placements->size(), std::size_t{3}, "synthetic placement count");
        check_eq((*placements)[0].x, 10, "slot 0 x from attribute");
        check_eq((*placements)[0].y, 20, "slot 0 y from attribute");
        check((*placements)[0].basis == gm::HudBasis::Proven, "slot 0 placement proven");
        check_eq((*placements)[1].x, 0, "slot 1 left edge");
        check_eq((*placements)[2].x, 192, "slot 2 right edge");
        check_eq((*placements)[1].width, 128, "film width");
        check_eq((*placements)[1].height, 64, "film height");
    }

    // The document overload must agree with the binding overload.
    const auto doc = parse_plain(kSyntheticMarkup);
    check(doc.has_value(), "synthetic document parses");
    if (doc) {
        const auto from_doc = gm::layout(*doc, film, target, error);
        check(from_doc.has_value(), "layout(doc) works: " + error);
        if (from_doc && placements) {
            check_eq(from_doc->size(), placements->size(), "layout(doc) count");
            for (std::size_t i = 0; i < from_doc->size() && i < placements->size(); ++i) {
                check_eq((*from_doc)[i].x, (*placements)[i].x, "layout(doc) x");
                check_eq((*from_doc)[i].y, (*placements)[i].y, "layout(doc) y");
            }
        }
    }
}

void test_overlay_and_clip() {
    std::string error;
    const auto binding = bind_screen("<d><p>no images here</p></d>", "design/TANK0.sml", error);
    check(binding.has_value(), "overlay binding: " + error);
    if (!binding) {
        return;
    }
    const auto film = synthetic_film(2, 2, 15);

    const auto full = gm::layout(*binding, film, make_target(320, 200, 0), error);
    check(full.has_value(), "overlay layout: " + error);
    if (full) {
        check_eq(full->size(), std::size_t{1}, "overlay placement count");
        check_eq((*full)[0].x, 14, "overlay x (0x495a08: +14)");
        check_eq((*full)[0].y, 125, "overlay y (0x495a08: 125)");
        check((*full)[0].basis == gm::HudBasis::Proven, "overlay is proven");
        check((*full)[0].visible && !(*full)[0].clipped, "overlay fully on screen");
    }

    // Bottom slice: y=125 on a 130-row surface leaves 5 rows.
    const auto sliced = gm::layout(*binding, film, make_target(320, 130, 0), error);
    check(sliced.has_value(), "sliced layout: " + error);
    if (sliced) {
        check((*sliced)[0].clipped, "sliced overlay is clipped");
        check((*sliced)[0].visible, "sliced overlay stays visible");
        check_eq((*sliced)[0].clip_height, 5, "sliced overlay clip height");
        check_eq((*sliced)[0].clip_width, 128, "sliced overlay clip width");
    }

    // Entirely below a 60-row surface: kept but not visible.
    const auto hidden = gm::layout(*binding, film, make_target(320, 60, 0), error);
    check(hidden.has_value(), "hidden layout: " + error);
    if (hidden) {
        check(!(*hidden)[0].visible, "hidden overlay is not visible");
        check((*hidden)[0].clipped, "hidden overlay is clipped");
        check_eq((*hidden)[0].clip_height, 0, "hidden overlay clip height");
    }

    // Right anchor narrower than the film: negative x, still visible.
    const auto narrow = bind_screen("<d><I f=\"x\" y=\"10\" a=\"r\"></d>", "brief/LEVEL1.SME",
                             error);
    check(narrow.has_value(), "narrow binding: " + error);
    if (narrow) {
        check_eq(narrow->images[0].x, 0, "right anchor without x records 0");
        const auto placed = gm::layout(*narrow, film, make_target(100, 60, 0), error);
        check(placed.has_value(), "narrow layout: " + error);
        if (placed) {
            check_eq((*placed)[0].x, -28, "right anchor x (100-128)");
            check((*placed)[0].clipped, "negative x is clipped");
            check((*placed)[0].visible, "negative x keeps 72 columns visible");
            check_eq((*placed)[0].clip_width, 100, "negative x clip width");
        }
    }
}

// ------------------------------------------------------------- film resolve

void test_resolve(const fs::path& game) {
    std::string error;
    const auto binding = bind_screen("<d><p>x</p></d>", "design/WEAPON4.sml", error);
    check(binding.has_value(), "resolve binding: " + error);
    if (!binding) {
        return;
    }

    // 1. Name wins: WEAPON4 -> weapon.fdx line 4.
    {
        const auto ref = gm::resolve_film(*binding, gm::HudState{}, game, error);
        check(ref.has_value(), "WEAPON4 resolves: " + error);
        if (ref) {
            check_eq(ref->name, "WEAPON4", "WEAPON4 name");
            check(ref->family == gm::HudFamily::Weapon, "WEAPON4 family");
            check(ref->basis == gm::HudBasis::Proven, "WEAPON4 is name-proven");
            check_eq(ref->id, 4, "WEAPON4 id");
            check_eq(ref->film.line, 4, "WEAPON4 index line");
            check_eq(ref->film.count, 2u, "WEAPON4 count from fdx");
            check_eq(ref->film.fps, 15u, "WEAPON4 fps from fdx");
            check(!ref->note.empty(), "WEAPON4 note");
        }
    }

    // 2. Generic screen + state focus.
    {
        const auto generic = bind_screen("<d><p>x</p></d>", "prelevel/level3", error);
        check(generic.has_value(), "generic binding: " + error);
        if (generic) {
            gm::HudState state;
            state.focus = gm::HudFamily::Weapon;
            state.weapon = 4;
            const auto ref = gm::resolve_film(*generic, state, game, error);
            check(ref.has_value(), "state weapon resolves: " + error);
            if (ref) {
                check_eq(ref->name, "WEAPON4", "state weapon name");
                check(ref->basis == gm::HudBasis::Hypothesis, "state choice is hypothesis");
                check_eq(ref->film.line, 4, "state weapon line");
            }

            state.focus = gm::HudFamily::Ammo;
            state.ammo = 3;
            const auto ammo = gm::resolve_film(*generic, state, game, error);
            check(ammo.has_value(), "state ammo resolves: " + error);
            if (ammo) {
                check_eq(ammo->name, "AMMO3", "state ammo name");
                check_eq(ammo->film.line, 3, "state ammo line");
            }

            // No slot for the focus -> Default.sof, not a guess.
            state.focus = gm::HudFamily::Equip;
            state.equip = -1;
            const auto fallback = gm::resolve_film(*generic, state, game, error);
            check(fallback.has_value(), "fallback resolves: " + error);
            if (fallback) {
                check_eq(fallback->name, "Default", "fallback name");
                check(fallback->family == gm::HudFamily::Default, "fallback family");
                check(fallback->basis == gm::HudBasis::Hypothesis, "fallback is hypothesis");
                check_eq(fallback->film.line, 0, "fallback line 0");
                check(!fallback->note.empty(), "fallback note");
            }
        }
    }

    // 3. Screen names a family without an id -> state supplies the item.
    {
        const auto no_id = bind_screen("<d><p>x</p></d>", "WEAPON", error);
        check(no_id.has_value(), "family-only binding: " + error);
        if (no_id) {
            gm::HudState state;
            state.weapon = 4;
            const auto ref = gm::resolve_film(*no_id, state, game, error);
            check(ref.has_value(), "family-only resolves: " + error);
            if (ref) {
                check_eq(ref->name, "WEAPON4", "family-only name");
                check(ref->basis == gm::HudBasis::Hypothesis,
                      "family-only id comes from state");
            }
        }
    }

    // 4. TANK by name.
    {
        const auto tank = bind_screen("<d><p>x</p></d>", "design/TANK2.SME", error);
        check(tank.has_value(), "tank binding: " + error);
        if (tank) {
            const auto ref = gm::resolve_film(*tank, gm::HudState{}, game, error);
            check(ref.has_value(), "TANK2 resolves: " + error);
            if (ref) {
                check_eq(ref->name, "TANK2", "TANK2 name");
                check_eq(ref->film.line, 2, "TANK2 line");
            }
        }
    }

    // 5. Missing film / missing line are explicit.
    {
        const auto missing = bind_screen("<d><p>x</p></d>", "design/WEAPON20.SML", error);
        check(missing.has_value(), "WEAPON20 binding: " + error);
        if (missing) {
            check(!gm::resolve_film(*missing, gm::HudState{}, game, error),
                  "WEAPON20 has no film");
            check(error.find("WEAPON20") != std::string::npos,
                  "WEAPON20 error names the film: " + error);
        }

        const auto outside = bind_screen("<d><p>x</p></d>", "design/WEAPON30.SML", error);
        check(outside.has_value(), "WEAPON30 binding: " + error);
        if (outside) {
            check(!gm::resolve_film(*outside, gm::HudState{}, game, error),
                  "WEAPON30 line is outside weapon.fdx");
            check(error.find("outside") != std::string::npos,
                  "WEAPON30 error mentions the index: " + error);
        }

        const auto equip = bind_screen("<d><p>x</p></d>", "design/EQUIP0.SME", error);
        check(equip.has_value(), "EQUIP0 binding: " + error);
        if (equip) {
            check(!gm::resolve_film(*equip, gm::HudState{}, game, error),
                  "EQUIP0 has no .sof in the shipment");
            check(error.find("EQUIP0") != std::string::npos,
                  "EQUIP0 error names the film: " + error);
        }
    }
}

void test_errors() {
    std::string error;

    // Hand-built documents: every structural field is checked.
    {
        gm::SmlDocument doc;
        check(!gm::parse_binding(doc, "x", error), "empty document rejected");
        check(error.find("empty") != std::string::npos, "empty document error");

        doc.text = "<d></d>";
        check(!gm::parse_binding(doc, "x", error), "tagless document rejected");
        check(error.find("no tags") != std::string::npos, "tagless document error");

        doc.tags = gm::sml_scan_tags(doc.text);
        doc.doc_open = false;
        check(!gm::parse_binding(doc, "x", error), "missing <d> rejected");
        check(error.find("<d>") != std::string::npos, "missing <d> error");

        doc.doc_open = true;
        doc.doc_close = false;
        check(!gm::parse_binding(doc, "x", error), "missing </d> rejected");
        check(error.find("</d>") != std::string::npos, "missing </d> error");

        doc.doc_close = true;
        doc.brackets_balanced = false;
        check(!gm::parse_binding(doc, "x", error), "bracket imbalance rejected");
        check(error.find("bracket") != std::string::npos, "bracket error");

        doc.brackets_balanced = true;
        doc.too_many_tags = true;
        check(!gm::parse_binding(doc, "x", error), "tag limit rejected");
        check(error.find("tag limit") != std::string::npos, "tag limit error");
    }

    // <I> attribute errors, each with a line number and a keyword.
    struct ImageCase {
        const char* markup;
        const char* needle;
    };
    const ImageCase cases[] = {
        {"<d><I x=\"1\" y=\"2\"></d>", "no f"},
        {"<d><I f=\"\" x=\"1\" y=\"2\"></d>", "no f"},
        {"<d><I f=\"a\" x=\"abc\" y=\"2\"></d>", "not an integer"},
        {"<d><I f=\"a\" x=\"1\" y=\"qq\"></d>", "not an integer"},
        {"<d><I f=\"a\" x=\"1\"></d>", "no y"},
        {"<d><I f=\"a\" y=\"2\"></d>", "no x"},
        {"<d><I f=\"a\" x=\"1\" y=\"2\" a=\"q\"></d>", "alignment"},
        {"<d><I f=\"a\" x=\"99999\" y=\"2\"></d>", "outside"},
        {"<d><I f=\"a\" x=\"1\" y=\"-99999\"></d>", "outside"},
    };
    for (const auto& item : cases) {
        const auto doc = parse_plain(item.markup);
        check(doc.has_value(), std::string("error case parses: ") + item.markup);
        if (!doc) {
            continue;
        }
        check(!gm::parse_binding(*doc, "brief/LEVEL1.SME", error),
              std::string("rejected: ") + item.markup);
        check(error.find(item.needle) != std::string::npos,
              std::string("error mentions '") + item.needle + "': " + error);
        check(error.find("line 1") != std::string::npos,
              std::string("error names the line: ") + error);
    }

    // 33 slots is over the per-screen limit.
    {
        std::string markup = "<d>";
        for (int i = 0; i < 33; ++i) {
            markup += "<I f=\"a\" x=\"1\" y=\"2\">";
        }
        markup += "</d>";
        const auto doc = parse_plain(markup);
        check(doc.has_value(), "33-image document parses");
        if (doc) {
            check(!gm::parse_binding(*doc, "brief/LEVEL1.SME", error),
                  "33 images rejected");
            check(error.find("more than 32") != std::string::npos,
                  "image limit error: " + error);
        }
    }

    // Layout: bad target and bad film.
    {
        const auto doc = parse_plain("<d><p>x</p></d>");
        check(doc.has_value(), "layout error document parses");
        if (doc) {
            const auto film = synthetic_film(2, 2, 15);
            gm::RenderResult zero;
            check(!gm::layout(*doc, film, zero, error), "zero-size target rejected");
            check(!error.empty(), "zero-size target error");

            auto short_buffer = make_target(320, 200, 0);
            short_buffer.rgb.pop_back();
            check(!gm::layout(*doc, film, short_buffer, error),
                  "short buffer rejected");
            check(error.find("buffer") != std::string::npos, "short buffer error");

            auto bad_film = synthetic_film(2, 0, 15);
            check(!gm::layout(*doc, bad_film, make_target(320, 200, 0), error),
                  "count 0 rejected");
            check(!error.empty(), "count 0 error");

            auto too_many = synthetic_film(2, 3, 15);
            check(!gm::layout(*doc, too_many, make_target(320, 200, 0), error),
                  "count above tiles rejected");
            check(!error.empty(), "count above tiles error");
        }
    }
}

} // namespace

int main() {
    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_DIR")) {
        game = env;
    }

    test_classify();
    test_synthetic_layout();
    test_overlay_and_clip();
    test_errors();

    const fs::path fixture = make_game_fixture();
    test_resolve(fixture);

    const bool shipped = test_shipped_screens(game);
    const bool corpus = test_corpus(game);
    std::error_code ec;
    fs::remove_all(fixture, ec);

    if (!shipped || !corpus) {
        std::cout << "hud_bind: game install incomplete, shipped goldens skipped\n";
    }

    if (failures == 0) {
        std::cout << "hud_bind: synthetic + shipped cases OK\n";
        return 0;
    }
    std::cerr << "hud_bind: " << failures << " failure(s)\n";
    return 1;
}
