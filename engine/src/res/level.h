#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "res/sbc.h"
#include "res/sbt.h"
#include "res/sbw.h"

namespace gm {

/// The three per-level files the engine loads together: world (``.sbw``),
/// compiled entities (``.sbt``) and lightmaps (``.sbc``).  Any of them may be
/// absent on disk; only the world is mandatory.
struct LevelBundle {
    std::string name;
    std::optional<SbwLevel> world;
    std::optional<SbtLevel> entities;
    std::optional<Sbc> lightmaps;
};

/// Resolve ``<name>.sbw/.sbt/.sbc`` under *dir* case-insensitively and parse
/// what is present.
std::optional<LevelBundle> load_level(const std::filesystem::path& dir,
                                      std::string_view name, std::string& error);

} // namespace gm
