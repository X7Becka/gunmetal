// Effect animation frames (`M4.2.6`): the `0x41de7d` branch of the frame pass.
//
// A polygon whose texture index is **negative** does not use a level texture:
// `-k` selects entry `k-1` of the `.sbw` effect table (table 2), and the frame
// drawn is that entry's frame at the current time (docs/EFFECTS.md §1.2):
//
//     t = now_ticks - poly[+0x68]              (0x9fdf12)
//     mode 0 and t >= period -> frames[n-1]    (0x41decb / 0x41ded9)
//     otherwise              -> (t % period) * n / period   (0x41dee7..0x41df01)
//     mode not 0/1           -> frame 0        (0x41df18 -> 0x44dc50)
//
// `period` is in engine ticks (91 Hz), not centiseconds: the `.soa` view of the
// same record stores `period * 91 / 100` (docs/SOA.md §4).
#pragma once

#include "res/sbw.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gm {

/// Frame index selected by `0x41de7d`, or nullopt when the engine's answer is
/// not modelled: a negative tick (the original's `idiv` leaves a negative
/// remainder and reads *before* the frame array, which no shipped level
/// reaches) or `period == 0` in the looping mode (division by zero).
std::optional<std::size_t> effect_frame_index(std::uint32_t mode,
                                              std::uint32_t period,
                                              std::size_t frames,
                                              std::int64_t tick);

/// Entry for a polygon texture index: `texture < 0` -> `effects[-texture - 1]`;
/// `texture >= 0` or out of range -> nullptr.
const SbwEffectRef* effect_for_texture(const std::vector<SbwEffectRef>& effects,
                                       int texture);

/// Frame texture id (1-based) for a polygon at *now* ticks, given its
/// `poly[+0x68]` start tick.  Empty when the index is not modelled.
std::optional<std::uint16_t> effect_frame_texture(const SbwEffectRef& effect,
                                                  std::int64_t start_tick,
                                                  std::int64_t now_tick);

} // namespace gm
