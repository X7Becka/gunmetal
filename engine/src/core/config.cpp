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
        } else if (arg == "--info") {
            options.info = true;
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
       << "  -l, --level NAME  level to load (opens the viewer; with --info prints a summary)\n"
       << "  -d, --dump FILE   print size and the first bytes of FILE, then exit\n"
       << "      --png OUT     with --dump of a .sbm/.bmp: write OUT as PNG\n"
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
       << "      --info        print the level summary instead of opening the viewer\n"
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
