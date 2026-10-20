// HUD composition: .sml/.sme screen + film timing + blit in one call.
//
// Shipped case (game install, read-only):
//   * DESIGN/ammo4.sml has no <I>, so the film is the 0x4958d0 overlay at
//     (14,125) and the composed 320x200 frame must be byte-identical to
//     hud_player's AMMO4 draw at ticks 0/91/182 (frames 1/25/49).  The FNV-1a64
//     goldens are the ones documented in docs/HUD_PLAYER.md §4:
//     aaf521e923f31099 / eeb1a6677e96f650 / 505b37daeecc1c76;
//   * DESIGN/equip0.sml resolves to EQUIP0, and the shipment has equip.fdx but
//     no EQUIP*.SOF -> explicit error, never a silent empty compose.
//
// Synthetic fixtures (no game needed): a temporary Film/ tree with a 2-tile
// Default film exercises <I> slots, anchors, clipping, the path/sibling
// extension resolution, negative/huge ticks, missing films, malformed markup,
// small targets and short palettes.  Every failure case checks that the target
// buffer is untouched.
//
// Pipeline and proven/hypothesis split: docs/HUD_COMPOSE.md.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "render/hud.h"
#include "render/hud_bind.h"
#include "render/hud_compose.h"
#include "render/hud_player.h"
#include "render/render.h"
#include "core/path.h"
#include "res/bmp.h"
#include "res/sml.h"
#include "res/sof.h"

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

std::uint64_t fnv1a64(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::uint8_t b : bytes) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    return h;
}

std::vector<std::uint8_t> make_palette() {
    std::vector<std::uint8_t> palette(256 * 4);
    for (int i = 0; i < 256; ++i) {
        palette[static_cast<std::size_t>(i) * 4 + 0] = static_cast<std::uint8_t>(i);
        palette[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>(255 - i);
        palette[static_cast<std::size_t>(i) * 4 + 2] = static_cast<std::uint8_t>((i * 7) & 0xff);
        palette[static_cast<std::size_t>(i) * 4 + 3] = 255;
    }
    return palette;
}

gm::RenderResult make_target(int width, int height, int fill) {
    gm::RenderResult target;
    target.width = width;
    target.height = height;
    target.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3,
                      static_cast<std::uint8_t>(fill));
    return target;
}

std::vector<std::uint8_t> flat_tile(std::uint8_t index) {
    return std::vector<std::uint8_t>(gm::kSofTileWidth * gm::kSofTileHeight, index);
}

std::vector<std::uint8_t> two_tile_sof() {
    std::vector<std::uint8_t> out;
    for (std::uint8_t index : {std::uint8_t{5}, std::uint8_t{6}}) {
        const auto tile = gm::encode_sof_tile(flat_tile(index));
        out.insert(out.end(), tile.begin(), tile.end());
    }
    return out;
}

std::vector<std::uint8_t> bytes_of(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

/// RGB triple of palette *index* in ``make_palette()``.
std::vector<std::uint8_t> palette_color(const std::vector<std::uint8_t>& palette,
                                        std::size_t index) {
    return {palette[index * 4 + 0], palette[index * 4 + 1], palette[index * 4 + 2]};
}

bool pixel_is(const gm::RenderResult& target, int x, int y,
              const std::vector<std::uint8_t>& rgb) {
    const std::size_t offset =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(target.width) +
         static_cast<std::size_t>(x)) *
        3;
    return target.rgb.size() >= offset + 3 && target.rgb[offset] == rgb[0] &&
           target.rgb[offset + 1] == rgb[1] && target.rgb[offset + 2] == rgb[2];
}

std::string fdx_text(int lines) {
    std::string text;
    for (int i = 0; i < lines; ++i) {
        text += "2, 15\r\n"; // count 2, fps 15
    }
    return text;
}

/// Temporary game tree: Film/Default.{sof,fdx} and Film/equip.fdx (no EQUIP
/// film, matching the shipment), plus SML/DESIGN/ammo4.sml for the path form.
fs::path make_game_fixture() {
    const fs::path root = fs::temp_directory_path() / "gm_hud_compose_game";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Film", ec);

    write_file(root / "Film" / "Default.sof", two_tile_sof());
    write_file(root / "Film" / "default.fdx", bytes_of(fdx_text(1)));
    write_file(root / "Film" / "ammo4.sof", two_tile_sof());
    write_file(root / "Film" / "ammo.fdx", bytes_of(fdx_text(16)));
    // equip.fdx exists in the shipment but EQUIP*.SOF does not.
    write_file(root / "Film" / "equip.fdx", bytes_of(fdx_text(6)));
    write_file(root / "SML" / "DESIGN" / "ammo4.sml", bytes_of("<d><p>Ammo 4</p></d>"));
    return root;
}

struct Fixture {
    fs::path root;
    std::vector<std::uint8_t> palette = make_palette();
};

// ------------------------------------------------------------ synthetic cases

void test_slots_and_clipping(const Fixture& fixture) {
    // Two slots: explicit (0,0) and right-anchored at y=64.  The names are
    // irrelevant to the geometry; "brief/level1.sml" resolves to Default.sof.
    const std::string markup =
        "<d><I f=\"ltmills\" x=\"0\" y=\"0\"><I f=\"lv1swtch\" a=\"r\" y=\"64\"></d>";
    const auto bytes = bytes_of(markup);
    const gm::HudState state;
    std::string error;

    const auto color = palette_color(fixture.palette, 6); // frame 1 -> tile 1 -> index 6

    {
        auto target = make_target(320, 200, 0);
        const auto summary =
            gm::compose_hud(target, fixture.root, bytes, "brief/level1.sml", state,
                            fixture.palette, 0, error);
        check(summary.has_value(), "slots: compose succeeds (" + error + ")");
        if (summary) {
            check_eq(summary->placements, std::size_t{2}, "slots: placements");
            check_eq(summary->visible, std::size_t{2}, "slots: visible");
            check_eq(summary->clipped, std::size_t{0}, "slots: clipped");
            check_eq(summary->drawn, std::size_t{2}, "slots: drawn");
            check(summary->family == gm::HudFamily::Default, "slots: Default family");
            check_eq(summary->film, "Default", "slots: Default film");
            check_eq(summary->line, 0, "slots: Default line 0");
            check_eq(summary->frame, std::size_t{1}, "slots: tick 0 -> frame 1");
            check_eq(summary->count, std::uint32_t{2}, "slots: count");
            check_eq(summary->fps, std::uint32_t{15}, "slots: fps");
            check(error.empty(), "slots: error cleared on success");
        }
        check(pixel_is(target, 0, 0, color), "slots: left slot pixel (0,0)");
        check(pixel_is(target, 127, 63, color), "slots: left slot bottom-right");
        check(pixel_is(target, 192, 64, color), "slots: right slot pixel (192,64)");
        check(pixel_is(target, 319, 127, color), "slots: right slot bottom-right");
        check(pixel_is(target, 128, 0, {0, 0, 0}), "slots: gap at x=128 untouched");
        check(pixel_is(target, 0, 64, {0, 0, 0}), "slots: row 64 under slot 0 untouched");
    }

    {
        // A 128x64 target can hold only the left slot; the right one is kept
        // in the layout but reported as clipped/invisible.
        auto target = make_target(128, 64, 0);
        const auto summary =
            gm::compose_hud(target, fixture.root, bytes, "brief/level1.sml", state,
                            fixture.palette, 0, error);
        check(summary.has_value(), "clip: compose succeeds (" + error + ")");
        if (summary) {
            check_eq(summary->placements, std::size_t{2}, "clip: placements kept");
            check_eq(summary->visible, std::size_t{1}, "clip: visible");
            check_eq(summary->clipped, std::size_t{1}, "clip: clipped");
            check_eq(summary->drawn, std::size_t{1}, "clip: drawn");
        }
        check(pixel_is(target, 127, 63, color), "clip: visible slot drawn");
    }
}

void test_path_resolution(const Fixture& fixture) {
    std::string error;

    // Wrong case and no extension: both must resolve to SML/DESIGN/ammo4.sml.
    const fs::path spellings[] = {
        fixture.root / "SML" / "DESIGN" / "AMMO4.sml",
        fixture.root / "SML" / "DESIGN" / "Ammo4",
    };
    for (const auto& path : spellings) {
        auto target = make_target(320, 200, 0);
        const auto summary = gm::compose_hud(target, fixture.root, path, gm::HudState{},
                                             fixture.palette, 0, error);
        check(summary.has_value(),
              "path: " + path.string() + " composes (" + error + ")");
        if (!summary) {
            continue;
        }
        check_eq(summary->screen, path.string(), "path: summary echoes the argument");
        check_eq(summary->stem, "AMMO4", "path: stem");
        check(summary->family == gm::HudFamily::Ammo, "path: Ammo family");
        check(summary->has_id, "path: AMMO4 has an id");
        check_eq(summary->id, 4, "path: id 4");
        check_eq(summary->film, "AMMO4", "path: film name");
        check_eq(summary->line, 4, "path: ammo.fdx line 4");
        check_eq(summary->placements, std::size_t{1}, "path: overlay placement");
        check_eq(summary->visible, std::size_t{1}, "path: overlay visible");
        check(summary->screen_basis == gm::HudBasis::Proven,
              "path: family prefix is proven");
    }

    // The path form must not write when the screen is absent.
    {
        auto target = make_target(320, 200, 7);
        const auto before = target.rgb;
        const auto summary =
            gm::compose_hud(target, fixture.root,
                            fixture.root / "SML" / "DESIGN" / "missing.sml", gm::HudState{},
                            fixture.palette, 0, error);
        check(!summary.has_value(), "path: missing screen rejected");
        check(error.find("no .sml/.sme screen") != std::string::npos,
              "path: missing screen error: " + error);
        check(target.rgb == before, "path: missing screen leaves the target untouched");
    }
}

void test_errors(const Fixture& fixture) {
    const auto markup = bytes_of("<d><p>Test</p></d>");
    const gm::HudState state;
    std::string error;

    // Missing film: equip.fdx exists, no EQUIP0.sof (the shipped situation).
    {
        auto target = make_target(320, 200, 3);
        const auto before = target.rgb;
        const auto summary =
            gm::compose_hud(target, fixture.root, markup, "design/equip0.sml", state,
                            fixture.palette, 0, error);
        check(!summary.has_value(), "errors: missing EQUIP film rejected");
        check(error.find("hud_compose:") != std::string::npos,
              "errors: missing film is prefixed: " + error);
        check(error.find("EQUIP0") != std::string::npos,
              "errors: missing film names EQUIP0: " + error);
        check(target.rgb == before, "errors: missing film leaves the target untouched");
    }

    // Negative tick cast to uint64: tick * fps overflows.
    {
        auto target = make_target(320, 200, 0);
        const auto before = target.rgb;
        const auto negative = static_cast<std::uint64_t>(-1);
        const auto summary =
            gm::compose_hud(target, fixture.root, markup, "brief/level1.sml", state,
                            fixture.palette, negative, error);
        check(!summary.has_value(), "errors: negative/huge tick rejected");
        check(error.find("overflow") != std::string::npos,
              "errors: tick overflow message: " + error);
        check(target.rgb == before, "errors: overflow leaves the target untouched");
    }

    // Exactly at the 64-bit boundary: UINT64_MAX / fps still overflows in the
    // scaled multiply for fps = 15.
    {
        auto target = make_target(320, 200, 0);
        const auto tick = std::numeric_limits<std::uint64_t>::max() / 15 + 1;
        check(!gm::compose_hud(target, fixture.root, markup, "brief/level1.sml", state,
                               fixture.palette, tick, error),
              "errors: tick*fps overflow rejected");
        check(error.find("overflow") != std::string::npos,
              "errors: boundary overflow message: " + error);
    }

    // Malformed markup.
    {
        auto target = make_target(320, 200, 0);
        const auto before = target.rgb;
        const auto broken = bytes_of("<d><p>truncated");
        check(!gm::compose_hud(target, fixture.root, broken, "brief/level1.sml", state,
                               fixture.palette, 0, error),
              "errors: malformed markup rejected");
        check(error.rfind("hud_compose:", 0) == 0,
              "errors: malformed markup is prefixed: " + error);
        check(target.rgb == before, "errors: malformed markup writes nothing");
    }

    // Empty screen bytes.
    {
        auto target = make_target(320, 200, 0);
        check(!gm::compose_hud(target, fixture.root, std::span<const std::uint8_t>(), "x.sml",
                               state, fixture.palette, 0, error),
              "errors: empty screen rejected");
        check(error.find("empty") != std::string::npos,
              "errors: empty screen message: " + error);
    }

    // Target too small / malformed.
    {
        auto target = make_target(64, 64, 0);
        const auto before = target.rgb;
        check(!gm::compose_hud(target, fixture.root, markup, "brief/level1.sml", state,
                               fixture.palette, 0, error),
              "errors: 64x64 target rejected");
        check(error.find("smaller than") != std::string::npos,
              "errors: small target message: " + error);
        check(target.rgb == before, "errors: small target writes nothing");

        auto short_buffer = make_target(320, 200, 0);
        short_buffer.rgb.pop_back();
        check(!gm::compose_hud(short_buffer, fixture.root, markup, "brief/level1.sml",
                               state, fixture.palette, 0, error),
              "errors: short buffer rejected");
        check(error.find("buffer") != std::string::npos,
              "errors: short buffer message: " + error);
    }

    // Palette shorter than 256 RGBA quads.
    {
        auto target = make_target(320, 200, 0);
        const auto before = target.rgb;
        std::vector<std::uint8_t> palette(255 * 4, 0);
        check(!gm::compose_hud(target, fixture.root, markup, "brief/level1.sml", state,
                               palette, 0, error),
              "errors: short palette rejected");
        check(error.find("palette") != std::string::npos,
              "errors: short palette message: " + error);
        check(target.rgb == before, "errors: short palette writes nothing");
    }
}

// --------------------------------------------------------------- shipped game

bool test_shipped(const fs::path& game) {
    std::error_code ec;
    if (!fs::is_directory(game / "Film", ec) || !fs::is_directory(game / "SML", ec)) {
        std::cout << "hud_compose: game not installed, shipped cases skipped\n";
        return false;
    }
    const auto screen = gm::resolve_ci(game, "SML/DESIGN/ammo4.sml");
    const auto palette_path = gm::resolve_ci(game, "Film/Kflm0000.bmp");
    if (!screen || !palette_path) {
        std::cout << "hud_compose: DESIGN/ammo4.sml or the palette is missing, "
                     "shipped cases skipped\n";
        return false;
    }

    std::string bmp_error;
    const auto bmp = gm::parse_bmp(read_file(*palette_path), bmp_error);
    check(bmp.has_value() && bmp->palette.size() == 1024,
          "shipped: Kflm0000.bmp carries 256 colours (" + bmp_error + ")");
    if (!bmp) {
        return false;
    }

    const std::uint64_t ticks[3] = {0, 91, 182};
    const std::size_t frames[3] = {1, 25, 49};
    const std::uint64_t goldens[3] = {0xaaf521e923f31099ull, 0xeeb1a6677e96f650ull,
                                      0x505b37daeecc1c76ull};
    std::string error;

    for (int i = 0; i < 3; ++i) {
        auto target = make_target(320, 200, 0);
        const auto summary =
            gm::compose_hud(target, game, *screen, gm::HudState{}, bmp->palette,
                            ticks[i], error);
        check(summary.has_value(), "shipped: AMMO4 tick " + std::to_string(ticks[i]) +
                                       " composes (" + error + ")");
        if (!summary) {
            continue;
        }
        check(summary->family == gm::HudFamily::Ammo, "shipped: family");
        check_eq(summary->stem, "AMMO4", "shipped: stem from the file name");
        check(summary->has_id && summary->id == 4, "shipped: id 4");
        check_eq(summary->film, "AMMO4", "shipped: film");
        check_eq(summary->line, 4, "shipped: ammo.fdx line 4");
        check_eq(summary->count, std::uint32_t{131}, "shipped: count 131");
        check_eq(summary->fps, std::uint32_t{24}, "shipped: fps 24");
        check_eq(summary->frame, frames[i], "shipped: frame");
        check_eq(summary->placements, std::size_t{1}, "shipped: overlay placement");
        check_eq(summary->visible, std::size_t{1}, "shipped: overlay visible");
        check_eq(summary->clipped, std::size_t{0}, "shipped: overlay not clipped");
        check_eq(summary->drawn, std::size_t{1}, "shipped: overlay drawn");
        check(summary->family_name == "ammo", "shipped: family name");

        // Byte-for-byte parity with the player at the same origin, not just an
        // equal hash.
        const auto film = gm::load_hud_film(game, "AMMO4", error);
        check(film.has_value(), "shipped: hud_player AMMO4 loads (" + error + ")");
        if (film) {
            auto reference = make_target(320, 200, 0);
            check(gm::draw(*film, reference, bmp->palette, gm::kHudBindFilmX,
                           gm::kHudBindFilmY, false, 1, 1.0, ticks[i], error),
                  "shipped: hud_player AMMO4 draws (" + error + ")");
            check(target.rgb == reference.rgb,
                  "shipped: AMMO4 tick " + std::to_string(ticks[i]) +
                      " matches hud_player byte for byte");
        }

        const std::uint64_t hash = fnv1a64(target.rgb);
        check(hash == goldens[i], "shipped: AMMO4 tick " + std::to_string(ticks[i]) +
                                      " FNV-1a64 " + std::to_string(goldens[i]) +
                                      " (got " + std::to_string(hash) + ")");
    }

    // EQUIP0 is the shipped "no film" case: equip.fdx exists, no .SOF.
    if (const auto equip = gm::resolve_ci(game, "SML/DESIGN/equip0.sml")) {
        auto target = make_target(320, 200, 0);
        const auto before = target.rgb;
        check(!gm::compose_hud(target, game, *equip, gm::HudState{}, bmp->palette, 0,
                               error),
              "shipped: EQUIP0 rejected");
        check(error.find("EQUIP0") != std::string::npos,
              "shipped: EQUIP0 error names the film: " + error);
        check(target.rgb == before, "shipped: EQUIP0 writes nothing");
    }

    // A screen with no id and no state slot falls back to Default.sof.
    if (const auto help = gm::resolve_ci(game, "SML/help.sme")) {
        auto target = make_target(320, 200, 0);
        const auto summary = gm::compose_hud(target, game, *help, gm::HudState{},
                                             bmp->palette, 0, error);
        check(summary.has_value(), "shipped: help.sme composes (" + error + ")");
        if (summary) {
            check_eq(summary->film, "Default", "shipped: help.sme -> Default.sof");
            check(summary->film_basis == gm::HudBasis::Hypothesis,
                  "shipped: fallback is a hypothesis");
        }
    }
    return true;
}

} // namespace

int main() {
    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_DIR")) {
        game = env;
    }

    const Fixture fixture{make_game_fixture()};

    test_slots_and_clipping(fixture);
    test_path_resolution(fixture);
    test_errors(fixture);

    const bool shipped = test_shipped(game);

    std::error_code ec;
    fs::remove_all(fixture.root, ec);

    if (!shipped) {
        std::cout << "hud_compose: game install incomplete, shipped goldens skipped\n";
    }

    if (failures == 0) {
        std::cout << "hud_compose: synthetic + shipped cases OK\n";
        return 0;
    }
    std::cerr << "hud_compose: " << failures << " failure(s)\n";
    return 1;
}
