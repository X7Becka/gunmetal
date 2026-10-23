#pragma once

// Headless save-slot list: the 20-entry name table of `gundx.exe` without a
// window, rendering or writing.
//
// This is the model of the list that screens 13/14/15 (START GAME / SAVE GAME /
// NAME ENTRY) draw.  The numbers and the strings come from docs/UI_SCREENS.md
// §5.1, which is the source of truth:
//
//   * the loader `0x48f970` fills a table of 20 entries (`0x6c2fe0`, step
//     0x13, end `0x6c315c`);
//   * `read_name` (`0x455d90`) opens `savegame\saved%d.sbt` (`0x4ab460`) and
//     copies the name from the save header (`name[0x50]`, docs/SAVE.md §1.1);
//   * a slot whose name cannot be read is labelled `::: empty slot :::`
//     (`0x4bc3b0`);
//   * the screens show 6 rows, the scroll `0x68ccdc` is bounded by 0..13
//     (`scroll > 0` and `scroll + 6 < 20` in `0x48aa60`/`0x48abd0`).
//
// What this class owns: the parsed header of every slot, the scroll, the
// selection and a pending overwrite confirmation.  What it does not own:
// pixels, fonts, the highlight rectangle `0x485d00`/`0x485cc0`, save/load
// writers (`0x4422d0`/`0x442340`), the paired world `saved<N>.sbw` and the
// name-entry buffer `0x661280`.  See docs/SAVE_SLOTS.md.
//
// Invariants (checked by `engine/tests/test_save_slots.cpp`)
// ----------------------------------------------------------
//   * `count()` is always 20; before `refresh()` (and after a refresh of a
//     missing directory) every slot has `present == false`, an empty `name`
//     and the label `::: empty slot :::`.
//   * `scroll() ∈ [0, 13]`, `selected() ∈ [scroll, scroll + 5]`.  The original
//     bound `scroll + 6 < 20` makes the window `[scroll, scroll + 5]`, so slot
//     19 can never be selected - see docs/SAVE_SLOTS.md §4.
//   * While `overwrite_pending()` is true, `scroll_*()`/`select_*()` are
//     refused (modal confirmation) and `cancel()` restores the selection that
//     `request_overwrite()` found.
//   * Every method is deterministic: same files and same call sequence give
//     the same state, and no call throws.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

/// One row of the save list.  `index` is the slot number (0..19), which is also
/// the `<N>` of `savegame\saved<N>.sbt`.
struct SaveSlot {
    int index = 0;
    bool present = false;   ///< `saved<N>.sbt` exists and parses
    std::string name;       ///< header title (`name[0x50]`, docs/SAVE.md §1.1)
    int level = -1;         ///< numeric suffix of `level[0x50]` ("LEVEL0" -> 0), -1 if not `LEVEL<n>`
    int mission = 0;        ///< header F2 (+0xac): next mission/level number (proven)
    std::string detail;     ///< `<level_name>, mission <N>`; empty for an empty slot
};

/// The 20-entry save list of the front-end, as a pure state model.
class SaveSlotList {
public:
    static constexpr int kSlots = 20;   ///< `(0x6c315c - 0x6c2fe0) / 0x13`
    static constexpr int kVisible = 6;  ///< rows drawn by screens 13/14
    /// Largest scroll of the original condition `scroll + 6 < 20`: 13.  The
    /// window is `[scroll, scroll + 5]`, and the condition requires one more
    /// slot after it, so `kSlots - kVisible` (14) is one too far and slot 19 is
    /// never visible (docs/SAVE_SLOTS.md §4).
    static constexpr int kMaxScroll = kSlots - kVisible - 1; ///< 13
    /// `0x4bc3b0`: label of a slot whose name cannot be read.
    static constexpr std::string_view kEmptyLabel = "::: empty slot :::";

    /// A list of 20 empty slots, scroll 0, selection 0, no pending overwrite.
    SaveSlotList();

    /// Read `savegame\saved<N>.sbt` for N = 0..19 (via `gm::parse_save`),
    /// fill the names/levels/missions and reset scroll/selection/pending.
    ///
    /// A missing `SAVEGAME`/`savegame` directory or a missing file is an empty
    /// slot, not an error.  A file that cannot be read or parsed leaves the
    /// slot empty; `*error` (when non-null) is cleared first and receives the
    /// first such problem, so the other slots are still loaded.  `error` may be
    /// null.
    void refresh(const std::filesystem::path& game, std::string* error);

    /// Back to 20 empty slots and the initial cursor (no I/O).
    void clear();

    /// Always `kSlots` (the table is global in the original, not a list of the
    /// files that happen to exist).
    int count() const { return kSlots; }

    /// Number of slots with `present == true` after the last `refresh()`.
    int present_count() const;

    /// Slot `index`, or the nearest valid slot for an out-of-range index.
    const SaveSlot& at(int index) const;

    // --- scroll (0..13) ---------------------------------------------------

    int scroll() const { return scroll_; }
    /// True when the window moved; false at 13 (`scroll + 6 < 20` is false) and
    /// while an overwrite confirmation is pending.
    bool scroll_down();
    /// True when the window moved; false at 0 and while a confirmation is pending.
    bool scroll_up();

    // --- selection (absolute slot index, [scroll, scroll + 5]) -------------

    int selected() const { return selected_; }
    /// Move the highlight one row down, scrolling the window when it would
    /// leave the bottom; false at the last reachable slot (18) and while a
    /// confirmation is pending.
    bool select_down();
    /// Move the highlight one row up, scrolling the window when it would leave
    /// the top; false at 0 and while a confirmation is pending.
    bool select_up();
    /// Select `slot` and scroll the window to show it.  False for an
    /// out-of-range slot, for slot 19 (unreachable, see the header) and while a
    /// confirmation is pending.
    bool select(int slot);

    // --- visible window ---------------------------------------------------

    /// Rows drawn by the screen: always 6.
    int visible_rows() const { return kVisible; }

    /// Label of visible row `row` (0..5): the slot name, or `::: empty slot :::`
    /// for an empty slot.  An out-of-range row is the empty string.
    std::string_view item_label(int row) const;

    // --- overwrite confirmation (a model, see docs/SAVE_SLOTS.md §5) ------

    /// True when `slot` is in range and occupied.  The original has no
    /// confirmation dialog at all (docs/UI_SCREENS.md §5.2); this is our model.
    bool needs_confirm(int slot) const;

    /// Ask to overwrite an occupied slot: moves the selection to it and enters
    /// the pending state.  An empty or out-of-range slot is refused; a second
    /// request while pending keeps the first restore point but moves the
    /// selection to the new slot.
    void request_overwrite(int slot);

    bool overwrite_pending() const { return pending_ >= 0; }
    /// Slot awaiting confirmation, -1 when none.  Named `pending_slot()` to keep
    /// the recorded selection available through `selected()`.
    int pending_slot() const { return pending_; }

    /// Accept the overwrite: leave the pending state with the selection on the
    /// confirmed slot.  No file is written - this class never saves.
    void confirm();

    /// Drop the pending overwrite and put the selection back where
    /// `request_overwrite()` found it.
    void cancel();

private:
    void place_selection(int slot);

    std::vector<SaveSlot> slots_;
    int scroll_ = 0;
    int selected_ = 0;
    int pending_ = -1;
    int restore_selected_ = 0;
};

} // namespace gm
