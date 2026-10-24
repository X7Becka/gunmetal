#include "core/config.h"

#include <cmath>
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

/// Comma-separated finite doubles with `min_count..max_count` components
/// (`--listener X,Y,Z[,YAW]`, docs/AUDIO_3D.md §13).
bool parse_double_list(std::string_view text, std::vector<double>& out, std::size_t min_count,
                       std::size_t max_count) {
    out.clear();
    std::size_t start = 0;
    for (;;) {
        const std::size_t comma = text.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        try {
            std::size_t used = 0;
            const std::string part(text.substr(start, end - start));
            const double parsed = std::stod(part, &used);
            if (used != part.size() || !std::isfinite(parsed)) {
                return false;
            }
            out.push_back(parsed);
        } catch (const std::exception&) {
            return false;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return out.size() >= min_count && out.size() <= max_count;
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
        } else if (arg == "--sample-play") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.sample_play = std::string(*v);
        } else if (arg == "--twice") {
            options.twice = true;
        } else if (arg == "--listener") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            std::vector<double> parts;
            if (!parse_double_list(*v, parts, 3, 4)) {
                error = "--listener expects X,Y,Z[,YAW] (yaw in radians)";
                return std::nullopt;
            }
            options.listener = {parts[0], parts[1], parts[2]};
            options.listener_yaw = parts.size() == 4 ? parts[3] : 0.0;
            options.has_listener = true;
        } else if (arg == "--volume") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            if (!parse_int(*v, options.sample_volume) || options.sample_volume < 1 ||
                options.sample_volume > 32) {
                error = "--volume expects an integer 1..32";
                return std::nullopt;
            }
        } else if (arg == "--atten") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            std::vector<double> parts;
            if (!parse_double_list(*v, parts, 1, 1) || parts[0] < 0.0) {
                error = "--atten expects a non-negative number";
                return std::nullopt;
            }
            options.sample_atten = parts[0];
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
        } else if (arg == "--png-scale") {
            // `--png-scale N`: repeat every pixel of the front-end frame as an
            // N x N block (`render/upscale.h`, docs/UPSCALE.md §2).  The limit
            // is `kUpscaleMaxScale` (8 -> 2560x1600), not an arithmetic bound.
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.png_scale = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
            if (options.png_scale < 1 || options.png_scale > 8) {
                return std::nullopt;
            }
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
        } else if (arg == "--fade-in" || arg == "--fade-out") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                const double seconds = std::stod(std::string(*v));
                if (!(seconds >= 0.0) || !std::isfinite(seconds)) {
                    error = std::string(arg) + " wants a non-negative number of seconds";
                    return std::nullopt;
                }
                if (arg == "--fade-in") {
                    options.music_fade_in = seconds;
                } else {
                    options.music_fade_out = seconds;
                }
            } catch (const std::exception&) {
                error = std::string(arg) + " wants a number of seconds";
                return std::nullopt;
            }
        } else if (arg == "--save-slots") {
            options.save_slots = true;
        } else if (arg == "--save-load") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.save_load = std::string(*v);
        } else if (arg == "--save-load-ticks") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.save_load_ticks = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
            if (options.save_load_ticks < 0) {
                return std::nullopt;
            }
        } else if (arg == "--pager") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.pager = std::string(*v);
        } else if (arg == "--pager-keys") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.pager_keys = std::string(*v);
        } else if (arg == "--page") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.pager_page = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
            if (options.pager_page < 1) {
                return std::nullopt;
            }
        } else if (arg == "--all") {
            options.pager_all = true;
        } else if (arg == "--background") {
            // `--pager ... --png OUT --background`: HELPSCRN before the text
            // (docs/PAGER_RENDER.md §2.4).
            options.pager_background = true;
        } else if (arg == "--fade-lut") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.pager_fade_lut = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--fade") {
            // `--fade N`: engine fade level 0..15 (0 = black, 11 = identity),
            // docs/PAGER_RENDER.md §2.5.  Not to be confused with `--fade-in`
            // (music).
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.pager_fade = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
            if (options.pager_fade < 0 || options.pager_fade > 15) {
                return std::nullopt;
            }
            // The same flag fades a `--menu --png` screen: one curve, one
            // spelling (`docs/UPSCALE.md` §6).
            options.menu_fade = options.pager_fade;
        } else if (arg == "--playlist") {
            // `--playlist FILE`: a `.sog` list resolved against `MUSIC/SOU.IDX`
            // (`audio/playlist.h`, docs/PLAYLIST.md).
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.playlist = std::string(*v);
        } else if (arg == "--best-times") {
            // `--best-times`: the 9 `GUNMETAL.HI` rows and the minimum per
            // level name (`ui/records.h`, docs/HI_SCORES.md).
            options.best_times = true;
        } else if (arg == "--killed") {
            // `--killed NAME`: the death messages of FRONTEND/KILLED.skb with
            // `%S` replaced by NAME (`ui/killed.h`, docs/KILLED.md).
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.killed_name = std::string(*v);
        } else if (arg == "--killed-all") {
            options.killed_all = true;
        } else if (arg == "--killed-seed") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.killed_seed = static_cast<unsigned>(std::stoul(std::string(*v)));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--hud-png") {
            // `--hud-png OUT`: the composed cockpit frame (panel, icons,
            // numbers) as a 320x200 PNG (`render/cockpit.h`, docs/COCKPIT.md).
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.hud_png = std::filesystem::path(std::string(*v));
        } else if (arg == "--hud-weapon" || arg == "--hud-ammo" ||
                   arg == "--hud-armor" || arg == "--hud-kills" ||
                   arg == "--hud-speed") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                if (arg == "--hud-speed") {
                    options.hud_speed = std::stod(std::string(*v));
                } else {
                    const int number = std::stoi(std::string(*v));
                    if (arg == "--hud-weapon") {
                        options.hud_weapon = number;
                    } else if (arg == "--hud-ammo") {
                        options.hud_ammo = number;
                    } else if (arg == "--hud-armor") {
                        options.hud_armor = number;
                    } else {
                        options.hud_kills = number;
                    }
                }
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--briefing") {
            // `--briefing LEVEL`: the mission briefing of `SML/brief/<level>.sme`
            // (`ui/briefing.h`, docs/BRIEFING.md).  A level name, not a path.
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.briefing = std::string(*v);
        } else if (arg == "--briefing-text") {
            options.briefing_text = true;
        } else if (arg == "--level-select") {
            // `--level-select`: screen 17, `SELECT LEVEL FOR NEW GAME`
            // (`ui/level_select_render.*`, docs/LEVEL_SELECT_RENDER.md).
            options.level_select = true;
        } else if (arg == "--level-select-counter" ||
                   arg == "--level-select-row") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                const int number = std::stoi(std::string(*v));
                if (arg == "--level-select-counter") {
                    options.level_select_counter = number;
                } else {
                    options.level_select_row = number;
                }
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--no-plate") {
            options.level_select_no_plate = true;
        } else if (arg == "--save-menu") {
            // `--save-menu`: screen 14, the six-row SAVE GAME list
            // (`ui/save_menu_render.*`, docs/SAVE_MENU_RENDER.md).
            options.save_menu = true;
        } else if (arg == "--save-menu-slot") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.save_menu_slot = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--keycfg") {
            // `--keycfg NAME`: screen 30, the key-set viewer
            // (`ui/keycfg_render.*`, docs/KEYCFG_RENDER.md).
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.keycfg = std::string(*v);
        } else if (arg == "--game-type") {
            // `--game-type`: screen 19, MULTIPLAYER GAME TYPE
            // (`ui/game_type_render.*`, docs/GAME_TYPE_RENDER.md).
            options.game_type = true;
        } else if (arg == "--game-type-cursor") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.game_type_cursor = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--game-type-mask") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.game_type_mask = static_cast<unsigned>(
                    std::stoul(std::string(*v), nullptr, 0));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--video-mode-list") {
            // `--video-mode-list`: screen 18, SELECT VIDEO MODE
            // (`ui/video_mode_list_render.*`, docs/VIDEO_MODE_LIST_RENDER.md).
            options.video_mode_list = true;
        } else if (arg == "--video-mode-list-cursor") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.video_mode_list_cursor = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--player-name") {
            // `--player-name`: screens 24/25
            // (`ui/player_name_render.*`, docs/PLAYER_NAME_RENDER.md).
            options.player_name = true;
        } else if (arg == "--player-name-player") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.player_name_player = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--player-name-text") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.player_name_text = std::string(*v);
        } else if (arg == "--player-name-caret-pos") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.player_name_caret_pos = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--player-name-no-caret") {
            options.player_name_no_caret = true;
        } else if (arg == "--new-keyset") {
            // `--new-keyset`: screen 31
            // (`ui/new_keyset_render.*`, docs/NEW_KEYSET_RENDER.md).
            options.new_keyset = true;
        } else if (arg == "--new-keyset-text") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.new_keyset_text = std::string(*v);
        } else if (arg == "--new-keyset-no-caret") {
            options.new_keyset_no_caret = true;
        } else if (arg == "--graphics-options") {
            // `--graphics-options`: screen 12
            // (`ui/graphics_options_render.*`, docs/GRAPHICS_OPTIONS_RENDER.md).
            options.graphics_options = true;
        } else if (arg == "--graphics-options-cursor") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.graphics_options_cursor = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--graphics-options-border") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.graphics_options_border = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--graphics-options-gamma") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.graphics_options_gamma = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--graphics-options-reflections") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.graphics_options_reflections = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--graphics-options-light-maps") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.graphics_options_light_maps = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--play-options") {
            // `--play-options`: screen 32
            // (`ui/play_options_render.*`, docs/PLAY_OPTIONS_RENDER.md).
            options.play_options = true;
        } else if (arg == "--play-options-cursor") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.play_options_cursor = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--play-options-flags") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.play_options_flags = static_cast<int>(
                    std::stoul(std::string(*v), nullptr, 0));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--play-options-camera") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.play_options_camera = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--play-options-in-game") {
            options.play_options_in_game = true;
        } else if (arg == "--control-options") {
            // `--control-options`: screen 10
            // (`ui/control_options_render.*`, docs/CONTROL_OPTIONS_RENDER.md).
            options.control_options = true;
        } else if (arg == "--control-options-cursor") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.control_options_cursor = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--control-options-keyset1") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.control_options_keyset1 = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--control-options-keyset2") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.control_options_keyset2 = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--control-options-joystick1") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.control_options_joystick1 = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--control-options-joystick2") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.control_options_joystick2 = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--control-options-mouse") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.control_options_mouse = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--sound-options") {
            // `--sound-options`: screen 11
            // (`ui/sound_options_render.*`, docs/SOUND_OPTIONS_RENDER.md).
            options.sound_options = true;
        } else if (arg == "--sound-options-cursor") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.sound_options_cursor = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--sound-options-music") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.sound_options_music = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--sound-options-effects") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.sound_options_effects = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--sound-options-swap") {
            options.sound_options_swap = true;
        } else if (arg == "--sound-options-off") {
            options.sound_options_off = true;
        } else if (arg == "--sound-options-low-memory") {
            options.sound_options_low_memory = true;
        } else if (arg == "--player-setup") {
            // `--player-setup`: screen 36, PLAYER 1/2 JOYSTICK SETUP
            // (`ui/player_setup_render.*`, docs/PLAYER_SETUP_RENDER.md).
            options.player_setup = true;
        } else if (arg == "--player-setup-cursor") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.player_setup_cursor = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--player-setup-joystick") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.player_setup_joystick = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--player-setup-player") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.player_setup_player = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--error") {
            // `--error`: screen 5, the ERROR screen
            // (`ui/error_render.*`, docs/ERROR_RENDER.md).
            options.error_screen = true;
        } else if (arg == "--error-message") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.error_message = std::string(*v);
        } else if (arg == "--lay-bucks") {
            // `--lay-bucks`: screen 20, the shareware stub
            // (`ui/lay_bucks_render.*`, docs/LAY_BUCKS_RENDER.md).
            options.lay_bucks = true;
        } else if (arg == "--save-name") {
            // `--save-name`: screen 15, SAVE GAME + name entry
            // (`ui/save_name_render.*`, docs/SAVE_NAME_RENDER.md).
            options.save_name = true;
        } else if (arg == "--save-name-text") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.save_name_text = std::string(*v);
        } else if (arg == "--save-name-row") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.save_name_row = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--save-name-no-caret") {
            options.save_name_no_caret = true;
        } else if (arg == "--no-fileslot") {
            options.save_name_no_fileslot = true;
        } else if (arg == "--hud-lut-dump") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.hud_lut_dump = std::string(*v);
        } else if (arg == "--ingame-menu") {
            // `--ingame-menu`: screen 9, the full-screen DEMOTION/DISBAND
            // frame (`ui/ingame_menu_render.*`, docs/INGAME_MENU_RENDER.md).
            options.ingame_menu = true;
        } else if (arg == "--ingame-menu-bg") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.ingame_menu_bg = std::string(*v);
        } else if (arg == "--key-jamming") {
            // `--key-jamming`: screen 16, KEY JAMMING
            // (`ui/key_jamming_render.*`, docs/KEY_JAMMING_RENDER.md).
            options.key_jamming = true;
        } else if (arg == "--key-jamming-keys") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            std::string text(*v);
            std::size_t start = 0;
            while (start <= text.size()) {
                const std::size_t comma = text.find(',', start);
                const std::string item =
                    text.substr(start, comma == std::string::npos ? std::string::npos
                                                                 : comma - start);
                if (!item.empty()) {
                    try {
                        options.key_jamming_keys.push_back(std::stoi(item));
                    } catch (const std::exception&) {
                        return std::nullopt;
                    }
                }
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
        } else if (arg == "--key-jamming-cursor") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.key_jamming_cursor = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--match-setup") {
            // `--match-setup`: screen 27, MULTIPLAY GAME OPTIONS
            // (`ui/match_setup_render.*`, docs/MATCH_SETUP_RENDER.md).
            options.match_setup = true;
        } else if (arg == "--match-setup-cursor") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.match_setup_cursor = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--match-setup-type") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.match_setup_type = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--match-setup-cash") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.match_setup_cash = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--match-setup-flags") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.match_setup_flags = static_cast<int>(
                    std::stoul(std::string(*v), nullptr, 0));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--joystick-centering") {
            // `--joystick-centering`: screen 26, JOYSTICK CENTERING
            // (`ui/joystick_centering_render.*`, docs/JOYSTICK_CENTERING_RENDER.md).
            options.joystick_centering = true;
        } else if (arg == "--joystick-centering-cursor") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.joystick_centering_cursor = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--mouse-setup") {
            // `--mouse-setup`: screen 22, MOUSE SETUP
            // (`ui/mouse_setup_render.*`, docs/MOUSE_SETUP_RENDER.md).
            options.mouse_setup = true;
        } else if (arg == "--mouse-setup-cursor") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.mouse_setup_cursor = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--things-anim") {
            // `--things-anim`: bind every thing to its clip/frame
            // (`world/thing_anim.*`, docs/THING_ANIM_BIND.md).
            options.things_anim = true;
        } else if (arg == "--things-anim-tick") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.things_anim_tick = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--things-anim-limit") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.things_anim_limit = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--restart") {
            // `--restart`: screen 6, RESTART GAME
            // (`ui/restart_render.*`, docs/RESTART_RENDER.md).
            options.restart = true;
        } else if (arg == "--restart-items") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.restart_items = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--restart-cursor") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.restart_cursor = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--video-mode") {
            // `--video-mode`: screen 37, NEW VIDEO MODE OK?
            // (`ui/video_mode_render.*`, docs/VIDEO_MODE_RENDER.md).
            options.video_mode = true;
        } else if (arg == "--video-mode-cursor") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.video_mode_cursor = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--quit") {
            // `--quit`: screen 33, QUIT GAME? / QUIT OPTIONS
            // (`ui/quit_render.*`, docs/QUIT_RENDER.md).
            options.quit = true;
        } else if (arg == "--quit-in-game") {
            options.quit_in_game = true;
        } else if (arg == "--quit-cursor") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.quit_cursor = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--local-players") {
            // `--local-players`: screen 21, NUMBER OF LOCAL PLAYERS
            // (`ui/local_players_render.*`, docs/LOCAL_PLAYERS_RENDER.md).
            options.local_players = true;
        } else if (arg == "--local-players-cursor") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.local_players_cursor = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--difficulty") {
            // `--difficulty`: screen 23, SET DIFFICULTY LEVEL
            // (`ui/difficulty_render.*`, docs/DIFFICULTY_RENDER.md).
            options.difficulty = true;
        } else if (arg == "--difficulty-cursor") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.difficulty_cursor = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--keycfg-mode") {
            const auto v = value(arg);
            if (!v || (*v != "view" && *v != "edit")) {
                return std::nullopt;
            }
            options.keycfg_mode = std::string(*v);
        } else if (arg == "--keycfg-cell") {
            const auto v = value(arg);
            if (!v || (*v != "a" && *v != "b")) {
                return std::nullopt;
            }
            options.keycfg_cell = std::string(*v);
        } else if (arg == "--keycfg-conflict") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.keycfg_conflict = std::string(*v);
        } else if (arg == "--keycfg-cursor") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            try {
                options.keycfg_cursor = std::stoi(std::string(*v));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (arg == "--start-game") {
            // `--start-game`: screen 13, `New Single Player Game` + slots
            // (`ui/start_game_render.*`, docs/START_GAME_RENDER.md).
            options.start_game = true;
        } else if (arg == "--save-action") {
            // `--save-action save|load`: apply the SAVE GAME action on the
            // container (`game/save_action.*`, docs/SAVE_ACTION.md).
            const auto v = value(arg);
            if (!v || (*v != "load" && *v != "save")) {
                return std::nullopt;
            }
            options.save_action = std::string(*v);
        } else if (arg == "--save-action-out") {
            const auto v = value(arg);
            if (!v) {
                return std::nullopt;
            }
            options.save_action_out = std::string(*v);
        } else if (arg == "--save-menu-mode") {
            const auto v = value(arg);
            if (!v || (*v != "load" && *v != "save")) {
                return std::nullopt;
            }
            options.save_menu_mode = std::string(*v);
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
       << "      --png OUT     with --dump of a .sbm/.bmp or with --pager: write OUT as PNG\n"
       << "      --png-scale N with --menu/--pager --png: integer zoom 1..8 (no filtering)\n"       << "      --playlist FILE  resolve a MUSIC/*.sog playlist against MUSIC/SOU.IDX\n"
       << "      --best-times     print the GUNMETAL.HI rows and best time per level\n"
       << "      --killed NAME    print the KILLED screen lines with %S = NAME\n"
       << "      --killed-all     with --killed: print every accepted line\n"
       << "      --killed-seed N  with --killed: which line pick() returns (seed % n)\n"
       << "      --hud-png OUT    composed cockpit frame (panel/icons/numbers) as PNG\n"
       << "      --hud-weapon N   with --hud-png: weapon slot 0..18 (-1 = none)\n"
       << "      --hud-ammo N     with --hud-png: rounds (default 0)\n"
       << "      --hud-armor N    with --hud-png: armour in hundredths (default 10000)\n"
       << "      --hud-kills N    with --hud-png: kill counter\n"
       << "      --hud-speed X    with --hud-png: throttle fraction 0..1\n"
       << "      --hud NAME    with --render: overlay a Film/*.sof HUD film\n"
       << "      --hud-screen F  with --render: compose the HUD from a BMP entry\n"
       << "      --hud-tick N  HUD clock in 91 Hz ticks (default 0)\n"
       << "      --hud-x N / --hud-y N   HUD layer origin (default 0,0)\n"
       << "      --hud-scale N   integer HUD zoom 1..8 (default 1)\n"
       << "      --hud-opacity X HUD alpha 0..1 (default 1)\n"
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
       << "      --lm-world    project lightmaps in world space\n"
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
       << "      --sample-play NAME  decode a level sample through SampleCache and play it\n"
       << "      --twice       with --sample-play: acquire the same name twice (cache hit)\n"
       << "      --pos X,Y,Z   camera eye; with --sample-play: the source position\n"
       << "      --listener X,Y,Z[,YAW]  with --sample-play: listener pose, YAW in radians\n"
       << "                    (default 0,0,0,0); turns on the 3D path, stereo output\n"
       << "      --volume N    with --sample-play: 5-bit volume 1..32 (default 32 = 64)\n"
       << "      --atten A     with --sample-play: falloff per unit (default 0.03)\n"
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
       << "      --spawn-camera   start the viewer at the campaign spawn\n"
       << "      --spawn-slot N   campaign spawn slot to use (default 0)\n"
       << "      --spawn-eye X    add X to the spawn eye height (default 0)\n"
       << "      --menu        dump the headless frontend screen (M8)\n"
       << "      --menu-keys up,down,enter,esc,text:0x1b  feed these keys and print each screen\n"
       << "      --menu-screen ID  start on that UI_SCREENS §2 screen id\n"
       << "      --menu-json   machine-readable dump\n"
       << "      --fade-in SEC  music: linear fade in (docs/MUSIC.md)\n"
       << "      --fade-out SEC music: linear fade out at the end\n"
       << "      --pager FILE  headless SML pager: status, lines, pages (M8.1)\n"
       << "      --pager-keys next,next,prev,first,last  feed the pager\n"
       << "      --briefing LEVEL  print the mission briefing of SML/brief/LEVEL.sme\n"
       << "      --level-select    screen 17 rows; --png OUT [--level-select-counter N]\n"
       << "                        [--level-select-row N] [--no-plate] [--png-scale N]\n"
       << "      --save-menu       screen 14 slots; --png OUT [--save-menu-slot N]\n"
       << "                        [--save-menu-mode load|save] [--no-plate]\n"
       << "      --game-type       screen 19 game types; --png OUT [--game-type-mask HEX]\n"
       << "                        [--game-type-cursor N] [--no-plate]\n"
       << "      --difficulty      screen 23 difficulty; --png OUT [--difficulty-cursor N]\n"
       << "      --local-players   screen 21 player count; --png OUT [--local-players-cursor N]\n"
       << "      --video-mode-list screen 18 video modes; --png OUT [--video-mode-list-cursor N]\n"
       << "      --mouse-setup     screen 22 mouse setup; --png OUT [--mouse-setup-cursor N]\n"
       << "      --ingame-menu     screen 9 DEMOTION/DISBAND; --png OUT [--ingame-menu-bg NAME]\n"
       << "      --player-name     screens 24/25 player names; --png OUT [--player-name-player N]\n"
       << "                        [--player-name-text NAME] [--player-name-caret-pos N]\n"
       << "                        [--player-name-no-caret]\n"
       << "      --new-keyset      screen 31 new key set name; --png OUT [--new-keyset-text NAME]\n"
       << "                        [--new-keyset-no-caret]\n"
       << "      --graphics-options screen 12 graphics options; --png OUT [--graphics-options-cursor N]\n"
       << "                        [--graphics-options-border N] [--graphics-options-gamma N]\n"
       << "                        [--graphics-options-reflections N] [--graphics-options-light-maps N]\n"
       << "      --play-options    screen 32 play options; --png OUT [--play-options-cursor N]\n"
       << "                        [--play-options-flags HEX] [--play-options-camera N] [--play-options-in-game]\n"
       << "      --control-options screen 10 control options; --png OUT [--control-options-cursor N]\n"
       << "                        [--control-options-keyset1 N] [--control-options-keyset2 N]\n"
       << "                        [--control-options-joystick1 N] [--control-options-joystick2 N]\n"
       << "                        [--control-options-mouse N]\n"
       << "      --sound-options   screen 11 sound options; --png OUT [--sound-options-cursor N]\n"
       << "                        [--sound-options-music N] [--sound-options-effects N]\n"
       << "                        [--sound-options-swap] [--sound-options-off] [--sound-options-low-memory]\n"
       << "      --player-setup    screen 36 joystick setup; --png OUT [--player-setup-cursor N]\n"
       << "                        [--player-setup-joystick N] [--player-setup-player N]\n"
       << "      --error           screen 5 error text; --png OUT [--error-message TEXT]\n"
       << "      --lay-bucks       screen 20 shareware stub; --png OUT [--no-plate]\n"
       << "      --save-name       screen 15 save name; --png OUT [--save-name-text NAME]\n"
       << "                        [--save-name-row N] [--save-name-no-caret] [--no-fileslot]\n"
       << "      --hud-lut-dump OUT  build the 16x256 fade tables from GAMEPAL and write them\n"
       << "      --key-jamming     screen 16 key jamming; --png OUT [--key-jamming-keys 30,31]\n"
       << "                        [--key-jamming-cursor N]\n"
       << "      --match-setup     screen 27 match options; --png OUT [--match-setup-cursor N]\n"
       << "                        [--match-setup-type N] [--match-setup-cash N] [--match-setup-flags HEX]\n"
       << "      --joystick-centering  screen 26 joystick centering; --png OUT\n"
       << "                        [--joystick-centering-cursor N]\n"
       << "      --things-anim     bind level things to clips/frames (M6.3.1)\n"
       << "                        [--level NAME] [--things-anim-tick N] [--things-anim-limit N]\n"
       << "      --restart         screen 6 restart menu; --png OUT [--restart-items N]\n"
       << "                        [--restart-cursor N] [--no-plate]\n"
       << "      --video-mode      screen 37 video mode OK?; --png OUT [--video-mode-cursor N]\n"
       << "      --quit            screen 33 quit dialog; --png OUT [--quit-in-game]\n"
       << "                        [--quit-cursor N] [--no-plate]\n"
       << "      --keycfg NAME     screen 30 key set (KEYCFG/NAME.key); --png OUT\n"
       << "                        [--keycfg-cursor N] (0..30) [--keycfg-mode view|edit]\n"
       << "                        [--keycfg-cell a|b] [--keycfg-conflict TEXT]\n"
       << "                        [--no-plate] [--png-scale N]\n"
       << "      --start-game      screen 13 slots; --png OUT [--save-menu-slot N]\n"
       << "                        [--no-plate] [--png-scale N]\n"
       << "      --save-action save|load  write/apply a slot; --save-menu-slot N\n"
       << "                        [--save-action-out DIR] (default /tmp/opencode/save-action)\n"
       << "      --briefing-text   with --briefing: print every page's text\n"
       << "      --page N      show page N (1-based)\n"
       << "      --all         with --pager: print every page\n"
       << "      --background  with --pager --png: draw HELPSCRN behind the text\n"
       << "      --fade N      with --pager/--menu --png: engine fade level 0..15\n"
       << "                    (0 = black, 11 = identity; model, see docs)\n"
       << "      --fade-lut N  with --pager --png: exact 0x682f90 tables (0..15);\n"
       << "                    dump from $GM_HUD_LUT or ~/.cache/gunmetal-re/hud_lut.bin\n"
       << "      --save-slots  headless save-slot list (20 slots, 6 visible)\n"
       << "      --save-load SLOT|PATH  load a save into the runtime (M8.7/M9)\n"
       << "      --save-load-ticks N    fixed ticks after --save-load (default 0)\n"
       << "      --sou1 NAME   decode a SOU1 entry to --wav OUT (PCM16 at source rate)\n"
       << "      --sou2 NAME   decode a SOU2 entry to --wav OUT (PCM16)\n"
       << "      --wav OUT     output file for --sou2\n"
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
