#include "game/effect_anim.h"

namespace gm {

std::optional<std::size_t> effect_frame_index(std::uint32_t mode,
                                              std::uint32_t period,
                                              std::size_t frames,
                                              std::int64_t tick) {
    if (frames == 0 || tick < 0) {
        return std::nullopt;
    }
    // 0x41df18: any mode other than one-shot/cycle always renders frame 0
    // (100/101/102 additionally draw into that texture, docs/EFFECTS.md §1.2).
    if (mode != 0 && mode != 1) {
        return std::size_t{0};
    }
    if (mode == 0 && tick >= static_cast<std::int64_t>(period)) {
        return frames - 1; // 0x41decb / 0x41ded9: hold the last frame
    }
    if (period == 0) {
        // mode 1 would divide by zero; mode 0 always takes the branch above.
        return std::nullopt;
    }
    const std::uint64_t t = static_cast<std::uint64_t>(tick);
    return static_cast<std::size_t>(
        (t % period) * static_cast<std::uint64_t>(frames) / period);
}

const SbwEffectRef* effect_for_texture(const std::vector<SbwEffectRef>& effects,
                                       int texture) {
    if (texture >= 0) {
        return nullptr;
    }
    const long long index = -static_cast<long long>(texture) - 1;
    if (index < 0 || static_cast<std::size_t>(index) >= effects.size()) {
        return nullptr;
    }
    return &effects[static_cast<std::size_t>(index)];
}

std::optional<std::uint16_t> effect_frame_texture(const SbwEffectRef& effect,
                                                  std::int64_t start_tick,
                                                  std::int64_t now_tick) {
    const auto index =
        effect_frame_index(effect.mode, effect.period, effect.ids.size(),
                           now_tick - start_tick);
    if (!index || *index >= effect.ids.size()) {
        return std::nullopt;
    }
    return effect.ids[*index];
}

} // namespace gm
