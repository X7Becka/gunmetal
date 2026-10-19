#include "core/bytes.h"
#include "core/config.h"
#include "core/log.h"
#include "core/path.h"
#include "core/time.h"
#include "platform/window.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

int dump_file(const gm::Options& options) {
    fs::path path = options.dump;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        if (path.is_relative()) {
            if (const auto resolved = gm::resolve_ci(options.game, path.string())) {
                path = *resolved;
            }
        }
    }
    if (!fs::exists(path, ec)) {
        gm::logf(gm::LogLevel::Error, "dump", "no such file: ", options.dump.string());
        return 2;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        gm::logf(gm::LogLevel::Error, "dump", "cannot open: ", path.string());
        return 2;
    }
    const auto size = fs::file_size(path, ec);
    if (ec) {
        gm::logf(gm::LogLevel::Warn, "dump", "file_size: ", ec.message());
    }
    std::vector<std::uint8_t> head(64);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));

    std::printf("file: %s\n", path.string().c_str());
    std::printf("size: %s bytes\n", ec ? "?" : std::to_string(size).c_str());
    std::printf("head: %s\n", gm::format_hex(head).c_str());
    std::printf("ascii: %s\n", gm::format_ascii(head).c_str());
    return 0;
}

int run(const gm::Options& options) {
    std::string error;
    auto window = gm::Window::open("Gunmetal", 320, 200, options.headless, error);
    if (!window) {
        gm::logf(gm::LogLevel::Error, "video", error);
        return 1;
    }
    gm::logf(gm::LogLevel::Info, "run", "game=", options.game.string(),
             options.headless ? " headless" : "");
    if (!options.level.empty()) {
        gm::logf(gm::LogLevel::Warn, "run",
                 "level loading is not implemented yet (M3.2): ", options.level);
    }

    gm::FrameClock clock;
    int frame = 0;
    while (window->pump()) {
        window->fill_gradient(frame);
        window->present();
        (void)clock.tick();
        ++frame;
        if (options.frames >= 0 && frame >= options.frames) {
            break;
        }
        if (options.headless) {
            continue;
        }
    }
    const double elapsed = clock.elapsed();
    const double fps = elapsed > 0.0 ? frame / elapsed : 0.0;
    gm::logf(gm::LogLevel::Info, "run", "frames=", frame, " elapsed=", elapsed, "s fps=", fps);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    gm::set_log_level_from_env();

    std::string error;
    const auto options = gm::parse_args(argc, argv, error);
    if (!options) {
        std::fprintf(stderr, "gunmetal: %s\n\n%s", error.c_str(), gm::usage().c_str());
        return 2;
    }
    if (options->show_version) {
        std::printf("gunmetal %s\n", gm::kVersion.data());
        return 0;
    }
    if (options->show_help) {
        std::fputs(gm::usage().c_str(), stdout);
        return 0;
    }
    if (options->verbose) {
        gm::set_log_level(gm::LogLevel::Debug);
    }
    if (!options->dump.empty()) {
        return dump_file(*options);
    }
    return run(*options);
}
