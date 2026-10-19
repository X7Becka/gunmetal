#include "core/arena.h"
#include "core/bytes.h"
#include "core/config.h"
#include "core/log.h"
#include "core/math.h"
#include "core/path.h"
#include "core/time.h"
#include "platform/window.h"
#include "res/bigfile.h"
#include "res/bmp.h"
#include "res/png.h"
#include "res/sbm.h"
#include "core/hash.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        const auto va = (a);                                                                 \
        const auto vb = (b);                                                                 \
        if (!(va == vb)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);     \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

void test_path() {
    CHECK(gm::iequals_ascii("Gunmetal.HI", "gunmetal.hi"));
    CHECK(!gm::iequals_ascii("abc", "abcd"));

    std::random_device rd;
    const fs::path root =
        fs::temp_directory_path() / ("gm_test_" + std::to_string(rd()));
    fs::create_directories(root / "TestDir" / "SUB");
    {
        std::ofstream(root / "TestDir" / "SUB" / "File.TxT") << "hello";
    }

    const auto hit = gm::resolve_ci(root, "testdir/sub/file.txt");
    CHECK(hit.has_value());
    if (hit) {
        CHECK_EQ(hit->filename().string(), std::string("File.TxT"));
        std::ifstream in(*hit);
        std::string text;
        in >> text;
        CHECK_EQ(text, std::string("hello"));
    }
    CHECK(!gm::resolve_ci(root, "testdir/nope.txt").has_value());

    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_arena() {
    gm::Arena arena(256);
    void* a = arena.alloc(3, 16);
    void* b = arena.alloc(16, 16);
    CHECK(a != nullptr && b != nullptr);
    CHECK_EQ(reinterpret_cast<std::uintptr_t>(a) % 16, 0u);
    CHECK_EQ(reinterpret_cast<std::uintptr_t>(b) % 16, 0u);
    CHECK(arena.used() >= 3 + 16);
    CHECK(arena.remaining() < arena.capacity());
    CHECK(arena.alloc(1024) == nullptr);
    arena.reset();
    CHECK_EQ(arena.used(), 0u);

    auto* values = arena.make<std::uint32_t>(4);
    CHECK(values != nullptr);
    values[3] = 7;
    CHECK_EQ(values[3], 7u);
}

void test_math() {
    const gm::Vec3 a{1.0f, 0.0f, 0.0f};
    const gm::Vec3 b{0.0f, 1.0f, 0.0f};
    CHECK_EQ(gm::dot(a, b), 0.0f);
    CHECK(std::fabs(gm::dot(a, a) - 1.0f) < 1e-6f);
    const gm::Vec3 c = gm::cross(a, b);
    CHECK(std::fabs(c.z - 1.0f) < 1e-6f);
    CHECK(std::fabs(gm::length(gm::Vec3{3.0f, 4.0f, 0.0f}) - 5.0f) < 1e-6f);
    CHECK(std::fabs(gm::length(gm::normalized(gm::Vec3{0.0f, 5.0f, 0.0f})) - 1.0f) < 1e-6f);
    CHECK_EQ(gm::clampf(5.0f, 0.0f, 1.0f), 1.0f);
    CHECK_EQ(gm::lerp(0.0f, 10.0f, 0.5f), 5.0f);
}

void test_bytes() {
    const std::uint8_t data[] = {0x78, 0x56, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x90};
    CHECK_EQ(gm::rd16(data), 0x5678u);
    CHECK_EQ(gm::rd24(data), 0x345678u);
    CHECK_EQ(gm::rd32(data), 0x12345678u);
    CHECK_EQ(gm::rd64(data), 0x90abcdef12345678ull);
    CHECK_EQ(gm::format_hex(std::span<const std::uint8_t>(data, 3)), std::string("78 56 34"));
    CHECK_EQ(gm::format_ascii(std::span<const std::uint8_t>(data, 2)), std::string("xV"));
}

void test_config() {
    std::vector<std::string> args = {"gunmetal", "--game", "/tmp/game", "--level", "level1",
                                     "--frames", "7", "--headless", "--verbose"};
    std::vector<char*> argv;
    for (auto& s : args) {
        argv.push_back(s.data());
    }
    std::string error;
    const auto options = gm::parse_args(static_cast<int>(argv.size()), argv.data(), error);
    CHECK(options.has_value());
    if (options) {
        CHECK_EQ(options->game.string(), std::string("/tmp/game"));
        CHECK_EQ(options->level, std::string("level1"));
        CHECK_EQ(options->frames, 7);
        CHECK(options->headless);
        CHECK(options->verbose);
    }

    std::vector<std::string> bad = {"gunmetal", "--nope"};
    std::vector<char*> bad_argv;
    for (auto& s : bad) {
        bad_argv.push_back(s.data());
    }
    CHECK(!gm::parse_args(static_cast<int>(bad_argv.size()), bad_argv.data(), error).has_value());
    CHECK(!error.empty());

    std::vector<std::string> bad_frames = {"gunmetal", "--frames", "x"};
    std::vector<char*> bad_frames_argv;
    for (auto& s : bad_frames) {
        bad_frames_argv.push_back(s.data());
    }
    CHECK(!gm::parse_args(static_cast<int>(bad_frames_argv.size()), bad_frames_argv.data(),
                          error)
               .has_value());
}

void test_time_and_window() {
    gm::FrameClock clock;
    CHECK_EQ(clock.tick(), 0.0);
    const double first = clock.tick();
    CHECK(first >= 0.0);
    CHECK(clock.elapsed() >= 0.0);

    std::string error;
    auto window = gm::Window::open("test", 8, 8, true, error);
    CHECK(window.has_value());
    if (window) {
        CHECK(window->headless());
        CHECK_EQ(window->width(), 8);
        CHECK_EQ(window->height(), 8);
        window->fill_gradient(3);
        window->fill(1, 2, 3);
        window->present();
        CHECK(window->pump());
    }
}

void test_bigfile() {
    std::random_device rd;
    const fs::path root = fs::temp_directory_path() / ("gm_big_" + std::to_string(rd()));
    fs::create_directories(root);

    auto put_name = [](std::vector<std::uint8_t>& out, const char* name) {
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<std::uint8_t>(name[i] ? name[i] : '\0'));
            if (name[i] == 0) {
                while (++i < 8) {
                    out.push_back(0);
                }
            }
        }
    };
    auto put_u32 = [](std::vector<std::uint8_t>& out, std::uint32_t v) {
        out.push_back(v & 0xFF);
        out.push_back((v >> 8) & 0xFF);
        out.push_back((v >> 16) & 0xFF);
        out.push_back((v >> 24) & 0xFF);
    };

    std::vector<std::uint8_t> idx = {'B', 'I', 'G', 'F', 'I', 'L', 'E', 0x1A};
    put_u32(idx, 2);
    put_name(idx, "ALPHA");
    put_u32(idx, 0);
    put_name(idx, "BETA");
    put_u32(idx, 4);
    {
        std::ofstream out(root / "TEST.IDX", std::ios::binary);
        out.write(reinterpret_cast<const char*>(idx.data()),
                  static_cast<std::streamsize>(idx.size()));
    }
    {
        std::ofstream out(root / "TEST.DAT", std::ios::binary);
        out << "AAAA" << "BBBBBB";
    }

    std::string error;
    const auto big = gm::BigFile::open(root / "TEST.IDX", error);
    CHECK(big.has_value());
    if (big) {
        CHECK_EQ(big->entries().size(), std::size_t{2});
        CHECK_EQ(big->entries()[0].name, std::string("ALPHA"));
        CHECK_EQ(big->entries()[0].offset, 0u);
        CHECK_EQ(big->entries()[0].size, 4u);
        CHECK_EQ(big->entries()[1].size, 6u);
        const auto alpha = big->read(big->entries()[0], error);
        CHECK(alpha.has_value());
        if (alpha) {
            CHECK_EQ(std::string(alpha->begin(), alpha->end()), std::string("AAAA"));
        }
        const auto beta = big->read(big->entries()[1], error);
        CHECK(beta.has_value());
        if (beta) {
            CHECK_EQ(std::string(beta->begin(), beta->end()), std::string("BBBBBB"));
        }
    }
    CHECK(!gm::BigFile::open(root / "NOPE.IDX", error).has_value());

    // The real archive, when this machine has the installation.
    const fs::path sounds = "/mnt/d/games/Gunmetal 3dfx/sounds/SOU.IDX";
    std::error_code ec;
    if (fs::is_regular_file(sounds, ec)) {
        const auto real = gm::BigFile::open(sounds, error);
        CHECK(real.has_value());
        if (real) {
            CHECK_EQ(real->entries().size(), std::size_t{999});
        }
    }

    fs::remove_all(root, ec);
}

void test_sbm_png() {
    std::vector<std::uint8_t> blob(12 + 16 + 4, 0);
    blob[4] = 2; // 4x4
    blob[5] = 2;
    blob[6] = 1; // two mips
    for (int i = 0; i < 16; ++i) {
        blob[12 + i] = static_cast<std::uint8_t>(i * 16);
    }
    for (int i = 0; i < 4; ++i) {
        blob[28 + i] = static_cast<std::uint8_t>(i * 64);
    }

    std::string error;
    const auto sbm = gm::parse_sbm(blob, error);
    CHECK(sbm.has_value());
    if (sbm) {
        CHECK_EQ(sbm->width, 4);
        CHECK_EQ(sbm->height, 4);
        CHECK_EQ(sbm->levels, 2);
        CHECK_EQ(sbm->mips.size(), std::size_t{2});
        CHECK_EQ(sbm->mips[1].width, 2);
        CHECK_EQ(sbm->mips[1].pixels.size(), std::size_t{4});
        std::vector<std::uint8_t> palette(256 * 4, 0);
        for (int i = 0; i < 256; ++i) {
            palette[i * 4 + 0] = static_cast<std::uint8_t>(i);
            palette[i * 4 + 1] = static_cast<std::uint8_t>(i);
            palette[i * 4 + 2] = static_cast<std::uint8_t>(i);
        }
        const auto rgba = gm::sbm_to_rgba(sbm->mips[0], palette);
        CHECK_EQ(rgba.size(), std::size_t{64});
        // pixel 0: index 0 -> black; pixel 1: index 16 -> 16; alpha 255.
        CHECK_EQ(rgba[3], std::uint8_t{255});
        CHECK_EQ(rgba[4], std::uint8_t{16});
        CHECK_EQ(rgba[7], std::uint8_t{255});
    }

    // Truncated chain must be rejected.
    std::vector<std::uint8_t> truncated(blob.begin(), blob.begin() + 20);
    CHECK(!gm::parse_sbm(truncated, error).has_value());

    const auto png = gm::encode_png_rgba(2, 2, std::vector<std::uint8_t>(16, 128));
    CHECK_EQ(png.size() > 40, true);
    const std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    CHECK(std::memcmp(png.data(), signature, 8) == 0);
    const auto be32 = [](const std::uint8_t* p) {
        return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
    };
    CHECK_EQ(be32(png.data() + 8), 13u);      // IHDR length
    CHECK(std::memcmp(png.data() + 12, "IHDR", 4) == 0);
    CHECK_EQ(be32(png.data() + 16), 2u);      // width
    CHECK_EQ(be32(png.data() + 20), 2u);      // height

    // Real data, when installed: Python-computed golden hashes.
    const fs::path pics = "/mnt/d/games/Gunmetal 3dfx/PICS/SBM.IDX";
    std::error_code ec;
    if (fs::is_regular_file(pics, ec)) {
        const auto big = gm::BigFile::open(pics, error);
        CHECK(big.has_value());
        if (big && !big->entries().empty()) {
            const auto bytes = big->read(big->entries()[0], error);
            CHECK(bytes.has_value());
            if (bytes) {
                const auto real = gm::parse_sbm(*bytes, error);
                CHECK(real.has_value());
                if (real) {
                    CHECK_EQ(real->width, 256);
                    CHECK_EQ(real->levels, 9);
                    CHECK_EQ(gm::fnv1a64(real->mips[0].pixels), 0xeca77c01c92266c7ull);
                }
            }
        }
    }
    const fs::path sprites = "/mnt/d/games/Gunmetal 3dfx/sprites/SBM.IDX";
    if (fs::is_regular_file(sprites, ec)) {
        const auto big = gm::BigFile::open(sprites, error);
        if (big && !big->entries().empty()) {
            const auto bytes = big->read(big->entries()[0], error);
            if (bytes) {
                const auto real = gm::parse_sbm(*bytes, error);
                CHECK(real.has_value());
                if (real) {
                    CHECK_EQ(real->width, 64);
                    CHECK_EQ(gm::fnv1a64(real->mips[0].pixels), 0x4f3ae259926f703aull);
                }
            }
        }
    }

    const fs::path flag = "/mnt/d/games/Gunmetal 3dfx/FLAG/naUSA.BMP";
    if (fs::is_regular_file(flag, ec)) {
        std::ifstream in(flag, std::ios::binary);
        std::vector<std::uint8_t> bmp_bytes((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>());
        const auto bmp = gm::parse_bmp(bmp_bytes, error);
        CHECK(bmp.has_value());
        if (bmp) {
            CHECK_EQ(bmp->palette.size(), std::size_t{1024});
            CHECK_EQ(bmp->bpp, 8);
        }
    }
}

void test_log() {
    gm::set_log_level(gm::LogLevel::Off);
    CHECK(!gm::log_enabled(gm::LogLevel::Error));
    gm::set_log_level(gm::LogLevel::Warn);
    CHECK(gm::log_enabled(gm::LogLevel::Error));
    CHECK(!gm::log_enabled(gm::LogLevel::Info));
    gm::logf(gm::LogLevel::Info, "test", "must not print ", 1);
    gm::set_log_level(gm::LogLevel::Info);
    CHECK_EQ(std::string(gm::log_level_name(gm::LogLevel::Warn)), std::string("warn"));
}

} // namespace

int main() {
    test_path();
    test_arena();
    test_math();
    test_bytes();
    test_config();
    test_time_and_window();
    test_bigfile();
    test_log();
    test_sbm_png();

    if (g_failures != 0) {
        std::fprintf(stderr, "gm_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("gm_tests: all checks passed\n");
    return 0;
}
