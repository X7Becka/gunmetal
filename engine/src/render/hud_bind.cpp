#include "render/hud_bind.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <string>

namespace gm {
namespace {

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

std::string upper_copy(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

std::string_view basename_of(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/// ``<I ...>`` requires one of the shipped alignment letters (docs/UI.md §5.2).
bool parse_anchor(std::optional<char> align, HudAnchor& out, std::string& error,
                  std::size_t line) {
    if (!align) {
        out = HudAnchor::None;
        return true;
    }
    if (*align == 'l') {
        out = HudAnchor::Left;
        return true;
    }
    if (*align == 'r') {
        out = HudAnchor::Right;
        return true;
    }
    error = "hud_bind: <I> line " + std::to_string(line) + ": unknown alignment '" +
            std::string(1, *align) + "' (expected 'l' or 'r')";
    return false;
}

/// Accumulate ``<FAMILY><digits>``; false when the tail is empty (family only)
/// or not a plain decimal that fits an int.
bool parse_id_tail(std::string_view rest, bool& has_id, int& id) {
    has_id = false;
    id = 0;
    if (rest.empty()) {
        return true;
    }
    for (const char c : rest) {
        if (!is_digit(c)) {
            return false;
        }
    }
    int value = 0;
    for (const char c : rest) {
        const int digit = c - '0';
        if (value > (INT_MAX - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    has_id = true;
    id = value;
    return true;
}

struct FamilyPrefix {
    std::string_view prefix;
    HudFamily family;
};

constexpr FamilyPrefix kFamilyPrefixes[] = {
    {"WEAPON", HudFamily::Weapon}, {"SHIELD", HudFamily::Shield},
    {"EQUIP", HudFamily::Equip},   {"TANK", HudFamily::Tank},
    {"AMMO", HudFamily::Ammo},
};

void clip_placement(HudPlacement& placement, const RenderResult& target) {
    const int right = placement.x + placement.width;
    const int bottom = placement.y + placement.height;
    const int x0 = std::max(placement.x, 0);
    const int y0 = std::max(placement.y, 0);
    const int x1 = std::min(right, target.width);
    const int y1 = std::min(bottom, target.height);
    placement.clipped =
        placement.x < 0 || placement.y < 0 || right > target.width || bottom > target.height;
    placement.visible = x1 > x0 && y1 > y0;
    placement.clip_width = placement.visible ? x1 - x0 : 0;
    placement.clip_height = placement.visible ? y1 - y0 : 0;
}

} // namespace

std::string_view hud_family_name(HudFamily family) {
    switch (family) {
    case HudFamily::Default: return "default";
    case HudFamily::Weapon: return "weapon";
    case HudFamily::Ammo: return "ammo";
    case HudFamily::Tank: return "tank";
    case HudFamily::Shield: return "shield";
    case HudFamily::Equip: return "equip";
    }
    return "default";
}

std::string_view hud_family_prefix(HudFamily family) {
    switch (family) {
    case HudFamily::Default: return "DEFAULT";
    case HudFamily::Weapon: return "WEAPON";
    case HudFamily::Ammo: return "AMMO";
    case HudFamily::Tank: return "TANK";
    case HudFamily::Shield: return "SHIELD";
    case HudFamily::Equip: return "EQUIP";
    }
    return "DEFAULT";
}

std::string_view hud_basis_name(HudBasis basis) {
    switch (basis) {
    case HudBasis::Proven: return "proven";
    case HudBasis::Hypothesis: return "hypothesis";
    }
    return "hypothesis";
}

std::string_view hud_anchor_name(HudAnchor anchor) {
    switch (anchor) {
    case HudAnchor::None: return "none";
    case HudAnchor::Left: return "left";
    case HudAnchor::Right: return "right";
    }
    return "none";
}

HudScreenClass classify_screen(std::string_view screen) {
    HudScreenClass out;
    std::string_view base = basename_of(screen);
    const std::size_t dot = base.find_last_of('.');
    if (dot != std::string_view::npos && dot != 0) {
        base = base.substr(0, dot);
    }
    out.stem = upper_copy(base);

    if (out.stem == "DEFAULT") {
        // ``Default.sof`` is the one film without digits; the loader maps it to
        // line 0 (0x49597c).
        out.family = HudFamily::Default;
        out.has_id = true;
        out.id = 0;
        out.basis = HudBasis::Proven;
        return out;
    }

    for (const auto& entry : kFamilyPrefixes) {
        if (out.stem.size() < entry.prefix.size() ||
            out.stem.compare(0, entry.prefix.size(), entry.prefix) != 0) {
            continue;
        }
        const std::string_view rest(out.stem.data() + entry.prefix.size(),
                                    out.stem.size() - entry.prefix.size());
        bool has_id = false;
        int id = 0;
        if (!parse_id_tail(rest, has_id, id)) {
            break; // e.g. "WEAPONS" is not a family screen
        }
        out.family = entry.family;
        out.has_id = has_id;
        out.id = id;
        // The prefix literals and the "%s%ld" -> index-line binding are xref'd
        // (0x4958d0); that the *page* is the family page is the hypothesis
        // spelled out in resolve_film.
        out.basis = HudBasis::Proven;
        return out;
    }

    // ``TEMP0.SML`` is the garage template: the page that calls
    // film_set("WEAPON", ...) (0x496d82) but owns no film family.
    if (out.stem.rfind("TEMP", 0) == 0) {
        const std::string_view rest(out.stem.data() + 4, out.stem.size() - 4);
        bool has_id = false;
        int id = 0;
        if (parse_id_tail(rest, has_id, id) && has_id) {
            out.family = HudFamily::Default;
            out.basis = HudBasis::Hypothesis;
            return out;
        }
    }

    out.family = HudFamily::Default;
    out.has_id = false;
    out.id = 0;
    out.basis = HudBasis::Hypothesis;
    return out;
}

std::optional<HudBinding> parse_binding(const SmlDocument& doc, std::string_view screen,
                                        std::string& error) {
    error.clear();
    if (doc.text.empty()) {
        error = "hud_bind: empty document";
        return std::nullopt;
    }
    if (doc.tags.empty()) {
        error = "hud_bind: document has no tags";
        return std::nullopt;
    }
    if (!doc.doc_open) {
        error = "hud_bind: document does not start with <d>";
        return std::nullopt;
    }
    if (!doc.doc_close) {
        error = "hud_bind: document does not end with </d>";
        return std::nullopt;
    }
    if (!doc.brackets_balanced) {
        error = "hud_bind: bracket imbalance";
        return std::nullopt;
    }
    if (doc.too_many_tags || doc.tags.size() >= kSmlMaxTags) {
        error = "hud_bind: tag limit exceeded";
        return std::nullopt;
    }

    HudBinding binding;
    if (!screen.empty()) {
        binding.screen.assign(screen.data(), screen.size());
    }
    binding.screen_class = classify_screen(screen);

    for (const auto& element : doc.elements) {
        if (element.kind != SmlKind::Image) {
            continue;
        }
        if (binding.images.size() >= kHudBindMaxImages) {
            error = "hud_bind: more than " + std::to_string(kHudBindMaxImages) +
                    " images (line " + std::to_string(element.line) + ")";
            return std::nullopt;
        }

        HudElementBinding image;
        image.index = binding.images.size();
        image.line = element.line;
        if (element.value.empty()) {
            error = "hud_bind: <I> line " + std::to_string(element.line) +
                    " has no f= image name";
            return std::nullopt;
        }
        image.image = element.value;

        if (!parse_anchor(element.align, image.anchor, error, element.line)) {
            return std::nullopt;
        }

        const std::string* raw_x = sml_attribute(element, "x");
        const std::string* raw_y = sml_attribute(element, "y");
        if (raw_x != nullptr && !element.x) {
            error = "hud_bind: <I> line " + std::to_string(element.line) + ": x=\"" +
                    *raw_x + "\" is not an integer";
            return std::nullopt;
        }
        if (raw_y != nullptr && !element.y) {
            error = "hud_bind: <I> line " + std::to_string(element.line) + ": y=\"" +
                    *raw_y + "\" is not an integer";
            return std::nullopt;
        }
        if (!element.y) {
            error = "hud_bind: <I> line " + std::to_string(element.line) +
                    " has no y= coordinate";
            return std::nullopt;
        }
        if (!element.x && image.anchor == HudAnchor::None) {
            error = "hud_bind: <I> line " + std::to_string(element.line) +
                    " has no x= coordinate and no a= anchor";
            return std::nullopt;
        }

        image.x = element.x.value_or(0);
        image.y = *element.y;
        if (image.x < -kHudBindMaxCoord || image.x > kHudBindMaxCoord ||
            image.y < -kHudBindMaxCoord || image.y > kHudBindMaxCoord) {
            error = "hud_bind: <I> line " + std::to_string(element.line) +
                    " coordinate outside +-" + std::to_string(kHudBindMaxCoord);
            return std::nullopt;
        }

        // With an anchor the edge wins and x becomes a reserved attribute; that
        // precedence is the documented hypothesis (docs/UI.md §5.2/§10.2).
        image.position_basis = image.anchor == HudAnchor::None ? HudBasis::Proven
                                                               : HudBasis::Hypothesis;
        image.x_reserved = image.anchor != HudAnchor::None;
        binding.images.push_back(std::move(image));
    }

    return binding;
}

int HudState::slot(HudFamily family) const {
    switch (family) {
    case HudFamily::Weapon: return weapon;
    case HudFamily::Ammo: return ammo;
    case HudFamily::Tank: return tank;
    case HudFamily::Shield: return shield;
    case HudFamily::Equip: return equip;
    case HudFamily::Default: return 0;
    }
    return -1;
}

std::optional<HudFilmRef> resolve_film(const HudBinding& binding, const HudState& state,
                                       const std::filesystem::path& game,
                                       std::string& error) {
    error.clear();
    const HudScreenClass& screen = binding.screen_class;

    HudFamily family = HudFamily::Default;
    int id = 0;
    HudBasis basis = HudBasis::Hypothesis;
    std::string note;

    if (screen.has_id) {
        family = screen.family;
        id = screen.id;
        basis = HudBasis::Proven;
        note = "screen name " + screen.stem;
    } else {
        const int screen_slot =
            screen.family == HudFamily::Default ? -1 : state.slot(screen.family);
        if (screen.family != HudFamily::Default && screen_slot >= 0) {
            family = screen.family;
            id = screen_slot;
            note = "screen family " + std::string(hud_family_name(family)) +
                   " from state";
        } else if (state.focus != HudFamily::Default && state.slot(state.focus) >= 0) {
            family = state.focus;
            id = state.slot(state.focus);
            note = "state focus " + std::string(hud_family_name(family));
        } else {
            family = HudFamily::Default;
            id = 0;
            note = "fallback Default.sof";
        }
    }

    HudFilmRef ref;
    ref.family = family;
    ref.basis = basis;
    ref.id = id;
    // The fallback file has no digits; the loader's canonical spelling is
    // ``Default`` (0x4bb344), not the upper-case prefix.
    ref.name = family == HudFamily::Default
                   ? std::string("Default")
                   : std::string(hud_family_prefix(family)) + std::to_string(id);
    ref.note = std::move(note);

    std::string load_error;
    auto film = load_hud_film(game, ref.name, load_error);
    if (!film) {
        error = "hud_bind: " + ref.name + ": " + load_error;
        return std::nullopt;
    }
    ref.film = std::move(*film);
    return ref;
}

std::optional<std::vector<HudPlacement>> layout(const HudBinding& binding,
                                               const HudFilm& film,
                                               const RenderResult& target,
                                               std::string& error) {
    error.clear();
    if (target.width <= 0 || target.height <= 0 || target.width > 8192 ||
        target.height > 8192) {
        error = "hud_bind: target " + std::to_string(target.width) + "x" +
                std::to_string(target.height) + " is outside 1..8192";
        return std::nullopt;
    }
    const std::size_t expected = static_cast<std::size_t>(target.width) *
                                 static_cast<std::size_t>(target.height) * 3u;
    if (target.rgb.size() != expected) {
        error = "hud_bind: target buffer is " + std::to_string(target.rgb.size()) +
                " bytes, expected " + std::to_string(expected);
        return std::nullopt;
    }
    // Same clock/count validation as the player; layout adds no frame choice.
    if (!frame_at(film, 0, error)) {
        return std::nullopt;
    }
    if (binding.images.size() > kHudBindMaxImages) {
        error = "hud_bind: more than " + std::to_string(kHudBindMaxImages) + " images";
        return std::nullopt;
    }

    std::vector<HudPlacement> placements;
    placements.reserve(std::max<std::size_t>(binding.images.size(), 1));

    if (binding.images.empty()) {
        // Full-page screen: the film is the HUD overlay of 0x4958d0.
        HudPlacement overlay;
        overlay.index = 0;
        overlay.anchor = HudAnchor::None;
        overlay.basis = HudBasis::Proven;
        overlay.x = kHudBindFilmX;
        overlay.y = kHudBindFilmY;
        overlay.width = kSofTileWidth;
        overlay.height = kSofTileHeight;
        clip_placement(overlay, target);
        placements.push_back(overlay);
        return placements;
    }

    for (const auto& image : binding.images) {
        HudPlacement placement;
        placement.index = image.index;
        placement.image = image.image;
        placement.anchor = image.anchor;
        placement.basis = image.position_basis;
        placement.width = kSofTileWidth;
        placement.height = kSofTileHeight;

        switch (image.anchor) {
        case HudAnchor::Left: placement.x = 0; break;
        case HudAnchor::Right: placement.x = target.width - kSofTileWidth; break;
        case HudAnchor::None: placement.x = image.x; break;
        }
        placement.y = image.y;

        clip_placement(placement, target);
        placements.push_back(std::move(placement));
    }
    return placements;
}

std::optional<std::vector<HudPlacement>> layout(const SmlDocument& doc,
                                               const HudFilm& film,
                                               const RenderResult& target,
                                               std::string& error) {
    const auto binding = parse_binding(doc, std::string_view(), error);
    if (!binding) {
        return std::nullopt;
    }
    return layout(*binding, film, target, error);
}

} // namespace gm
