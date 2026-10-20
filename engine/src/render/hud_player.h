#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "render/hud.h"
#include "render/render.h"
#include "res/sof.h"

namespace gm {

/// The engine advances films on its 91 Hz tick counter (``0x44bdda``); the HUD
/// caller turns ``fps`` into a frame step with ``ms = 91 // fps``
/// (``0x49594a``).  This player uses the rational form of the same clock:
/// ``frame = tick * fps / 91`` - see docs/HUD_PLAYER.md §2 for the difference.
inline constexpr std::uint64_t kHudTicksPerSecond = 91;

/// Longest accepted film name (basename, extension optional).  The engine caps
/// the index prefix at 20 bytes (``0x498e3d``); this is a looser bound that
/// still rejects garbage before touching the filesystem.
inline constexpr std::size_t kHudMaxNameLength = 64;

/// One playable HUD film: the decoded container, its ``.fdx`` record and the
/// clock state lifted from ``film_set`` (``0x4958d0``).
///
/// ``sof``/``fdx``/``line`` are the loaded assets; the remaining fields are the
/// playback policy.  ``count`` is the exclusive frame bound of the ``.fdx``
/// record (frames ``0 .. count-1``), ``fps`` its clock rate, ``start_frame``
/// the frame shown at tick 0 and ``loop`` the wrap/hold switch.  The HUD loader
/// passes ``start_frame = 1`` and ``loop = 1``; both stay explicit parameters
/// because the scripted loader (``0x44da80``) uses ``start = 0``.
struct HudFilm {
    Sof sof;
    Fdx fdx;
    int line = 0;

    std::uint32_t count = 0;
    std::uint32_t fps = 0;
    std::uint32_t start_frame = 1;
    bool loop = true;
};

/// Result of one clock step.  ``ended`` is set only for a non-looping film
/// that has run past its last frame; the frame then holds at ``count - 1``
/// (the engine's "film is over" branch in ``0x49fd40``).
struct HudFrame {
    std::size_t frame = 0;
    bool ended = false;
};

/// Load ``<game>/Film/<name>`` (``.sof`` appended when missing, matched
/// case-insensitively) plus its sibling ``<letters>.fdx`` (falling back to the
/// legacy ``.idx``).  The numeric suffix of the name selects the index line:
/// ``WEAPON8`` -> line 8, ``Default`` -> line 0.  Returns ``nullopt`` and fills
/// *error* for a missing film/index, a suffix outside the index, ``count``
/// larger than the tile count, or any ``.sof``/``.fdx`` parse failure.
std::optional<HudFilm> load_hud_film(const std::filesystem::path& game,
                                     const std::string& name, std::string& error);

/// Frame on screen at *tick*: ``(tick * fps / 91 + start_frame)``, wrapped by
/// ``count`` when ``loop`` is set and clamped to ``count - 1`` otherwise.
/// Rejects a film with no index record, ``fps`` outside ``1..kFdxMaxFps``,
/// ``count`` of 0 or above the tile count, and a tick whose ``tick * fps``
/// would overflow 64 bits.
std::optional<HudFrame> frame_at(const HudFilm& film, std::uint64_t tick,
                                 std::string& error);

/// ``frame_at(tick)`` composed into *target* through ``blit_indexed``.
///
/// * ``transparent`` keys out VGA index 0 (the films' background index).
/// * ``scale``/``opacity`` are validated by ``blit_indexed`` (1..4, finite).
/// * ``x``/``y`` must be non-negative: HUD films are placed by screen
///   descriptors, so a negative origin is a caller bug.  Partial off-screen
///   placement is still available through ``blit_indexed`` directly.
bool draw(const HudFilm& film, RenderResult& target,
          const std::vector<std::uint8_t>& palette, int x, int y,
          bool transparent, int scale, double opacity, std::uint64_t tick,
          std::string& error);

} // namespace gm
