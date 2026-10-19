#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "res/sbt.h"

namespace gm {

/// A light placed by the level script (``ADD_SKY_LIGHT_SOURCE`` /
/// ``ADD_LIGHT_SOURCE``).  Coordinates are the raw script operands: the frame
/// matches the level mesh (see gm/scene.py).
struct SceneLight {
    int x = 0;
    int y = 0;
    int z = 0;
    int r = 255;
    int g = 255;
    int b = 255;
    std::string kind = "local"; // "sky" | "local"
    int param = 0;
    std::vector<int> extra;
};

struct SceneLocation {
    int id = 0;
    int x = 0;
    int y = 0;
    int z = 0;
    std::array<double, 3> orientation{};
};

struct SceneItem {
    int id = 0;
    int x = 0;
    int y = 0;
    int z = 0;
    bool fixed = false;
    std::array<int, 5> extra{};
};

struct SceneSound {
    int x = 0;
    int y = 0;
    int z = 0;
    int sample = 0;
    int volume = 0;
    double pitch = 0.0;
};

struct SceneSprite {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    int jitter = 0;
    int id = 0;
    std::array<int, 5> extra{};
};

/// ``ADD_DOOR`` operands, kept in the order the script passes them (the engine
/// permutes them into its door array; see gm/scene.py for the proven mapping).
struct SceneDoor {
    int flags = 0;
    std::array<int, 5> ids{};
    std::array<double, 3> angles{};
    std::array<int, 7> params{};
    std::array<double, 9> values{};
    std::array<int, 6> tail{};

    bool moving() const;
};

struct SceneSwitch {
    int kind = 0;
    int id = 0;
    int radius = 0;
    std::array<int, 3> pos{};
    std::array<int, 2> extra{};
    bool absolute() const { return true; }
};

/// A thing built by an ``ADD_THING*`` condition inside an entity sub-script
/// (gm/scene.py's ``Thing``; names/models need THING_TYPES/SOB_TABLE).
struct SceneThing {
    int entity = 0;
    std::string kind;
    int type_index = -1;
    bool absolute = false;
    int x = 0;
    int y = 0;
    int z = 0;
    std::vector<double> angles;
    int owner = 0;
};

/// One row of the SCRIPT entity table: ``(id, flags, sub-script offset)``.
struct SceneEntity {
    int id = 0;
    std::uint32_t flags = 0;
    std::uint32_t offset = 0;
};

/// Everything ``gm/scene.py`` decodes without the ``ADD_THING*`` conditions.
struct Scene {
    std::vector<SceneLight> lights;
    std::vector<SceneLocation> locations;
    std::vector<SceneItem> items;
    std::vector<SceneSound> sounds;
    std::vector<SceneSprite> sprites;
    std::vector<SceneDoor> doors;
    std::vector<SceneSwitch> switches;
    std::vector<SceneEntity> entities;
    std::vector<SceneThing> things;
    std::map<std::string, int> other; // placement commands not decoded yet
};

/// Decode the SCRIPT record of *level* into a :class:`Scene`.  Returns nullopt
/// when the record is missing or the bytecode does not decode.
std::optional<Scene> extract_scene(const SbtLevel& level, std::string& error);

} // namespace gm
