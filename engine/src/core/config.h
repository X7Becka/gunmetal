#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace gm {

inline constexpr std::string_view kVersion = "0.1.0";

struct Options {
    std::filesystem::path game;
    std::string level;
    std::filesystem::path dump;
    std::filesystem::path png;
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
