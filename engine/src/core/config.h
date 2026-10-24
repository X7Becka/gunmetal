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
    std::string sample_play; // --sample-play: decode/play through SampleCache (M7.4)
    bool twice = false;      // --twice: acquire the same sample twice (cache hit)
    // M7.3 slice 2: positional `--sample-play` (docs/AUDIO_3D.md §13).
    // The source position reuses `--pos`/`has_pos` (the camera eye otherwise);
    // `--sample-play` with `--pos` or `--listener` switches to the 3D path.
    std::array<double, 3> listener{}; // --listener X,Y,Z[,YAW]
    double listener_yaw = 0.0;        // yaw in radians, 0 = facing +Z
    bool has_listener = false;
    int sample_volume = 32;      // --volume: 5-bit 1..32 (doubled for gain_of)
    double sample_atten = 0.03;  // --atten: linear falloff per world unit
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
    std::string briefing;     // --briefing LEVEL: mission briefing (M8.3)
    bool level_select = false;    // --level-select: screen 17 frame (M8.4)
    int level_select_counter = 1; // --level-select-counter N: campaign counter
    int level_select_row = 0;     // --level-select-row N: cursor row index
    bool level_select_no_plate = false; // --no-plate: black canvas, no PLATE
    bool save_menu = false;        // --save-menu: screen 14 frame (M8.7)
    int save_menu_slot = 0;        // --save-menu-slot N: cursor slot index
    std::string save_menu_mode = "load"; // --save-menu-mode load|save
    std::string save_action;       // --save-action save|load (M8.7 slice 2)
    std::string save_action_out;   // --save-action-out DIR: where `save` writes
    bool start_game = false;       // --start-game: screen 13 frame (M8.7)
    std::string keycfg;           // --keycfg NAME: screen 30 frame of a .key
    int keycfg_cursor = 0;        // --keycfg-cursor N: highlighted action 0..30
    std::string keycfg_mode = "view";   // --keycfg-mode view|edit (screens 34/35)
    std::string keycfg_cell = "a";      // --keycfg-cell a|b: edited column
    std::string keycfg_conflict;        // --keycfg-conflict TEXT: line at y 109
    bool game_type = false;             // --game-type: screen 19 frame (M8.8)
    int game_type_cursor = 0;           // --game-type-cursor N: item id 0..11
    unsigned game_type_mask = 0xFFu;    // --game-type-mask HEX: 0x4c1290 byte
    bool difficulty = false;            // --difficulty: screen 23 frame (M8.8)
    int difficulty_cursor = 2;          // --difficulty-cursor N: 0..5 (2 default)
    bool local_players = false;         // --local-players: screen 21 frame (M8.8)
    int local_players_cursor = 0;       // --local-players-cursor N: 0..2
    bool quit = false;                  // --quit: screen 33 frame (M8.8)
    bool quit_in_game = false;          // --quit-in-game: QUIT OPTIONS variant
    int quit_cursor = 0;                // --quit-cursor N: item id
    bool video_mode = false;            // --video-mode: screen 37 frame (M8.8)
    int video_mode_cursor = 0;          // --video-mode-cursor N: 0..1
    bool restart = false;               // --restart: screen 6 frame (M8.8)
    int restart_items = 2;              // --restart-items N: 2..4 (clamped)
    int restart_cursor = 0;             // --restart-cursor N: item id
    bool things_anim = false;           // --things-anim: clip/frame per thing
    int things_anim_tick = 0;           // --things-anim-tick N: 91 Hz phase
    int things_anim_limit = 20;         // --things-anim-limit N: printed lines
    bool video_mode_list = false;       // --video-mode-list: screen 18 (M8.8)
    int video_mode_list_cursor = 0;     // --video-mode-list-cursor N: 0..14
    bool mouse_setup = false;           // --mouse-setup: screen 22 (M8.8)
    int mouse_setup_cursor = 0;         // --mouse-setup-cursor N: 0..11
    bool joystick_centering = false;    // --joystick-centering: screen 26 (M8.8)
    int joystick_centering_cursor = -1; // --joystick-centering-cursor N (-1 = none)
    bool match_setup = false;           // --match-setup: screen 27 (M8.8)
    int match_setup_cursor = 7;         // --match-setup-cursor N: item 0..7
    int match_setup_type = 0;           // --match-setup-type N: 0..2
    int match_setup_cash = 4;           // --match-setup-cash N: 0..11
    int match_setup_flags = 0;          // --match-setup-flags HEX: checkbox bits
    bool key_jamming = false;           // --key-jamming: screen 16 (M8.8)
    std::vector<int> key_jamming_keys;  // --key-jamming-keys 30,31,... (comma list)
    int key_jamming_cursor = 4;         // --key-jamming-cursor N (4 = shipped)
    bool ingame_menu = false;           // --ingame-menu: screen 9 (M8.8)
    std::string ingame_menu_bg = "DEMOTION"; // --ingame-menu-bg DEMOTION|DISBAND
    std::string hud_lut_dump;           // --hud-lut-dump OUT: write the 4096-byte tables
    bool lay_bucks = false;             // --lay-bucks: screen 20 (M8.8)
    bool save_name = false;             // --save-name: screen 15 (M8.8)
    std::string save_name_text = "A SAVED GAME"; // --save-name-text NAME
    int save_name_row = 0;              // --save-name-row N: 0..5 (clamped)
    bool save_name_no_caret = false;    // --save-name-no-caret
    bool save_name_no_fileslot = false; // --no-fileslot
    bool error_screen = false;          // --error: screen 5 (M8.8)
    std::string error_message = "No Error."; // --error-message TEXT
    bool player_setup = false;          // --player-setup: screen 36 (M8.8)
    int player_setup_cursor = 0;        // --player-setup-cursor N
    int player_setup_joystick = -1;     // --player-setup-joystick N (-1 = none)
    int player_setup_player = 0;        // --player-setup-player N: 0/1
    bool control_options = false;       // --control-options: screen 10 (M8.8)
    int control_options_cursor = 0;     // --control-options-cursor N: 0..9
    int control_options_keyset1 = 0;    // --control-options-keyset1 N: 0..15
    int control_options_keyset2 = 1;    // --control-options-keyset2 N: 0..15
    int control_options_joystick1 = -1; // --control-options-joystick1 N
    int control_options_joystick2 = -1; // --control-options-joystick2 N
    int control_options_mouse = 0;      // --control-options-mouse N: 0..2
    bool sound_options = false;         // --sound-options: screen 11 (M8.8)
    int sound_options_cursor = 0;       // --sound-options-cursor N
    int sound_options_music = 10;       // --sound-options-music N: 0..20
    int sound_options_effects = 20;     // --sound-options-effects N: 0..20
    bool sound_options_swap = false;    // --sound-options-swap
    bool sound_options_off = false;     // --sound-options-off
    bool sound_options_low_memory = false; // --sound-options-low-memory
    bool graphics_options = false;      // --graphics-options: screen 12 (M8.8)
    int graphics_options_cursor = 0;    // --graphics-options-cursor N: 0..5
    int graphics_options_border = 3;    // --graphics-options-border N: 0..9
    int graphics_options_gamma = 0;     // --graphics-options-gamma N: 0..3
    int graphics_options_reflections = 1; // --graphics-options-reflections N
    int graphics_options_light_maps = 1;  // --graphics-options-light-maps N: 0..2
    bool play_options = false;          // --play-options: screen 32 (M8.8)
    int play_options_cursor = 0;        // --play-options-cursor N: 0..7
    int play_options_flags = 0;         // --play-options-flags HEX
    int play_options_camera = 1;        // --play-options-camera N: 0..2 (.data = 1)
    bool play_options_in_game = false;  // --play-options-in-game




    bool briefing_text = false; // --briefing-text: print the page text
    std::string pager_keys;   // --pager-keys next,next,prev,first,last
    int pager_page = 0;       // --page N: 1-based page to show
    bool pager_all = false;   // --all: every page
    bool pager_background = false; // --background: HELPSCRN behind the page (M8.1)
    int pager_fade = -1;           // --fade N: fade level 0..15 (-1 = off, M8.1)
    int pager_fade_lut = -1;       // --fade-lut N: exact 0x682f90 tables (-1 = off)
    int menu_fade = -1;            // --fade N for --menu --png (same flag)
    bool save_slots = false;  // --save-slots: headless save-slot list (M8.7)
    // M8.7/M9 slice 2: load a save into a live runtime (docs/SAVE_LOAD.md).
    // `save_load` is a slot number 0..19 (`SAVEGAME/saved<N>.sbt`) or a path.
    std::string save_load;
    int save_load_ticks = 0;  // --save-load-ticks N: fixed ticks after the load
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
    // --png-scale N: integer nearest-neighbour zoom of a 320x200 front-end
    // frame (render/upscale.h, docs/UPSCALE.md).  1 = no change.
    int png_scale = 1;
    // --hud-png OUT: composed 320x200 cockpit frame (`render/cockpit.h`).
    std::filesystem::path hud_png;
    // --killed NAME: the KILLED screen lines with %S = NAME (ui/killed.h).
    std::string killed_name;
    bool killed_all = false;          // --killed-all: every accepted line
    unsigned killed_seed = 0;         // --killed-seed N: which line pick() returns
    bool best_times = false;          // --best-times: GUNMETAL.HI rows (ui/records.h)
    std::string playlist;             // --playlist FILE: .sog resolved against MUSIC (M7.5)
    int hud_weapon = -1;   // --hud-weapon N (weapon slot 0..18)
    int hud_ammo = 0;      // --hud-ammo N
    int hud_armor = 10000; // --hud-armor N (hundredths)
    int hud_kills = 0;     // --hud-kills N
    double hud_speed = 0.0;// --hud-speed X (throttle 0..1)
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
