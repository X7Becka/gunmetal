#pragma once

// Save/load action on top of the screen model (M8.7/M9.3, slice 2;
// docs/SAVE_ACTION.md).
//
// `ui/save_menu.*` produces the event of Enter (`SaveMenuEvent::save`/`load`),
// `res/save_write.*` writes a save container and `game/save_load.*` reads one
// into a runtime.  This module is the seam that ties the three together:
//
//   * `save` on the highlighted slot rebuilds that slot's container with
//     `write_save()` and writes it to `<out_dir>/saved<N>.sbt`.  The bytes are
//     a byte-for-byte copy of the source when the source is a shipped/valid
//     save (the writer round-trips it; docs/SAVE_WRITE_CPP.md §5).  The source
//     is always `<game>/SAVEGAME/saved<N>.sbt`; the game directory itself is
//     never written (checked below).
//   * `load` on an occupied slot parses it and applies the proven subset to a
//     scratch `GameRuntime` + `SimPlayer` pair through `apply_save()`
//     (docs/SAVE_LOAD.md §2).  The caller reads the applied level/pose/gauges
//     from `last_load()`.
//   * `back`/`none` change nothing.
//
// Missing source slots are refused as `refused_empty` instead of being
// invented: there is no proven "empty save" template, so a save into a slot
// that has no `saved<N>.sbt` (and no template) does not create a file
// (docs/SAVE_ACTION.md §1).  A file that exists but cannot be read or parsed
// is `refused_read` for both actions.
//
// Writing into the game installation is refused by path (`refused_write`):
// `out_dir` is canonicalised and must not be the game directory or lie inside
// it.  `out_dir` itself is created when missing (model, §1) - the original's
// own writer is documented to create `savegame` (docs/SAVE_WRITE_CPP.md §1).
//
// Errors are values, never exceptions; every refusal bumps `stats().refusals`
// and leaves a one-line text in `error()` (and in `*error` when not null).
//
// Not connected to the front-end or `main.cpp`: like `ui/save_menu.*` this is
// a library with a test (docs/SAVE_ACTION.md §4).

#include <filesystem>
#include <string>

#include "game/save_load.h"
#include "ui/save_menu.h"

namespace gm {

/// What `SaveAction::apply()` did.
enum class SaveActionResult {
    none,          ///< `back`/`none`: no action, no counter changed
    saved,         ///< the container was written to `<out_dir>/saved<N>.sbt`
    loaded,        ///< the slot was parsed and applied; see `last_load()`
    refused_empty, ///< the slot has no source file (no template is invented)
    refused_write, ///< output refused: game directory, unwritable target, writer refusal
    refused_read,  ///< the source exists but cannot be read or parsed/applied
};

/// Counters, all monotonic for the lifetime of one `SaveAction`.
struct SaveActionStats {
    int saves = 0;    ///< `saved`
    int loads = 0;    ///< `loaded`
    int refusals = 0; ///< `refused_empty` + `refused_write` + `refused_read`
};

/// One save/load action per `SaveMenu` event.
class SaveAction {
public:
    /// Mutable counters (the test and future CLI both read them); the const
    /// overload is the read-only view.
    SaveActionStats& stats() { return stats_; }
    const SaveActionStats& stats() const { return stats_; }

    /// Text of the last refusal, empty after a successful apply.  Same text as
    /// the `error` out-parameter of that call.
    std::string error() const { return error_; }

    /// What the last `loaded` action applied: level name (lower-cased, as
    /// `save_level_name()` returns it), feet, yaw, gauges, loadout and the
    /// `unsupported` list of `game/save_load.*`.  Default when nothing has
    /// been loaded yet.
    const LoadedSave& last_load() const { return last_load_; }

    /// Apply *event* of *menu* (whose `selected()` is the target slot).
    ///
    /// * `none`/`back`            -> `SaveActionResult::none`, counters and
    ///                               `error()` are left untouched.
    /// * `save`, occupied source  -> write `<out_dir>/saved<N>.sbt`; `saved`.
    /// * `save`, missing source   -> `refused_empty`, no file is created.
    /// * `load`, occupied source  -> parse + `apply_save()` into a scratch
    ///                               runtime; `loaded`, `last_load()` updated.
    /// * `load`, missing source   -> `refused_empty`.
    /// * unreadable/corrupt file  -> `refused_read` with the parser text.
    /// * unwritable/game directory/unrepresentable model -> `refused_write`.
    ///
    /// A closed menu is refused too (`refused_write` for `save`, `refused_read`
    /// for `load`) because `selected()` then names no real slot.  Never throws
    /// and never writes into *game*.
    SaveActionResult apply(const SaveMenu& menu, SaveMenuEvent event,
                           const std::filesystem::path& game,
                           const std::filesystem::path& out_dir,
                           std::string* error = nullptr);

private:
    SaveActionStats stats_{};
    std::string error_;
    LoadedSave last_load_{};
};

} // namespace gm
