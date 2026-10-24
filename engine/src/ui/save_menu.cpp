#include "ui/save_menu.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "res/save.h"

namespace gm {
namespace {

/// Same cap as `res/save.cpp` / `ui/save_slots.cpp`: a bigger file is not a
/// save and is never read into memory here.
constexpr std::uintmax_t kMaxSlotFileBytes = 64u * 1024u * 1024u;

/// The game opens `savegame\saved<N>.sbt` (lower case, `0x4ab460`); the shipped
/// install spells it `SAVEGAME`.  Both are accepted, upper first (same order as
/// `ui/save_slots.cpp`).
constexpr const char* kSaveDirs[] = {"SAVEGAME", "savegame"};

std::filesystem::path find_save_dir(const std::filesystem::path& game) {
    std::error_code ec;
    for (const char* name : kSaveDirs) {
        std::filesystem::path dir = game / name;
        ec.clear();
        if (std::filesystem::is_directory(dir, ec) && !ec) {
            return dir;
        }
    }
    return {};
}

bool read_slot_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out,
                    std::string& error) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "cannot stat " + path.string() + ": " + ec.message();
        return false;
    }
    if (size > kMaxSlotFileBytes) {
        error = path.string() + " is " + std::to_string(size) +
                " bytes, larger than any save (64 MiB)";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path.string();
        return false;
    }
    out.assign(static_cast<std::size_t>(size), 0);
    if (size != 0 &&
        !in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size))) {
        error = "cannot read " + path.string();
        return false;
    }
    return true;
}

/// Slot number as the refusal texts spell it.
std::string slot_text(int index) { return "save slot " + std::to_string(index); }

} // namespace

SaveMenu::SaveMenu() {
    slots_.assign(kSlots, SaveSlotView{});
    for (int i = 0; i < kSlots; ++i) {
        slots_[static_cast<std::size_t>(i)].index = i;
    }
    problems_.assign(kSlots, std::string{});
}

void SaveMenu::open(SaveMenuMode mode, const std::filesystem::path& game) {
    mode_ = mode;
    game_ = game;
    open_ = true;
    event_ = SaveMenuEvent::none;
    selected_ = 0;
    first_ = 0;
    error_.clear();
    directory_ = find_save_dir(game);

    slots_.assign(kSlots, SaveSlotView{});
    problems_.assign(kSlots, std::string{});
    for (int i = 0; i < kSlots; ++i) {
        slots_[static_cast<std::size_t>(i)].index = i;
    }
    if (directory_.empty()) {
        // No directory: 20 empty views, nothing selectable.  Listing stays
        // successful (like `SaveSlotList::refresh`), but there is nothing to
        // load and nowhere to save, so `activate()` refuses with a text.
        place_initial_selection();
        return;
    }

    for (int i = 0; i < kSlots; ++i) {
        SaveSlotView& view = slots_[static_cast<std::size_t>(i)];
        const std::filesystem::path path =
            directory_ / ("saved" + std::to_string(i) + ".sbt");

        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec) {
            view.selectable = mode == SaveMenuMode::save && i <= kLastReachable;
            continue; // missing (or not a file) == empty slot
        }

        std::vector<std::uint8_t> bytes;
        std::string problem;
        std::optional<Save> save;
        if (!read_slot_file(path, bytes, problem)) {
            problems_[static_cast<std::size_t>(i)] = std::move(problem);
        } else {
            std::string parse_error;
            save = parse_save(bytes, parse_error);
            if (!save) {
                problems_[static_cast<std::size_t>(i)] = path.string() + ": " + parse_error;
            }
        }
        if (!save) {
            if (error_.empty()) {
                error_ = problems_[static_cast<std::size_t>(i)];
            }
            // An unreadable slot is empty for a load but can be overwritten.
            view.selectable = mode == SaveMenuMode::save && i <= kLastReachable;
            continue;
        }

        view.occupied = true;
        view.name = save->header.title;
        view.level = save->header.level_name;
        // F1 (+0xa8) is the level clock; F20 (+0xf4) is the proven
        // "playtime in seconds = F1 / 91" (0x45c057, docs/SAVE_FIELDS.md §2).
        view.seconds = static_cast<int>(save->header.fields[1] / kTicksPerSecond);
        view.selectable = i <= kLastReachable; // occupied: both modes
    }
    place_initial_selection();
}

void SaveMenu::place_initial_selection() {
    selected_ = 0;
    for (int i = 0; i <= kLastReachable; ++i) {
        if (slots_[static_cast<std::size_t>(i)].selectable) {
            selected_ = i;
            break;
        }
    }
    first_ = 0;
    reveal();
}

void SaveMenu::reveal() {
    if (selected_ < first_) {
        first_ = selected_;
    } else if (selected_ > first_ + kVisible - 1) {
        first_ = selected_ - (kVisible - 1);
    }
    first_ = std::clamp(first_, 0, kMaxFirst);
}

void SaveMenu::move(int delta) {
    if (!open_ || delta == 0) {
        return;
    }
    const int direction = delta > 0 ? 1 : -1;
    // `-delta` of INT_MIN is undefined; a journey longer than the table is
    // just "to the end", so cap the step count at kSlots.
    const long long magnitude =
        delta > 0 ? static_cast<long long>(delta) : -static_cast<long long>(delta);
    const int steps = static_cast<int>(std::min<long long>(magnitude, kSlots));

    for (int step = 0; step < steps; ++step) {
        int next = selected_;
        for (int candidate = selected_ + direction;
             candidate >= 0 && candidate <= kLastReachable; candidate += direction) {
            if (slots_[static_cast<std::size_t>(candidate)].selectable) {
                next = candidate;
                break;
            }
        }
        if (next == selected_) {
            break; // edge of the selectable range: clamp, never wrap
        }
        selected_ = next;
        reveal();
    }
}

void SaveMenu::escape() {
    event_ = SaveMenuEvent::back;
}

bool SaveMenu::activate(std::string* error) {
    event_ = SaveMenuEvent::none;
    if (error != nullptr) {
        error->clear();
    }
    const auto fail = [error](std::string text) {
        if (error != nullptr) {
            *error = std::move(text);
        }
        return false;
    };

    if (!open_) {
        return fail("save menu is not open");
    }
    const SaveSlotView& slot = slots_[static_cast<std::size_t>(selected_)];

    if (mode_ == SaveMenuMode::load) {
        if (!slot.occupied) {
            const std::string& problem = problems_[static_cast<std::size_t>(selected_)];
            if (!problem.empty()) {
                return fail(slot_text(selected_) + " cannot be loaded: " + problem);
            }
            return fail(slot_text(selected_) + " is empty");
        }
        event_ = SaveMenuEvent::load;
        return true;
    }

    // save
    if (directory_.empty()) {
        return fail("cannot save: no SAVEGAME directory under " + game_.string());
    }
    if (!slot.selectable) {
        return fail(slot_text(selected_) + " is not reachable");
    }
    // Occupied or not: the original has no overwrite confirmation (§5.2); the
    // caller decides what to show and `slot.occupied` says whether it is a
    // rewrite.  No file is written here.
    event_ = SaveMenuEvent::save;
    return true;
}

const SaveSlotView& SaveMenu::at(int index) const {
    const int clamped = std::clamp(index, 0, kSlots - 1);
    return slots_[static_cast<std::size_t>(clamped)];
}

std::string_view SaveMenu::label(int row) const {
    if (row < 0 || row >= kVisible) {
        return {};
    }
    const int index = first_ + row;
    if (index < 0 || index >= kSlots) {
        return {};
    }
    const SaveSlotView& slot = slots_[static_cast<std::size_t>(index)];
    return slot.occupied ? std::string_view(slot.name) : kEmptyLabel;
}

} // namespace gm
