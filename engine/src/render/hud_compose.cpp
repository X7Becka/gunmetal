#include "render/hud_compose.h"

#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "core/path.h"
#include "render/hud.h"
#include "res/sml.h"

namespace fs = std::filesystem;

namespace gm {
namespace {

bool read_bytes(const fs::path& path, std::vector<std::uint8_t>& out,
                std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path.string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        error = "read failed: " + path.string();
        return false;
    }
    return true;
}

/// One candidate path: exact name first (fast path), then case-insensitive
/// resolution against *game* (relative) or the parent directory (absolute).
std::optional<fs::path> try_path(const fs::path& candidate, const fs::path& game) {
    std::error_code ec;
    if (fs::is_regular_file(candidate, ec)) {
        return candidate;
    }
    std::optional<fs::path> hit;
    if (candidate.is_absolute()) {
        hit = resolve_ci(candidate.parent_path(), candidate.filename().string());
    } else if (!game.empty()) {
        hit = resolve_ci(game, candidate.generic_string());
    }
    if (hit && fs::is_regular_file(*hit, ec)) {
        return hit;
    }
    return std::nullopt;
}

/// Resolve *screen*: the exact file, else the same stem with ``.sml``/``.sme``
/// in either case (the shipment mixes ``ammo4.sml``, ``AMMO4.SME``).
std::optional<fs::path> resolve_screen_path(const fs::path& screen, const fs::path& game) {
    if (auto hit = try_path(screen, game)) {
        return hit;
    }
    fs::path stem = screen;
    stem.replace_extension();
    for (const char* extension : {".sml", ".SML", ".sme", ".SME"}) {
        fs::path candidate = stem;
        candidate += extension;
        if (auto hit = try_path(candidate, game)) {
            return hit;
        }
    }
    return std::nullopt;
}

/// Prefix *message* with this module, keeping the origin of nested errors.
std::string compose_error(std::string_view message) {
    return "hud_compose: " + std::string(message);
}

bool validate_target(const RenderResult& target, std::string& error) {
    if (target.width < kHudComposeMinWidth || target.height < kHudComposeMinHeight) {
        error = compose_error("target " + std::to_string(target.width) + "x" +
                              std::to_string(target.height) + " is smaller than " +
                              std::to_string(kHudComposeMinWidth) + "x" +
                              std::to_string(kHudComposeMinHeight));
        return false;
    }
    if (target.width > 8192 || target.height > 8192) {
        error = compose_error("target " + std::to_string(target.width) + "x" +
                              std::to_string(target.height) + " is outside 1..8192");
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(target.width) *
                                 static_cast<std::size_t>(target.height) * 3u;
    if (target.rgb.size() != expected) {
        error = compose_error("target buffer is " + std::to_string(target.rgb.size()) +
                              " bytes, expected " + std::to_string(expected));
        return false;
    }
    return true;
}

bool validate_palette(const std::vector<std::uint8_t>& palette, std::string& error) {
    if (palette.size() < kHudComposePaletteBytes) {
        error = compose_error("palette has " + std::to_string(palette.size()) +
                              " bytes, need " + std::to_string(kHudComposePaletteBytes) +
                              " (256 RGBA entries)");
        return false;
    }
    return true;
}

std::optional<HudComposeSummary> compose(RenderResult& target, const fs::path& game,
                                         std::string_view screen_name,
                                         std::span<const std::uint8_t> bytes,
                                         const HudState& state,
                                         const std::vector<std::uint8_t>& palette,
                                         std::uint64_t tick, std::string& error) {
    error.clear();

    // Validate everything that does not need the filesystem first, so a bad
    // caller argument never reaches the game directory and never writes pixels.
    if (!validate_target(target, error)) {
        return std::nullopt;
    }
    if (!validate_palette(palette, error)) {
        return std::nullopt;
    }
    if (bytes.empty()) {
        error = compose_error("screen '" + std::string(screen_name) + "' is empty");
        return std::nullopt;
    }

    const auto doc = parse_sml(bytes, error);
    if (!doc) {
        error = compose_error(error);
        return std::nullopt;
    }
    const auto binding = parse_binding(*doc, screen_name, error);
    if (!binding) {
        error = compose_error(error);
        return std::nullopt;
    }

    const auto film_ref = resolve_film(*binding, state, game, error);
    if (!film_ref) {
        error = compose_error(error);
        return std::nullopt;
    }

    // Clock check before layout/blit: a tick overflow must not leave a partial
    // frame behind.
    const auto current = frame_at(film_ref->film, tick, error);
    if (!current) {
        error = compose_error(error);
        return std::nullopt;
    }

    const auto placements = layout(*binding, film_ref->film, target, error);
    if (!placements) {
        error = compose_error(error);
        return std::nullopt;
    }

    HudComposeSummary summary;
    summary.screen.assign(screen_name.data(), screen_name.size());
    summary.stem = binding->screen_class.stem;
    summary.family = binding->family();
    summary.family_name = std::string(hud_family_name(binding->family()));
    summary.has_id = binding->has_id();
    summary.id = binding->id();
    summary.screen_basis = binding->screen_class.basis;
    summary.film_basis = film_ref->basis;
    summary.film = film_ref->name;
    summary.note = film_ref->note;
    summary.line = film_ref->film.line;
    summary.count = film_ref->film.count;
    summary.fps = film_ref->film.fps;
    summary.tick = tick;
    summary.frame = current->frame;
    summary.ended = current->ended;
    summary.placements = placements->size();

    for (const HudPlacement& placement : *placements) {
        if (placement.clipped) {
            ++summary.clipped;
        }
        if (!placement.visible) {
            continue;
        }
        ++summary.visible;

        HudLayer layer;
        layer.x = placement.x;
        layer.y = placement.y;
        layer.transparent = false; // parity with hud_player's AMMO4 goldens
        layer.transparent_index = 0;
        layer.opacity = 1.0;
        layer.scale = 1;
        if (!blit_indexed(target, film_ref->film.sof, current->frame, palette, layer,
                          error)) {
            error = compose_error("placement " + std::to_string(placement.index) + ": " +
                                  error);
            return std::nullopt;
        }
        ++summary.drawn;
    }

    return summary;
}

} // namespace

std::optional<HudComposeSummary> compose_hud(
    RenderResult& target, const fs::path& game, const fs::path& screen_path,
    const HudState& state, const std::vector<std::uint8_t>& palette,
    std::uint64_t tick, std::string& error) {
    error.clear();
    // Check the cheap invariants before touching the filesystem; ``compose``
    // repeats them, but this keeps the "no .sml" error from masking a bad
    // palette or a target that cannot hold a tile.
    if (!validate_target(target, error) || !validate_palette(palette, error)) {
        return std::nullopt;
    }

    const auto resolved = resolve_screen_path(screen_path, game);
    if (!resolved) {
        error = compose_error("no .sml/.sme screen at '" + screen_path.string() + "'");
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes;
    if (!read_bytes(*resolved, bytes, error)) {
        error = compose_error(std::move(error));
        return std::nullopt;
    }
    // ``classify_screen`` only reads the basename, so the original argument is
    // passed through: the summary can then echo exactly what the caller asked
    // for, not the resolved spelling.
    return compose(target, game, screen_path.string(), bytes, state, palette, tick,
                   error);
}

std::optional<HudComposeSummary> compose_hud(
    RenderResult& target, const fs::path& game,
    std::span<const std::uint8_t> screen_bytes, std::string_view screen_name,
    const HudState& state, const std::vector<std::uint8_t>& palette,
    std::uint64_t tick, std::string& error) {
    return compose(target, game, screen_name, screen_bytes, state, palette, tick, error);
}

} // namespace gm
