#include "ui/save_slots.h"

#include <algorithm>
#include <cctype>
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

/// Same cap as `res/save.cpp` (`kMaxSaveBytes`): a bigger file is not a save and
/// is never read into memory here.
constexpr std::uintmax_t kMaxSlotFileBytes = 64u * 1024u * 1024u;

/// The game writes `savegame\saved<N>.sbt` (lower case, `0x4ab460`); the shipped
/// install uses `SAVEGAME` on disk.  Both spellings are accepted, upper first.
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

/// `"LEVEL0"` -> 0, `"level12"` -> 12; anything else (including `"LEVEL"`) -> -1.
int level_number(std::string_view name) {
    constexpr std::string_view kPrefix = "LEVEL";
    if (name.size() <= kPrefix.size()) {
        return -1;
    }
    for (std::size_t i = 0; i < kPrefix.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(name[i])) !=
            static_cast<unsigned char>(kPrefix[i])) {
            return -1;
        }
    }
    int value = 0;
    for (std::size_t i = kPrefix.size(); i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!std::isdigit(c)) {
            return -1;
        }
        if (value > 100000) { // keep the parse total and obvious
            return -1;
        }
        value = value * 10 + (c - '0');
    }
    return value;
}

} // namespace

SaveSlotList::SaveSlotList() { clear(); }

void SaveSlotList::clear() {
    slots_.assign(kSlots, SaveSlot{});
    for (int i = 0; i < kSlots; ++i) {
        slots_[static_cast<std::size_t>(i)].index = i;
    }
    scroll_ = 0;
    selected_ = 0;
    pending_ = -1;
    restore_selected_ = 0;
}

void SaveSlotList::refresh(const std::filesystem::path& game, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    clear();

    const std::filesystem::path dir = find_save_dir(game);
    if (dir.empty()) {
        return; // no save directory: an empty list, not an error
    }

    for (int i = 0; i < kSlots; ++i) {
        const std::filesystem::path path =
            dir / ("saved" + std::to_string(i) + ".sbt");
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec) {
            continue; // missing (or not a file) == empty slot
        }

        std::vector<std::uint8_t> bytes;
        std::string problem;
        if (!read_slot_file(path, bytes, problem)) {
            if (error != nullptr && error->empty()) {
                *error = std::move(problem);
            }
            continue;
        }

        std::string parse_error;
        const std::optional<Save> save = parse_save(bytes, parse_error);
        if (!save) {
            if (error != nullptr && error->empty()) {
                *error = path.string() + ": " + parse_error;
            }
            continue;
        }

        SaveSlot& slot = slots_[static_cast<std::size_t>(i)];
        slot.present = true;
        slot.name = save->header.title;
        slot.level = level_number(save->header.level_name);
        slot.mission = static_cast<int>(save->header.fields[2]); // F2 at +0xac
        slot.detail = save->header.level_name + ", mission " + std::to_string(slot.mission);
    }
}

int SaveSlotList::present_count() const {
    int count = 0;
    for (const SaveSlot& slot : slots_) {
        if (slot.present) {
            ++count;
        }
    }
    return count;
}

const SaveSlot& SaveSlotList::at(int index) const {
    const int clamped = std::clamp(index, 0, kSlots - 1);
    return slots_[static_cast<std::size_t>(clamped)];
}

bool SaveSlotList::scroll_down() {
    if (pending_ >= 0 || scroll_ >= kMaxScroll) {
        return false;
    }
    ++scroll_;
    selected_ = std::clamp(selected_, scroll_, scroll_ + kVisible - 1);
    return true;
}

bool SaveSlotList::scroll_up() {
    if (pending_ >= 0 || scroll_ <= 0) {
        return false;
    }
    --scroll_;
    selected_ = std::clamp(selected_, scroll_, scroll_ + kVisible - 1);
    return true;
}

bool SaveSlotList::select_down() {
    if (pending_ >= 0) {
        return false;
    }
    if (selected_ >= scroll_ + kVisible - 1) {
        if (scroll_ >= kMaxScroll) {
            return false; // bottom of the 13..18 window, slot 19 is unreachable
        }
        ++scroll_;
        ++selected_;
        return true;
    }
    ++selected_;
    return true;
}

bool SaveSlotList::select_up() {
    if (pending_ >= 0) {
        return false;
    }
    if (selected_ <= scroll_) {
        if (scroll_ <= 0) {
            return false;
        }
        --scroll_;
        --selected_;
        return true;
    }
    --selected_;
    return true;
}

bool SaveSlotList::select(int slot) {
    if (pending_ >= 0 || slot < 0 || slot >= kSlots) {
        return false;
    }
    const int previous_scroll = scroll_;
    const int previous_selected = selected_;
    place_selection(slot);
    // Slot 19 cannot be brought inside [scroll, scroll+5] with scroll <= 13;
    // the original's bound has the same hole (docs/SAVE_SLOTS.md §4).
    if (selected_ < scroll_ || selected_ > scroll_ + kVisible - 1) {
        scroll_ = previous_scroll;
        selected_ = previous_selected;
        return false;
    }
    return true;
}

void SaveSlotList::place_selection(int slot) {
    if (slot < scroll_) {
        scroll_ = slot;
    } else if (slot > scroll_ + kVisible - 1) {
        scroll_ = slot - (kVisible - 1);
    }
    scroll_ = std::clamp(scroll_, 0, kMaxScroll);
    selected_ = slot;
}

std::string_view SaveSlotList::item_label(int row) const {
    if (row < 0 || row >= kVisible) {
        return {};
    }
    const int slot = scroll_ + row;
    if (slot < 0 || slot >= kSlots) {
        return {};
    }
    const SaveSlot& entry = slots_[static_cast<std::size_t>(slot)];
    return entry.present ? std::string_view(entry.name) : kEmptyLabel;
}

bool SaveSlotList::needs_confirm(int slot) const {
    return slot >= 0 && slot < kSlots && slots_[static_cast<std::size_t>(slot)].present;
}

void SaveSlotList::request_overwrite(int slot) {
    if (!needs_confirm(slot)) {
        return;
    }
    if (pending_ < 0) {
        restore_selected_ = selected_;
    }
    pending_ = slot;
    place_selection(slot);
}

void SaveSlotList::confirm() {
    if (pending_ < 0) {
        return;
    }
    pending_ = -1;
    // The selection stays on the confirmed slot; no file is written.
}

void SaveSlotList::cancel() {
    if (pending_ < 0) {
        return;
    }
    pending_ = -1;
    place_selection(restore_selected_);
}

} // namespace gm
