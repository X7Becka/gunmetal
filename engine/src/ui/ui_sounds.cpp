#include "ui/ui_sounds.h"

#include <array>
#include <cctype>

namespace gm {
namespace {

constexpr std::array<UiSoundSample, 6> kSamples = {{
    {UiSound::up_down, "updown", 581},   // §1.1 `0x48f780`, §1.2 idx 581
    {UiSound::select, "select", 510},    // §1.1 `0x48f7a8`, §1.2 idx 510
    {UiSound::select_m, "selectm", 511}, // §1.1 `0x48f85e`, §1.2 idx 511
    {UiSound::menu_on, "menuon", 425},   // §1.1 `0x48f835`, §1.2 idx 425
    {UiSound::v_left, "vleft", 804},     // §1.1 `0x48f7cc`, §1.2 idx 804
    {UiSound::back, "vback", 636},       // §1.1 `0x48f814`, §1.2 idx 636
}};

bool same_name(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto left = static_cast<unsigned char>(a[i]);
        const auto right = static_cast<unsigned char>(b[i]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

const UiSoundSample* sample_for(UiSound sound) {
    for (const UiSoundSample& row : kSamples) {
        if (row.sound == sound) {
            return &row;
        }
    }
    return nullptr;
}

UiSoundEvent event_for(UiSound sound) {
    UiSoundEvent event;
    if (const UiSoundSample* row = sample_for(sound)) {
        event.sound = row->sound;
        event.name = row->name;
        event.table_index = row->table_index;
    }
    return event;
}

/// §2.3: navigation sounds exist on the §2 screens 1..37, and screen 16
/// (`KEY JAMMING`) is excluded because the key is data there.
bool audible_screen(Screen screen) {
    const int id = FrontendMenu::screen_id(screen);
    return id >= 1 && id <= 37 && id != 16;
}

/// Item ids of the SOUND OPTIONS / GRAPHICS OPTIONS rows the doc singles out
/// (docs/UI_SOUNDS.md §2.3 and the item tables in docs/UI_SCREENS.md §3).
enum SoundItem { kMusicVolume = 0, kEffectsVolume = 2, kSwitchLeftRight = 4 };
enum GraphicsItem { kBorder = 1 };

/// One event, with the highlighted item id when the caller knows it (-1 = not
/// known).  Item-specific branches fall back to the proved default `select`.
std::optional<UiSoundEvent> primary_event(const KeyEvent& key, Screen from,
                                          Screen /*to*/, bool moved, int item) {
    switch (key.kind) {
    case KeyEvent::Up:
    case KeyEvent::Down:
        if (!audible_screen(from) || !moved) {
            return std::nullopt;
        }
        return event_for(UiSound::up_down);

    case KeyEvent::Left:
    case KeyEvent::Right:
        if (!audible_screen(from)) {
            return std::nullopt;
        }
        if (from == Screen::SoundOptions) {
            // `Switch Left Right`: vleft only, select suppressed (0x48c6c0).
            if (item == kSwitchLeftRight) {
                return event_for(UiSound::v_left);
            }
            // Volume rows: updown only when the value really changed
            // (0x48be88/0x48be93, 0x48c717/0x48c722); the limit is silent.
            if (item == kMusicVolume || item == kEffectsVolume) {
                return moved ? std::optional<UiSoundEvent>(event_for(UiSound::up_down))
                             : std::nullopt;
            }
        }
        if (from == Screen::GraphicsOptions && item == kBorder) {
            return event_for(UiSound::up_down); // 0x48c7a8
        }
        // Default horizontal event 2 (0x48bd79); Gamma/Reflections/Light Maps
        // keep it (0x48c7c9..0x48c7e4).
        return event_for(UiSound::select);

    case KeyEvent::Enter:
        // Every Enter on an audible screen plays select now; selectm follows
        // on the next frame (§2.4) and is added by the plan.
        if (!audible_screen(from)) {
            return std::nullopt;
        }
        return event_for(UiSound::select);

    case KeyEvent::Escape: // §2.3: Esc writes no event.
    case KeyEvent::Backspace:
    case KeyEvent::Text:
    case KeyEvent::Other:
    default:
        return std::nullopt;
    }
}

UiSoundPlan plan_for(const FrontendMenu& menu, const KeyEvent& key, int item,
                     std::optional<bool> moved) {
    UiSoundPlan plan;
    const Screen from = menu.screen();
    // `to` is only needed for the model `menu_on` edge; a screen change via
    // Enter is the observable "entered a submenu".
    FrontendMenu probe = menu;
    const bool changed = probe.feed(key);
    const Screen to = probe.screen();
    // Without a host observation only a key the automaton handled counts as a
    // move (volume values live outside `FrontendMenu`, see the header).
    const bool observed_move = moved.value_or(changed && to == from);

    const auto primary = primary_event(key, from, to, observed_move, item);
    if (!primary) {
        return plan;
    }
    plan.events.push_back(*primary);

    if (key.kind == KeyEvent::Enter && primary->sound == UiSound::select) {
        // §2.4: the 27 ms highlight callback 0x486700 plays selectm on every
        // Enter, one frame after select.  Proved, not a model.
        plan.events.push_back(event_for(UiSound::select_m));
        // Model (docs/UI_SOUNDS_STATE.md): entering another screen plays
        // menuon.  The original's menuon is the title-art callback, so this
        // edge is our invention and is marked as such.
        if (changed && to != from && audible_screen(to)) {
            plan.events.push_back(event_for(UiSound::menu_on));
        }
    }
    return plan;
}

} // namespace

std::span<const UiSoundSample> ui_sound_samples() { return kSamples; }

const UiSoundSample* ui_sound_sample(UiSound sound) { return sample_for(sound); }

std::optional<UiSoundEvent> ui_sound_for(const KeyEvent& key, Screen from,
                                         Screen to, bool moved) {
    // No item id: item-specific Left/Right overrides are out of reach here and
    // fall back to the proved default (`select`); `ui_sounds_for` knows more.
    return primary_event(key, from, to, moved, -1);
}

UiSoundPlan ui_sounds_for(const FrontendMenu& menu, const KeyEvent& key,
                          std::optional<bool> moved) {
    return plan_for(menu, key, menu.selected_id(), moved);
}

int table_slot(const SoundSampleTable& table, std::string_view name) {
    if (name.empty()) {
        return -1;
    }
    for (std::size_t i = 0; i < SoundSampleTable::kSlots; ++i) {
        if (table.counts[i] == 0 || table.names[i].empty()) {
            continue;
        }
        if (same_name(table.names[i], name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool table_has(const SoundSampleTable& table, int index) {
    if (index < 0) {
        return false;
    }
    for (const UiSoundSample& row : kSamples) {
        if (row.table_index != index) {
            continue;
        }
        return table_slot(table, row.name) >= 0;
    }
    return false;
}

} // namespace gm
