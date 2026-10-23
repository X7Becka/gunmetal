#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

inline constexpr std::string_view kVersion = "0.1.0";

struct Options {
    std::filesystem::path game;
    std::string level;
    std::string play;
    std::string song;
    std::string sample; // --sample: resolve a SOUND_SAMPLE_TABLE name
    std::string scene;
    std::string sim;          // --sim LEVEL: headless fixed-tick run
    int sim_ticks = 1;        // --ticks N
    int sim_players = 1;      // --sim-players N (1..8)
    int sim_mode = 0;         // --sim-mode N (game_mode of the VM context)
    int sim_difficulty = 1;   // --sim-difficulty N
    bool sim_god = false;     // --god: the GOD_MODE branch
    std::string sim_drive;    // --sim-drive PLAN: scripted player segments
    bool sim_projectile = false; // --sim-projectile: spawn rounds, not hitscan
    std::string sim_vitals;   // --sim-vitals ARMOR,SHIELD: starting gauges
    bool menu = false;        // --menu: dump the headless frontend screen
    std::string menu_keys;    // --menu-keys up,down,enter: feed the menu
    std::string menu_screen;  // --menu-screen ID: start on that §2 screen
    bool menu_json = false;   // --menu-json: machine-readable screen dump
    double music_fade_in = 0.0;  // --fade-in SEC: music fades in (M7.5)
    double music_fade_out = 0.0; // --fade-out SEC: music fades out at the end
    std::string pager;        // --pager FILE: headless SML pager (M8.1)
    std::string pager_keys;   // --pager-keys next,next,prev,first,last
    int pager_page = 0;       // --page N: 1-based page to show
    bool pager_all = false;   // --all: every page
    bool save_slots = false;  // --save-slots: headless save-slot list (M8.7)
    int sim_respawn = -1;     // --sim-respawn N: auto-respawn N ticks after death
    std::string film;
    std::string anim;
    std::string hud;          // optional Film/*.sof overlay
    std::filesystem::path hud_screen; // --hud-screen SML/... overlay
    int hud_tick = 0;         // 91 Hz tick for --render
    int hud_x = 0;
    int hud_y = 0;
    int hud_scale = 1;
    float hud_opacity = 1.0f;
    int frame = 0;
    int at = 0;
    std::string music;
    std::string sou1;
    std::string sou2;
    std::filesystem::path wav;
    std::filesystem::path dump;
    std::filesystem::path save_out; // --dump a save: re-serialise it here
    std::vector<std::string> save_edits; // --set KEY=VALUE (ammo0, armor, ...)
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
    bool mip_auto = false;     // --mip auto: screen-space LOD
    bool bilinear = false;     // opt-in bilinear filtering
    bool alpha_test = false;   // key out the transparent palette index
    bool chromakey = false;    // key palette 0 only on textures with flags bit 0
    bool texel_uv = false;     // engine texture projection (docs/TEXUV.md)
    bool skip_untextured = false; // drop texture==0 && handle==0 polygons
    double haze = 0.0;            // white fade 0..1 (SET_CAMERA_HAZE)
    int transparent_index = 0;
    bool spawn_camera = false;  // start the viewer at SET_PLAYER_START
    int spawn_slot = 0;
    float spawn_eye = 10.0f;    // LIGHT chassis (player+0x2F50)
    bool third_person = false;  // place the player chassis, pull the camera back
    float third_distance = 220.0f;
    float third_height = 40.0f;
    std::array<double, 3> third_pos{}; // photo override: feet position
    bool has_third_pos = false;
    float third_yaw = 0.0f;           // photo override: chassis yaw, degrees
    bool has_third_yaw = false;
    std::string player_model = "TICK"; // LIGHT chassis (SOBS/TICK.sob)
    float player_scale = 1.0f;    // chassis mesh scale (engine world units)
    bool player_origin = false;   // place the mesh origin at the feet, not min_z
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
