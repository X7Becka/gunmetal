#include "render/fx_frame.h"

#include "game/effect_anim.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <utility>

namespace gm::fx {
namespace {

void fail(std::string* error, std::string text) {
    if (error != nullptr) {
        *error = std::move(text);
    }
}

/// The setup reads the index with `movsx` (`0x9fdefc`).
int signed_index(std::uint16_t value) {
    return static_cast<std::int16_t>(value);
}

bool ascii_iequal(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(left[i])) !=
            std::toupper(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

bool SbmCatalog::contains(int kind, const std::string& name) const {
    const std::vector<std::string>* archive = nullptr;
    if (kind == 0) {
        archive = &pics;
    } else if (kind == 1) {
        archive = &sprites;
    } else {
        return false;
    }
    return std::any_of(archive->begin(), archive->end(),
                       [&name](const std::string& entry) { return ascii_iequal(entry, name); });
}

const char* SbmCatalog::archive_name(int kind) const {
    if (kind == 0) {
        return "PICS";
    }
    if (kind == 1) {
        return "sprites";
    }
    return "";
}

SoaCatalog make_soa_catalog(const SbwLevel& world, std::string level) {
    SoaCatalog catalog;
    catalog.level = std::move(level);
    catalog.effects = world.header.effects;
    catalog.textures.reserve(world.header.textures.size());
    for (const SbwTexRef& texture : world.header.textures) {
        FxTextureRef ref;
        ref.name = texture.name;
        // loader 0x42ab8e stores `kind` at +0x05; .sbw keeps it in the high
        // byte of the flag word (docs/EFFECTS.md §2.3).
        ref.kind = static_cast<int>(texture.flag >> 8);
        catalog.textures.push_back(std::move(ref));
    }
    for (const SbwGeometry& geometry : world.geometry) {
        for (const SbwPoly& poly : geometry.polys) {
            if (poly.indices.size() <= 2) {
                continue; // 0x9fde93: n_idx <= 2 is skipped before the index
            }
            const int index =
                poly.handle < 1 ? signed_index(poly.texture) : signed_index(poly.overlay);
            if (index < 0) {
                catalog.negative_indices.push_back(index);
            }
        }
    }
    return catalog;
}

std::optional<FxFrameRef> resolve_fx_frame(int negative_index, const SoaCatalog& soa,
                                           const SbmCatalog& sbm, std::string* error,
                                           std::int64_t now_tick) {
    if (error != nullptr) {
        error->clear();
    }
    if (negative_index >= 0) {
        fail(error, "texture index " + std::to_string(negative_index) +
                        " is not an effect id: only -k < 0 selects a frame");
        return std::nullopt;
    }
    if (soa.effects.empty()) {
        fail(error, "empty effect table: the .sbw has no record to resolve " +
                        std::to_string(negative_index) + " against");
        return std::nullopt;
    }
    if (sbm.empty()) {
        fail(error, "empty SBM catalog: no PICS/SBM.IDX or sprites/SBM.IDX entries "
                    "to verify the frame texture against");
        return std::nullopt;
    }
    const SbwEffectRef* effect = effect_for_texture(soa.effects, negative_index);
    if (effect == nullptr) {
        fail(error, "no effect entry for " + std::to_string(negative_index) + ": the .sbw "
                    "table has " + std::to_string(soa.effects.size()) + " record(s)");
        return std::nullopt;
    }
    const int series_index = static_cast<int>(-static_cast<long long>(negative_index) - 1);
    if (effect->ids.empty()) {
        fail(error, "broken effect record " + effect->name + " (" +
                        std::to_string(negative_index) + "): no frame ids");
        return std::nullopt;
    }
    const std::optional<std::size_t> frame =
        effect_frame_index(effect->mode, effect->period, effect->ids.size(), now_tick);
    if (!frame) {
        fail(error, "effect " + effect->name + " has no frame at tick " +
                        std::to_string(now_tick) + " (mode " + std::to_string(effect->mode) +
                        ", period " + std::to_string(effect->period) + ")");
        return std::nullopt;
    }
    if (*frame >= effect->ids.size()) {
        fail(error, "effect " + effect->name + ": frame index " + std::to_string(*frame) +
                        " outside its " + std::to_string(effect->ids.size()) + " frame ids");
        return std::nullopt;
    }
    const std::uint16_t id = effect->ids[*frame];
    if (id == 0) {
        fail(error, "effect " + effect->name + ": frame " + std::to_string(*frame) +
                        " has texture id 0 (nothing to draw)");
        return std::nullopt;
    }
    if (static_cast<std::size_t>(id) > soa.textures.size()) {
        fail(error, "effect " + effect->name + ": frame id " + std::to_string(id) +
                        " outside the texture registry (" +
                        std::to_string(soa.textures.size()) + " textures)");
        return std::nullopt;
    }
    const FxTextureRef& texture = soa.textures[static_cast<std::size_t>(id) - 1];
    if (texture.kind != 0 && texture.kind != 1) {
        fail(error, "effect " + effect->name + ": frame texture " + texture.name +
                        " has kind " + std::to_string(texture.kind) +
                        "; only pics(0) and sprites(1) are documented for effect frames");
        return std::nullopt;
    }
    if (texture.name.empty()) {
        fail(error, "effect " + effect->name + ": texture registry entry " +
                        std::to_string(id) + " has an empty name");
        return std::nullopt;
    }
    if (!sbm.contains(texture.kind, texture.name)) {
        fail(error, "effect " + effect->name + ": frame texture " + texture.name +
                        " is not in " + sbm.archive_name(texture.kind) + "/SBM.IDX");
        return std::nullopt;
    }

    FxFrameRef ref;
    ref.series_index = series_index;
    ref.series_name = effect->name;
    ref.frame = static_cast<int>(*frame);
    ref.frame_count = static_cast<int>(effect->ids.size());
    ref.animated = effect->mode == 1 && effect->ids.size() > 1;
    return ref;
}

std::vector<FxFrameRef> frames_for_level(const SoaCatalog& soa, const SbmCatalog& sbm,
                                         std::int64_t now_tick,
                                         std::vector<std::string>* errors) {
    if (errors != nullptr) {
        errors->clear();
    }
    std::vector<int> indices = soa.negative_indices;
    std::sort(indices.begin(), indices.end(), std::greater<int>()); // -1, -2, ...
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

    std::vector<FxFrameRef> frames;
    frames.reserve(indices.size());
    for (const int index : indices) {
        std::string error;
        if (auto ref = resolve_fx_frame(index, soa, sbm, &error, now_tick)) {
            frames.push_back(std::move(*ref));
        } else if (errors != nullptr) {
            errors->push_back(std::move(error));
        }
    }
    return frames;
}

int draw_order_rank(FxFrameKey key) {
    // §4.2: one pass, polygon order; material/lightmap are not an ordering key
    // for the effect branch.  Keep the parameter read so an unused warning does
    // not suggest the key is free to grow.
    (void)key;
    return kFxSinglePass;
}

} // namespace gm::fx
