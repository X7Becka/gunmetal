#include "game/save_action.h"

#include <cstdint>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include "core/path.h"            // resolve_ci
#include "game/chassis_loadout.h" // ChassisSpec
#include "game/runtime.h"         // GameRuntime
#include "game/sim_player.h"      // SimPlayer
#include "res/save.h"             // parse_save
#include "res/save_write.h"       // write_save

namespace gm {
namespace {

namespace fs = std::filesystem;

/// The parser refuses anything above 64 MiB (`res/save.cpp`,
/// `kMaxSaveBytes`); stop before reading such a file into memory.
constexpr std::uintmax_t kMaxSaveBytes = 64u * 1024u * 1024u;

/// Best-effort absolute, normalised path: `weakly_canonical` resolves the
/// existing prefix of a path that is about to be created.
fs::path canonicalish(const fs::path& path) {
    std::error_code ec;
    fs::path result = fs::weakly_canonical(path, ec);
    if (ec || result.empty()) {
        result = fs::absolute(path, ec);
        if (ec || result.empty()) {
            return path.lexically_normal();
        }
    }
    return result.lexically_normal();
}

/// True when *child* is *parent* or lies below it (component-wise, so
/// `/games/Gunmetal 2` is not inside `/games/Gunmetal`).
bool same_or_inside(const fs::path& child, const fs::path& parent) {
    auto c = child.begin();
    for (auto p = parent.begin(); p != parent.end(); ++p, ++c) {
        if (c == child.end() || *c != *p) {
            return false;
        }
    }
    return true;
}

bool read_file(const fs::path& path, std::vector<std::uint8_t>& out, std::string& error) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        error = "cannot stat " + path.string() + ": " + ec.message();
        return false;
    }
    if (size > kMaxSaveBytes) {
        error = path.string() + " is larger than the 67108864-byte save limit";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path.string();
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    if (size != 0) {
        in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
        if (!in) {
            error = "cannot read " + path.string();
            return false;
        }
    }
    if (in.peek() != std::char_traits<char>::eof()) {
        error = path.string() + " changed while it was being read";
        return false;
    }
    return true;
}

} // namespace

SaveActionResult SaveAction::apply(const SaveMenu& menu, SaveMenuEvent event,
                                   const std::filesystem::path& game,
                                   const std::filesystem::path& out_dir,
                                   std::string* error) {
    if (event == SaveMenuEvent::none || event == SaveMenuEvent::back) {
        // Esc / no event: nothing to do, counters and `error()` stay as they
        // were (docs/SAVE_ACTION.md §2).
        return SaveActionResult::none;
    }
    const bool saving = event == SaveMenuEvent::save;

    auto refuse = [&](SaveActionResult result, std::string text) {
        ++stats_.refusals;
        error_ = std::move(text);
        if (error != nullptr) {
            *error = error_;
        }
        return result;
    };
    auto succeed = [&](SaveActionResult result) {
        if (result == SaveActionResult::saved) {
            ++stats_.saves;
        } else {
            ++stats_.loads;
        }
        error_.clear();
        if (error != nullptr) {
            error->clear();
        }
        return result;
    };

    if (!menu.is_open()) {
        return refuse(saving ? SaveActionResult::refused_write
                             : SaveActionResult::refused_read,
                      "save action: the save menu is not open");
    }
    if (game.empty()) {
        return refuse(saving ? SaveActionResult::refused_write
                             : SaveActionResult::refused_read,
                      "save action: no game directory given");
    }
    const int slot = menu.selected();
    if (slot < 0 || slot >= SaveMenu::kSlots) {
        return refuse(saving ? SaveActionResult::refused_write
                             : SaveActionResult::refused_read,
                      "save action: slot " + std::to_string(slot) + " out of range [0, " +
                          std::to_string(SaveMenu::kSlots - 1) + "]");
    }

    const std::string name = "saved" + std::to_string(slot) + ".sbt";
    const auto source = resolve_ci(game, "SAVEGAME/" + name);
    if (!source) {
        // No template: an empty slot stays empty.  The original's writer
        // expects a full save container, and inventing one would be a guess
        // (docs/SAVE_ACTION.md §1).
        return refuse(SaveActionResult::refused_empty,
                      "save slot " + std::to_string(slot) + " is empty: " +
                          (game / "SAVEGAME" / name).string() +
                          " does not exist (no empty template is invented)");
    }

    std::vector<std::uint8_t> bytes;
    std::string read_error;
    if (!read_file(*source, bytes, read_error)) {
        return refuse(SaveActionResult::refused_read,
                      "save slot " + std::to_string(slot) + " cannot be read: " + read_error);
    }
    std::string parse_error;
    const auto save = parse_save(bytes, parse_error);
    if (!save) {
        return refuse(SaveActionResult::refused_read,
                      "save slot " + std::to_string(slot) + " (" + source->string() +
                          ") cannot be parsed: " + parse_error);
    }

    if (saving) {
        if (out_dir.empty()) {
            return refuse(SaveActionResult::refused_write,
                          "save action: no output directory given");
        }
        // The installation is read-only: refuse before creating anything.
        if (same_or_inside(canonicalish(out_dir), canonicalish(game))) {
            return refuse(SaveActionResult::refused_write,
                          "refusing to write into the game installation: " +
                              out_dir.string() + " is inside " + game.string());
        }

        // Rebuild the container through the strict writer.  Passing the
        // original bytes keeps the opaque header (0x10..0x50) and makes the
        // shipped saves come out byte for byte (docs/SAVE_WRITE_CPP.md §3).
        std::vector<std::uint8_t> rebuilt;
        std::string write_error;
        SaveWriteCode code = SaveWriteCode::ok;
        if (!write_save(*save, rebuilt, &write_error, &code, bytes)) {
            return refuse(SaveActionResult::refused_write,
                          "save slot " + std::to_string(slot) + " cannot be rebuilt (" +
                              save_write_code_name(code) + "): " + write_error);
        }

        std::error_code ec;
        fs::create_directories(out_dir, ec);
        if (ec) {
            return refuse(SaveActionResult::refused_write,
                          "cannot create " + out_dir.string() + ": " + ec.message());
        }
        if (!fs::is_directory(out_dir, ec) || ec) {
            return refuse(SaveActionResult::refused_write,
                          out_dir.string() + " is not a directory");
        }

        const fs::path target = out_dir / name;
        const fs::path part = out_dir / (name + ".part");
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            if (!out) {
                return refuse(SaveActionResult::refused_write,
                              "cannot create " + part.string());
            }
            out.write(reinterpret_cast<const char*>(rebuilt.data()),
                      static_cast<std::streamsize>(rebuilt.size()));
            if (!out) {
                out.close();
                fs::remove(part, ec);
                return refuse(SaveActionResult::refused_write,
                              "cannot write " + part.string());
            }
        }
        fs::rename(part, target, ec);
        if (ec) {
            fs::remove(part, ec);
            return refuse(SaveActionResult::refused_write,
                          "cannot move " + part.string() + " to " + target.string() + ": " +
                              ec.message());
        }
        return succeed(SaveActionResult::saved);
    }

    // --- load ---------------------------------------------------------------
    // The save carries no chassis, so the scratch player is initialised with
    // the shipped LIGHT capsule values; `apply_save()` re-initialises it at the
    // saved feet/yaw and the values in `LoadedSave` do not depend on it
    // (docs/SAVE_LOAD.md §1).
    ChassisSpec scratch;
    scratch.name = "save-action scratch";
    scratch.eye_height = 10.0;
    scratch.max_speed = 1.7;
    std::string init_error;
    SimPlayer player;
    if (!player.init(Vec3d{}, 0.0, scratch, &init_error)) {
        return refuse(SaveActionResult::refused_read,
                      "cannot load save slot " + std::to_string(slot) + ": " + init_error);
    }

    GameRuntime runtime;
    LoadedSave loaded;
    std::string apply_error;
    if (!apply_save(*save, runtime, player, loaded, &apply_error)) {
        return refuse(SaveActionResult::refused_read,
                      "cannot load save slot " + std::to_string(slot) + ": " + apply_error);
    }
    last_load_ = loaded;
    return succeed(SaveActionResult::loaded);
}

} // namespace gm
