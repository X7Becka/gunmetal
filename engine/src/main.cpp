#include "core/bytes.h"
#include "core/config.h"
#include "core/log.h"
#include "core/path.h"
#include "core/time.h"
#include "platform/window.h"
#include "res/bigfile.h"
#include "res/bmp.h"
#include "res/png.h"
#include "res/sbm.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::optional<std::vector<std::uint8_t>> read_file(const fs::path& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open: " + path.string();
        return std::nullopt;
    }
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

bool write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

std::vector<std::uint8_t> bmp_to_rgba(const gm::BmpImage& bmp) {
    std::vector<std::uint8_t> rgba(bmp.pixels.size() * 4, 0);
    const std::size_t colors = bmp.palette.size() / 4;
    for (std::size_t i = 0; i < bmp.pixels.size(); ++i) {
        const std::size_t index = bmp.pixels[i];
        if (index < colors) {
            rgba[i * 4 + 0] = bmp.palette[index * 4 + 0];
            rgba[i * 4 + 1] = bmp.palette[index * 4 + 1];
            rgba[i * 4 + 2] = bmp.palette[index * 4 + 2];
            rgba[i * 4 + 3] = 255;
        }
    }
    return rgba;
}

int export_png(const gm::Options& options, int width, int height,
               const std::vector<std::uint8_t>& rgba) {
    const auto png = gm::encode_png_rgba(width, height, rgba);
    if (png.empty() || !write_file(options.png, png)) {
        gm::logf(gm::LogLevel::Error, "png", "cannot write ", options.png.string());
        return 2;
    }
    std::printf("png: %s (%zu bytes)\n", options.png.string().c_str(), png.size());
    return 0;
}

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

    const std::string ext = path.extension().string();
    std::string error;

    if (gm::iequals_ascii(ext, ".sbm")) {
        const auto bytes = read_file(path, error);
        if (!bytes) {
            gm::logf(gm::LogLevel::Error, "dump", error);
            return 2;
        }
        const auto sbm = gm::parse_sbm(*bytes, error);
        if (!sbm) {
            gm::logf(gm::LogLevel::Error, "sbm", error);
            return 2;
        }
        std::printf("kind: SBM texture\n");
        std::printf("dimensions: %dx%d\n", sbm->width, sbm->height);
        std::printf("levels: %d\n", sbm->levels);
        std::printf("flags: %u\n", sbm->flags);
        for (int i = 0; i < sbm->levels; ++i) {
            std::printf("mip%d: %dx%d\n", i, sbm->mips[static_cast<std::size_t>(i)].width,
                        sbm->mips[static_cast<std::size_t>(i)].height);
        }
        if (!options.png.empty()) {
            const auto palette = gm::installed_palette(options.game, error);
            if (!palette) {
                gm::logf(gm::LogLevel::Error, "palette", error);
                return 2;
            }
            const auto& level = sbm->mips[0];
            const auto rgba = gm::sbm_to_rgba(level, *palette);
            return export_png(options, level.width, level.height, rgba);
        }
        return 0;
    }

    if (head.size() >= 2 && head[0] == 'B' && head[1] == 'M') {
        const auto bytes = read_file(path, error);
        if (!bytes) {
            gm::logf(gm::LogLevel::Error, "dump", error);
            return 2;
        }
        const auto bmp = gm::parse_bmp(*bytes, error);
        if (!bmp) {
            gm::logf(gm::LogLevel::Error, "bmp", error);
            return 2;
        }
        std::printf("kind: BMP image\n");
        std::printf("dimensions: %dx%d\n", bmp->width, bmp->height);
        std::printf("bpp: %d\n", bmp->bpp);
        std::printf("palette entries: %zu\n", bmp->palette.size() / 4);
        if (!options.png.empty()) {
            return export_png(options, bmp->width, bmp->height, bmp_to_rgba(*bmp));
        }
        return 0;
    }

    if (head.size() >= 7 && std::memcmp(head.data(), "BIGFILE", 7) == 0) {
        auto big = gm::BigFile::open(path, error);
        if (!big) {
            gm::logf(gm::LogLevel::Error, "dump", error);
            return 2;
        }
        std::printf("kind: BIGFILE index\n");
        std::printf("entries: %zu\n", big->entries().size());
        std::printf("data: %llu bytes\n",
                    static_cast<unsigned long long>(big->dat_size()));
        if (!big->entries().empty()) {
            const auto& first = big->entries().front();
            const auto& last = big->entries().back();
            std::printf("first: %s (%u bytes)\n", first.name.c_str(), first.size);
            std::printf("last: %s (%u bytes)\n", last.name.c_str(), last.size);
        }
        return 0;
    }

    if (!options.png.empty()) {
        gm::logf(gm::LogLevel::Error, "png", "unsupported input for --png: ", path.string());
        return 2;
    }
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
    if (!options->png.empty()) {
        gm::logf(gm::LogLevel::Error, "png", "--png needs --dump FILE");
        return 2;
    }
    return run(*options);
}
