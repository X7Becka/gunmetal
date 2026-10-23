#include "core/config.h"

#include <cstdlib>
#include <sstream>
#include <system_error>

namespace gm {
namespace {

bool parse_int(std::string_view text, int& out) {
    std::istringstream is{std::string(text)};
    int value = 0;
    is >> value;
    if (!is || !is.eof()) {
        return false;
    }
    out = value;
    return true;
}

} // namespace

std::filesystem::path default_game_dir() {
    if (const char* env = std::getenv("GUNMETAL_DIR"); env != nullptr && *env != '\0') {
        return std::filesystem::path(env);
    }
    const std::filesystem::path windows_install = "/mnt/d/games/Gunmetal 3dfx";
    std::error_code ec;
    if (std::filesystem::is_directory(windows_install, ec)) {
        return windows_install;
    }
    return std::filesystem::current_path();
}

namespace {

bool parse_vec3(std::string_view text, std::array<double, 3>& out) {
    std::size_t start = 0;
    for (int i = 0; i < 3; ++i) {
        const std::size_t comma = text.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        try {
            std::size_t used = 0;
            const std::string part(text.substr(start, end - start));
            out[static_cast<std::size_t>(i)] = std::stod(part, &used);
            if (used != part.size()) {
                return false;
            }
        } catch (const std::exception&) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

} // namespace

std::optional<Options> parse_args(int argc, char** argv, std::string& error) {
    Options options;
    options.game = default_game_dir();

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        auto value = [&](std::string_view name) -> std::optional<std::string_view> {
            if (i + 1 >= argc) {
                error = std::string(name) + " needs a value";
                return std::nullopt;
            }
            return std::string_view{argv[++i]};
        };

        if (arg == "--game" || arg == "-g") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.game = std::filesystem::path(std::string(*v));
        } else if (arg == "--level" || arg == "-l") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.level = std::string(*v);
        } else if (arg == "--sou1") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.sou1 = std::string(*v);
        } else if (arg == "--sou2") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.sou2 = std::string(*v);
        } else if (arg == "--save-out") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.save_out = std::filesystem::path(std::string(*v));
        } else if (arg == "--set") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.save_edits.emplace_back(std::string(*v));
        } else if (arg == "--wav") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.wav = std::filesystem::path(std::string(*v));
        } else if (arg == "--music") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.music = std::string(*v);
        } else if (arg == "--film") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.film = std::string(*v);
        } else if (arg == "--anim") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.anim = std::string(*v);
        } else if (arg == "--hud") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.hud = std::string(*v);
        } else if (arg == "--hud-screen") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.hud_screen = std::filesystem::path(std::string(*v));
        } else if (arg == "--hud-tick") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.hud_tick = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                error = "--hud-tick expects an integer (91 Hz ticks)";
                return std::nullopt;
            }
        } else if (arg == "--hud-x" || arg == "--hud-y" || arg == "--hud-scale") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                const int n = std::stoi(std::string(*v));
                if (arg == "--hud-x") options.hud_x = n;
                if (arg == "--hud-y") options.hud_y = n;
                if (arg == "--hud-scale") options.hud_scale = n;
            } catch (const std::exception&) {
                error = std::string(arg) + " expects an integer";
                return std::nullopt;
            }
        } else if (arg == "--hud-opacity") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.hud_opacity = std::stof(std::string(*v));
            } catch (const std::exception&) {
                error = "--hud-opacity expects a number";
                return std::nullopt;
            }
        } else if (arg == "--at") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.at = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                error = "--at expects an integer (milliseconds)";
                return std::nullopt;
            }
        } else if (arg == "--frame") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.frame = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                error = "--frame expects an integer";
                return std::nullopt;
            }
        } else if (arg == "--scene") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.scene = std::string(*v);
        } else if (arg == "--sample") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.sample = std::string(*v);
        } else if (arg == "--song") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.song = std::string(*v);
        } else if (arg == "--play") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.play = std::string(*v);
        } else if (arg == "--dump" || arg == "-d") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.dump = std::filesystem::path(std::string(*v));
        } else if (arg == "--png") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.png = std::filesystem::path(std::string(*v));
        } else if (arg == "--render") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.render = std::filesystem::path(std::string(*v));
        } else if (arg == "--model") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.model = std::filesystem::path(std::string(*v));
        } else if (arg == "--width") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            if (!parse_int(*v, options.width) || options.width <= 0) {
                error = "--width expects a positive integer";
                return std::nullopt;
            }
        } else if (arg == "--height") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            if (!parse_int(*v, options.height) || options.height <= 0) {
                error = "--height expects a positive integer";
                return std::nullopt;
            }
        } else if (arg == "--az" || arg == "--el" || arg == "--zoom" || arg == "--fov") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            float number = 0.0f;
            try {
                std::size_t used = 0;
                number = std::stof(std::string(*v), &used);
                if (used != v->size()) {
                    throw std::invalid_argument("trailing");
                }
            } catch (const std::exception&) {
                error = std::string(arg) + " expects a number";
                return std::nullopt;
            }
            if (arg == "--az") {
                options.az = number;
            } else if (arg == "--el") {
                options.el = number;
            } else if (arg == "--zoom") {
                options.zoom = number;
            } else {
                options.fov = number;
            }
        } else if (arg == "--no-textures") {
            options.no_textures = true;
        } else if (arg == "--no-lightmaps") {
            options.no_lightmaps = true;
        } else if (arg == "--lm-world") {
            options.lightmap_world = true;
        } else if (arg == "--wire") {
            options.wire = true;
        } else if (arg == "--mip") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            if (*v == "auto") {
                options.mip_auto = true;
            } else {
                try {
                    options.mip = std::stoi(std::string(*v));
                } catch (const std::exception&) {
                    error = "--mip expects an integer or 'auto'";
                    return std::nullopt;
                }
            }
        } else if (arg == "--filter") {
            const auto v = value(arg);
            if (!v || (*v != "nearest" && *v != "bilinear")) {
                error = "--filter expects nearest|bilinear";
                return std::nullopt;
            }
            options.bilinear = (*v == "bilinear");
        } else if (arg == "--uv-texel") {
            options.texel_uv = true;
        } else if (arg == "--skip-untextured") {
            options.skip_untextured = true;
        } else if (arg == "--haze") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.haze = std::stod(std::string(*v));
            } catch (...) {
                error = "--haze expects a number in 0..1";
                return std::nullopt;
            }
            if (!(options.haze >= 0.0 && options.haze <= 1.0)) {
                error = "--haze expects a number in 0..1";
                return std::nullopt;
            }
        } else if (arg == "--chromakey") {
            options.chromakey = true;
        } else if (arg == "--alpha-test") {
            options.alpha_test = true;
        } else if (arg == "--transparent-index") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.transparent_index = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                error = "--transparent-index expects an integer";
                return std::nullopt;
            }
        } else if (arg == "--sim") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.sim = std::string(*v);
        } else if (arg == "--ticks") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.sim_ticks = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                error = "--ticks expects an integer";
                return std::nullopt;
            }
            if (options.sim_ticks < 0 || options.sim_ticks > 10000000) {
                error = "--ticks expects 0..10000000";
                return std::nullopt;
            }
        } else if (arg == "--sim-players") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.sim_players = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                error = "--sim-players expects an integer";
                return std::nullopt;
            }
            if (options.sim_players < 1 || options.sim_players > 8) {
                error = "--sim-players expects 1..8";
                return std::nullopt;
            }
        } else if (arg == "--sim-mode") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.sim_mode = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                error = "--sim-mode expects an integer";
                return std::nullopt;
            }
        } else if (arg == "--sim-difficulty") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.sim_difficulty = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                error = "--sim-difficulty expects an integer";
                return std::nullopt;
            }
        } else if (arg == "--sim-drive") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.sim_drive = std::string(*v);
        } else if (arg == "--menu") {
            options.menu = true;
        } else if (arg == "--menu-json") {
            options.menu = true;
            options.menu_json = true;
        } else if (arg == "--menu-keys") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.menu = true;
            options.menu_keys = std::string(*v);
        } else if (arg == "--menu-screen") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.menu = true;
            options.menu_screen = std::string(*v);
        } else if (arg == "--sim-projectile") {
            options.sim_projectile = true;
        } else if (arg == "--sim-vitals") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.sim_vitals = std::string(*v);
        } else if (arg == "--sim-respawn") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.sim_respawn = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
            if (options.sim_respawn < 0) {
                return std::nullopt;
            }
        } else if (arg == "--god") {
            options.sim_god = true;
        } else if (arg == "--spawn-camera") {
            options.spawn_camera = true;
        } else if (arg == "--spawn-slot") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.spawn_slot = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                error = "--spawn-slot expects an integer";
                return std::nullopt;
            }
        } else if (arg == "--spawn-eye") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.spawn_eye = std::stof(std::string(*v));
            } catch (const std::exception&) {
                error = "--spawn-eye expects a number";
                return std::nullopt;
            }
        } else if (arg == "--third-person") {
            options.third_person = true;
        } else if (arg == "--third-distance" || arg == "--third-height") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                std::size_t used = 0;
                const float number = std::stof(std::string(*v), &used);
                if (used != v->size() || number <= 0.0f) {
                    throw std::invalid_argument("bad");
                }
                if (arg == "--third-distance") {
                    options.third_distance = number;
                } else {
                    options.third_height = number;
                }
            } catch (const std::exception&) {
                error = std::string(arg) + " expects a positive number";
                return std::nullopt;
            }
        } else if (arg == "--player-model") {
            const auto v = value(arg);
            if (!v || v->empty()) {
                error = "--player-model expects a SOBS/NAME";
                return std::nullopt;
            }
            options.player_model = std::string(*v);
        } else if (arg == "--third-pos") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            std::array<double, 3> vec{};
            if (!parse_vec3(*v, vec)) {
                error = "--third-pos expects X,Y,Z";
                return std::nullopt;
            }
            options.third_pos = vec;
            options.has_third_pos = true;
        } else if (arg == "--third-yaw") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                std::size_t used = 0;
                options.third_yaw = std::stof(std::string(*v), &used);
                if (used != v->size()) {
                    throw std::invalid_argument("trailing");
                }
            } catch (const std::exception&) {
                error = "--third-yaw expects degrees";
                return std::nullopt;
            }
            options.has_third_yaw = true;
        } else if (arg == "--player-scale") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                std::size_t used = 0;
                options.player_scale = std::stof(std::string(*v), &used);
                if (used != v->size() || !(options.player_scale > 0.0f)) {
                    throw std::invalid_argument("bad scale");
                }
            } catch (const std::exception&) {
                error = "--player-scale expects a positive number";
                return std::nullopt;
            }
        } else if (arg == "--player-origin") {
            options.player_origin = true;
        } else if (arg == "--things") {
            options.things = true;
        } else if (arg == "--thing-scale") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                std::size_t used = 0;
                options.thing_scale = std::stof(std::string(*v), &used);
                if (used != v->size()) {
                    throw std::invalid_argument("trailing");
                }
            } catch (const std::exception&) {
                error = "--thing-scale expects a number";
                return std::nullopt;
            }
        } else if (arg == "--info") {
            options.info = true;
        } else if (arg == "--pos" || arg == "--look") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            std::array<double, 3> vec{};
            if (!parse_vec3(*v, vec)) {
                error = std::string(arg) + " expects X,Y,Z";
                return std::nullopt;
            }
            if (arg == "--pos") {
                options.pos = vec;
                options.has_pos = true;
            } else {
                options.look = vec;
                options.has_look = true;
            }
        } else if (arg == "--clip") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.clip = std::stof(std::string(*v));
            } catch (const std::exception&) {
                error = "--clip expects a number";
                return std::nullopt;
            }
        } else if (arg == "--frames" || arg == "-n") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            if (!parse_int(*v, options.frames) || options.frames < 0) {
                error = "--frames expects a non-negative integer";
                return std::nullopt;
            }
        } else if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--verbose" || arg == "-v") {
            options.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            options.show_help = true;
        } else if (arg == "--version") {
            options.show_version = true;
        } else if (arg == "--") {
            break;
        } else {
            error = "unknown option: " + std::string(arg);
            return std::nullopt;
        }
    }
    return options;
}

std::string usage() {
    std::ostringstream os;
    os << "gunmetal " << kVersion << " — asset-complete runtime (M3 skeleton)\n\n"
       << "usage: gunmetal [options]\n\n"
       << "  -g, --game DIR    installation root (default: $GUNMETAL_DIR or\n"
       << "                    /mnt/d/games/Gunmetal 3dfx)\n"
       << "      --film NAME   inspect a Film/*.sof (tiles, .fdx, frame FNV)\n"
       << "      --frame N     frame for --film (default 0)\n"
       << "      --anim NAME   morph a Sobanim/NAME.soo pose into --render\n"
       << "      --at MS       animation time for --anim (default 0)\n"
       << "  -l, --level NAME  level to load (opens the viewer; with --info prints a summary)\n"
       << "  -d, --dump FILE   print size and the first bytes of FILE, then exit\n"
       << "      --png OUT     with --dump of a .sbm/.bmp: write OUT as PNG\n"
       << "      --save-out OUT with --dump of a save: re-serialise it to OUT\n"
       << "      --set K=V     edit a save field (ammo0..11, armor, shield0..3,\n"
       << "                    posx/posy/posz, weapon); repeatable\n"
       << "      --render OUT  render --level or --model into OUT (PNG)\n"
       << "      --model FILE  render a .sob model instead of a level\n"
       << "      --width N     render/window width (default 640)\n"
       << "      --height N    render/window height (default 400)\n"
       << "      --az DEG      camera azimuth (default 35)\n"
       << "      --el DEG      camera elevation (default 22)\n"
       << "      --zoom X      camera zoom (default 1)\n"
       << "      --fov DEG     field of view (default 45)\n"
       << "      --no-textures flat colours instead of archive textures\n"
       << "      --no-lightmaps ignore .sbc lightmaps\n"
       << "      --wire        draw polygon edges\n"
       << "      --mip N|auto  fixed mip level (default 0) or screen-space LOD\n"
       << "      --filter M    nearest|bilinear texture filtering\n"
       << "      --alpha-test  drop texels equal to --transparent-index\n"
       << "      --uv-texel    engine texture projection (repeat) instead of\n"
       << "                    stretching one copy (docs/TEXUV.md)\n"
       << "      --skip-untextured  skip polygons with texture==0 and handle==0\n"
       << "      --haze N           white fade 0..1 (SET_CAMERA_HAZE, docs/SCREEN_FX.md)\n"
       << "      --chromakey   same, but only for textures whose .sbm flags bit 0\n"
       << "                    is set (the game's transparent type, docs/ALPHA.md)\n"
       << "      --transparent-index N  palette key (default 0)\n"
       << "      --things      place the scene's ADD_THING models\n"
       << "      --thing-scale X  extra scale for those models (default 1)\n"
       << "      --third-person   place the player chassis at SET_PLAYER_START and\n"
       << "                       view it from behind (with --render or the viewer)\n"
       << "      --third-distance X  camera distance behind the chassis (default 220)\n"
       << "      --third-height X    camera height above the player eye (default 40)\n"
       << "      --third-pos X,Y,Z   photo override: chassis feet position (world)\n"
       << "      --third-yaw DEG     photo override: chassis yaw (default: spawn yaw)\n"
       << "      --player-model NAME chassis .sob under SOBS/ (default TICK, LIGHT)\n"
       << "      --player-scale X    chassis mesh scale (default 1)\n"
       << "      --player-origin     put the .sob origin at the spawn (default: feet)\n"
       << "      --info        print the level summary instead of opening the viewer\n"
       << "      --play NAME   play Vid/NAME.sbv (--frames limits the frame count)\n"
       << "      --song NAME   list the MUSIC/NAME.sog playlist and its SOU.DAT entries\n"
       << "      --sample NAME resolve a sample name like the engine (sounds, then music)\n"
       << "      --music NAME  play MUSIC/NAME.sog through the SOG playlist\n"
       << "      --scene NAME  print the level script scene (lights/doors/items...)\n"
       << "      --sim NAME    headless fixed-tick run of a level (no window)\n"
       << "      --ticks N     ticks for --sim (91 Hz, default 1)\n"
       << "      --sim-players N  VM player count for --sim (1..8)\n"
       << "      --sim-mode N     VM game_mode for --sim (0 = campaign)\n"
       << "      --sim-difficulty N  VM difficulty for --sim\n"
       << "      --god         run --sim with the GOD_MODE branch (57 CHEVY)\n"
       << "      --sim-drive PLAN  drive the --sim player: forward=1:900;turn=0.5:300;fire:200\n"
       << "      --sim-projectile  the driven player fires rounds (blast damage), not hitscan\n"
       << "      --sim-vitals ARMOR,SHIELD  starting gauges of the driven player\n"
       << "      --sim-respawn N  respawn the driven player N ticks after death (0 = now)\n"
       << "      --menu        dump the headless frontend screen (M8)\n"
       << "      --menu-keys up,down,enter,esc,text:0x1b  feed these keys and print each screen\n"
       << "      --menu-screen ID  start on that UI_SCREENS §2 screen id\n"
       << "      --menu-json   machine-readable dump\n"
       << "      --sou1 NAME   decode a SOU1 entry to --wav OUT (PCM16 at source rate)\n"
       << "      --sou2 NAME   decode a SOU2 entry to --wav OUT (PCM16)\n"
       << "      --wav OUT     output file for --sou2\n"
       << "      --pos X,Y,Z   camera eye in world space (with --look)\n"
       << "      --look X,Y,Z  camera target in world space\n"
       << "      --clip N      drop faces farther than N from the eye\n"
       << "      --headless    do not create a window\n"
       << "  -n, --frames N    stop after N frames (default: until closed)\n"
       << "  -v, --verbose     debug logging\n"
       << "  -h, --help        this text\n"
       << "      --version     print the version\n\n"
       << "The toolkit (gm.py) is the reference implementation of the formats;\n"
       << "the engine consumes the same layouts (see docs/FORMATS.md).\n";
    return os.str();
}

} // namespace gm
