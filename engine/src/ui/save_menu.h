#pragma once

// Headless model of the SAVE GAME (14) / LOAD GAME (13) screen - the screen
// around the 20-entry slot table that `ui/save_slots.*` already parses.
//
// `save_slots` owns "what is in the table"; this class owns the screen state on
// top of it: save vs load mode, the highlighted slot, the 6-row window, the
// per-row data the screen shows (name, level, elapsed seconds) and the action
// Enter produces (load / save / back).  It still has no pixels, no fonts, no
// writer and no loader.
//
// Proven behaviour (docs/UI_SCREENS.md - the source of truth):
//
//   * 20 slots, table `0x6c2fe0`, step 0x13, end `0x6c315c` (§5.1);
//   * `read_name` (`0x455d90`) opens `savegame\saved%d.sbt` (`0x4ab460`) and
//     copies the title from the save header (§5.1, docs/SAVE.md §1.1);
//   * 6 rows are drawn; the scroll `0x68ccdc` is bounded by 0..13 because the
//     original tests `scroll + 6 < 20` (§5.1), so slot 19 is unreachable - the
//     same hole `ui/save_slots.*` reproduces (`docs/SAVE_SLOTS.md` §4);
//   * both screens carry `ESC TO CANCEL` (§3.6), so Esc leaves the screen;
//   * **there is no overwrite confirmation**: screen 14 goes straight to the
//     name entry (15) and the only "used twice" string belongs to key sets
//     (§5.2).  This is a conclusion from the absence of a branch/string, not a
//     positive byte; `activate()` therefore returns `save` for an occupied slot
//     and leaves any confirmation to the caller.  `SaveSlotView::occupied`
//     tells it whether the target will be overwritten.
//
// Model (not in the original screen):
//
//   * `SaveSlotView::level` and `SaveSlotView::seconds` are read from the save
//     header (`level[0x50]` at +0x54 and the level clock F1 at +0xa8) and shown
//     per row.  The original rows of screens 13/14 carry only the name
//     (`0x6c2fe0 + slot*0x13`, §5.1); printing level/time is our decision.
//     `seconds = F1 / 91`: F20 (+0xf4) is the proven "playtime in seconds =
//     F1 / 91" (`0x45c057`/`0x46053a`, docs/SAVE_FIELDS.md §2); the divisor is
//     proven, applying it to every row is the model.
//   * In load mode an empty (or unreadable) slot is not selectable, so
//     `move()` skips it and the initial selection is the first occupied slot.
//     In save mode every reachable slot is selectable - that is how a new save
//     lands in an empty slot and an old one is overwritten.
//   * Slot 19 is never selectable in either mode: it cannot be brought into
//     the `[first, first + 5]` window with `first <= 13`.
//
// Errors are values, never exceptions: a missing `SAVEGAME`/`savegame`
// directory leaves 20 empty views and is reported by `activate()` with a text;
// a file that cannot be read or parsed keeps its slot unoccupied, records the
// first problem in `error()` (like `SaveSlotList::refresh`) and is refused with
// a text on load.  Nothing here writes to the installation.
//
// Invariants (checked by `engine/tests/test_save_menu.cpp`)
// ---------------------------------------------------------
//   * `slots().size() == kSlots` (20) before and after `open()`.
//   * `selected() ∈ [0, kLastReachable]` once open, and always selectable;
//     `visible_first() ∈ [0, kMaxFirst]` and `selected()` is inside
//     `[visible_first(), visible_first() + kVisible - 1]`.
//   * `event()` is `none` after `open()`; `activate()`/`escape()` set it and
//     `clear_event()` resets it.  `activate()` returns true exactly when
//     `event() != none` afterwards.
//   * Same directory + same call sequence -> identical state (deterministic).

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "ui/save_slots.h"

namespace gm {

/// Which screen the model represents.  Both share the same 20-entry table; only
/// the selectability of an empty slot and the action of Enter differ.
enum class SaveMenuMode { save, load };

/// The action `activate()` (Enter) produced.
enum class SaveMenuEvent { none, load, save, back };

/// One slot as the screen sees it.  `index` is the `<N>` of
/// `savegame\saved<N>.sbt`; `occupied` means the file exists and parses.
struct SaveSlotView {
    int index = 0;
    bool occupied = false;
    std::string name;   ///< header title, bytes 0..11 of `name[0x50]` (docs/SAVE.md §1.1)
    std::string level;  ///< header `level[0x50]` verbatim ("LEVEL0"), empty for an empty slot
    int seconds = 0;    ///< model: F1 (+0xa8) / 91, the proven clock-to-seconds divisor
    bool selectable = false; ///< reachable (never 19) and, in load mode, occupied
};

class SaveMenu {
public:
    static constexpr int kSlots = SaveSlotList::kSlots;        ///< 20
    static constexpr int kVisible = SaveSlotList::kVisible;    ///< 6 rows
    /// Largest window start of `scroll + 6 < 20` (`SaveSlotList::kMaxScroll`).
    static constexpr int kMaxFirst = SaveSlotList::kMaxScroll; ///< 13
    /// Last reachable slot: `kMaxFirst + kVisible - 1` = 18; slot 19 is not.
    static constexpr int kLastReachable = kMaxFirst + kVisible - 1;
    /// Proven "seconds = F1 / 91" divisor (`0x45c057`, docs/SAVE_FIELDS.md §2).
    static constexpr int kTicksPerSecond = 91;
    /// `0x4bc3b0`: the label of an empty slot (same as `SaveSlotList`).
    static constexpr std::string_view kEmptyLabel = SaveSlotList::kEmptyLabel;

    /// 20 empty views (`occupied == false`, `selectable == false`), no open
    /// screen; `selected() == 0`, `event() == none`.
    SaveMenu();

    // --- lifecycle ---------------------------------------------------------

    /// Read the 20 slots of *game* for *mode* and reset the cursor/window/event.
    /// Never throws and never writes; see the header for the error contract.
    void open(SaveMenuMode mode, const std::filesystem::path& game);

    bool is_open() const { return open_; }
    SaveMenuMode mode() const { return mode_; }

    /// First problem met while reading the slots (unreadable/corrupt file), or
    /// empty.  A missing save directory is not a listing error - it shows up in
    /// `activate()` because there is nothing to load and nowhere to save.
    const std::string& error() const { return error_; }

    /// True when `<game>/SAVEGAME` or `<game>/savegame` exists.
    bool has_save_directory() const { return !directory_.empty(); }

    // --- slots -------------------------------------------------------------

    /// Always `kSlots` views, in slot order (0..19).
    const std::vector<SaveSlotView>& slots() const { return slots_; }

    /// Slot `index`, clamped to 0..19.
    const SaveSlotView& at(int index) const;

    /// Label of visible row `row` (0..5): the save name or `::: empty slot :::`
    /// for an unoccupied slot; an out-of-range row is the empty string.
    std::string_view label(int row) const;

    /// Rows the window holds: always `kVisible` while open (20 - 13 >= 6).
    int visible_rows() const { return kVisible; }

    // --- cursor and window -------------------------------------------------

    /// Absolute slot index of the highlight (0 when nothing is selectable).
    int selected() const { return selected_; }

    /// Move `delta` selectable slots (down for > 0, up for < 0); `delta == 0`
    /// and the window edges keep the selection.  Unselectable slots are skipped:
    /// slot 19 always, empty slots in load mode.  A request past the first/last
    /// selectable slot clamps there instead of wrapping.
    void move(int delta);

    /// First slot of the 6-row window (`0..13`).
    int visible_first() const { return first_; }

    // --- action ------------------------------------------------------------

    /// Enter on the highlighted slot.  Returns true when an event was produced
    /// (`load`/`save`); on refusal it returns false, sets `event()` to `none`
    /// and writes a one-line reason into `*error` (may be null).
    ///
    /// * load: an occupied slot yields `load`; an empty or unreadable slot is
    ///   refused with a text.
    /// * save: a reachable slot yields `save`, occupied or not - the original
    ///   has no overwrite confirmation (see the header).  A missing save
    ///   directory or the unreachable slot 19 is refused with a text.
    ///
    /// No byte of the save/world is written: this is only the event.
    bool activate(std::string* error = nullptr);

    /// Esc: `ESC TO CANCEL` of both screens (§3.6).  Sets `SaveMenuEvent::back`
    /// and leaves the state otherwise untouched (the caller closes the screen).
    void escape();

    /// Last event; `none` after `open()` and after a refused `activate()`.
    SaveMenuEvent event() const { return event_; }
    void clear_event() { event_ = SaveMenuEvent::none; }

private:
    void reveal();
    void place_initial_selection();

    std::vector<SaveSlotView> slots_;
    std::vector<std::string> problems_; ///< per slot, empty when readable
    std::filesystem::path directory_;   ///< resolved SAVEGAME/savegame, empty if none
    std::filesystem::path game_;
    std::string error_;
    SaveMenuMode mode_ = SaveMenuMode::load;
    SaveMenuEvent event_ = SaveMenuEvent::none;
    int selected_ = 0;
    int first_ = 0;
    bool open_ = false;
};

} // namespace gm
