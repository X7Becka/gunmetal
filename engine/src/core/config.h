#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace gm {

inline constexpr std::string_view kVersion = "0.1.0";

struct Options {
    std::filesystem::path game;
    std::string level;
    std::string play;
    std::string song;
    std::string scene;
    std::string music;
    std::string sou1;
    std::string sou2;
    std::filesystem::path wav;
    std::filesystem::path dump;
    std::filesystem::path png;
    std::filesystem::path render; // output PNG for --render
    std::filesystem::path model;  // .sob to render instead of a level
    int width = 640;
    int height = 400;
    float az = 35.0f;
    float el = 22.0f;
    float zoom = 1.0f;
    float fov = 45.0f;
    std::array<double, 3> pos{};
    std::array<double, 3> look{};
    bool has_pos = false;
    bool has_look = false;
    float clip = 0.0f;
    bool no_textures = false;
    bool no_lightmaps = false;
    bool lightmap_world = false;
    bool wire = false;
    int mip = 0;               // fixed mip level (0 = base)
    bool bilinear = false;     // opt-in bilinear filtering
    bool alpha_test = false;   // key out the transparent palette index
    int transparent_index = 0;
    bool things = false;       // place the scene's ADD_THING models
    float thing_scale = 1.0f;  // extra scale for those models
    bool info = false;
    bool headless = false;
    int frames = -1; // < 0: run until the window is closed
    bool verbose = false;
    bool show_help = false;
    bool show_version = false;
};

/// Parse the command line. On error returns `std::nullopt` and fills `error`.
std::optional<Options> parse_args(int argc, char** argv, std::string& error);

/// `GUNMETAL_DIR`, the usual install path, or the current directory.
std::filesystem::path default_game_dir();

std::string usage();

} // namespace gm
