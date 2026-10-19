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
#include "res/sbw.h"
#include "res/sou.h"
#include "res/sbt.h"
#include "res/sbc.h"
#include "res/sob.h"
#include "res/text.h"
#include "res/level.h"
#include "res/sbv.h"
#include "res/cache.h"
#include "render/render.h"
#include "res/anim.h"
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

void test_sbw() {
    std::vector<std::uint8_t> blob;
    auto u32 = [&](std::uint32_t v) {
        blob.push_back(v & 0xFF);
        blob.push_back((v >> 8) & 0xFF);
        blob.push_back((v >> 16) & 0xFF);
        blob.push_back((v >> 24) & 0xFF);
    };
    u32(1);
    blob.insert(blob.end(), {'T', 'E', 'X', 0});
    blob.push_back(1);
    blob.push_back(0);
    u32(7);
    u32(1);
    blob.insert(blob.end(), {'F', 'X', 0});
    u32(2);
    u32(3);
    u32(2);
    blob.push_back(10);
    blob.push_back(0);
    blob.push_back(11);
    blob.push_back(0);
    u32(1);
    u32(1);
    blob.push_back(6);
    blob.push_back(0);
    blob.push_back(0);
    u32(42);
    blob.insert(blob.end(), 96, 0xAA);

    std::string error;
    const auto header = gm::parse_sbw_header(blob, error);
    CHECK(header.has_value());
    if (header) {
        CHECK_EQ(header->textures.size(), std::size_t{1});
        CHECK_EQ(header->textures[0].name, std::string("TEX"));
        CHECK_EQ(header->textures[0].value, 7u);
        CHECK_EQ(header->effects.size(), std::size_t{1});
        CHECK_EQ(header->effects[0].ids.size(), std::size_t{2});
        CHECK_EQ(header->effects[0].ids[1], std::uint16_t{11});
        CHECK_EQ(header->sectors.size(), std::size_t{1});
        CHECK_EQ(header->sectors[0].kind, std::uint16_t{6});
        CHECK_EQ(header->object_count, std::size_t{1});
        CHECK_EQ(header->sectors[0].objects[0].id, 42u);
        CHECK_EQ(header->header_end, blob.size());
    }
    std::vector<std::uint8_t> cut(blob.begin(), blob.begin() + blob.size() - 50);
    CHECK(!gm::parse_sbw_header(cut, error).has_value());

    // Full parse with one object (3 vertices, 1 triangle, 112-byte poly tail).
    std::vector<std::uint8_t> full = blob;
    full.push_back(3);
    full.push_back(0);
    full.push_back(1);
    full.push_back(0);
    for (int v = 0; v < 3; ++v) {
        for (int c = 0; c < 3; ++c) {
            const float f = static_cast<float>(v * 3 + c);
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&f);
            full.insert(full.end(), bytes, bytes + 4);
        }
    }
    full.insert(full.end(), 50, 0);
    full.push_back(0); // texture
    full.push_back(0);
    full.push_back(0); // lightmap
    full.push_back(0);
    full.push_back(0); // material
    full.push_back(0);
    full.push_back(0); // overlay
    full.push_back(0);
    full.push_back(0); // f7a
    full.push_back(0);
    full.push_back(0); // brightness
    full.push_back(3); // n_idx
    full.push_back(0); full.push_back(0);
    full.push_back(1); full.push_back(0);
    full.push_back(2); full.push_back(0);
    full.insert(full.end(), 112, 0);

    const auto level = gm::parse_sbw(full, error);
    CHECK(level.has_value());
    if (level) {
        CHECK_EQ(level->total_vertices, std::size_t{3});
        CHECK_EQ(level->total_polygons, std::size_t{1});
        CHECK_EQ(level->geometry.size(), std::size_t{1});
        CHECK_EQ(level->geometry[0].vertices.size(), std::size_t{3});
        CHECK_EQ(level->geometry[0].polys[0].indices.size(), std::size_t{3});
        CHECK_EQ(level->geometry[0].polys[0].indices[2], std::uint16_t{2});
        CHECK_EQ(level->file_end, full.size());
    }

    // Real levels: Python-computed golden values.
    const fs::path dir = "/mnt/d/games/Gunmetal 3dfx";
    std::error_code ec;
    struct Golden {
        const char* name;
        std::size_t tex, eff, sec, obj, end;
        std::uint64_t fnv;
        std::size_t verts, polys;
        std::uint64_t vfnv, idxsum;
    };
    const Golden golden[] = {
        {"level0.sbw", 381, 37, 58, 192, 26195, 0x5cd9bb6033c473c1ull,
         2173, 1449, 0xc54cc67faaff0eecull, 0x4892a617780b38e4ull},
        {"level7.sbw", 512, 40, 122, 835, 92847, 0x92338789ca88bb60ull,
         9553, 6580, 0xa1b11d8b1108f046ull, 0xf8b8faeba6410f74ull},
        {"secrt1.sbw", 444, 36, 116, 432, 51496, 0x821ee6466cb5cbc6ull,
         5252, 3622, 0x7c0e8f8945740486ull, 0x21a783fccf4c6111ull},
    };
    for (const auto& g : golden) {
        const fs::path path = dir / g.name;
        if (!fs::is_regular_file(path, ec)) {
            continue;
        }
        std::ifstream in(path, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
        const auto h = gm::parse_sbw(bytes, error);
        CHECK(h.has_value());
        if (!h) {
            continue;
        }
        CHECK_EQ(h->header.textures.size(), g.tex);
        CHECK_EQ(h->header.effects.size(), g.eff);
        CHECK_EQ(h->header.sectors.size(), g.sec);
        CHECK_EQ(h->header.object_count, g.obj);
        CHECK_EQ(h->header.header_end, g.end);
        CHECK_EQ(gm::fnv1a64(std::span<const std::uint8_t>(bytes.data(), g.end)), g.fnv);
        CHECK_EQ(h->file_end, bytes.size());
        CHECK_EQ(h->total_vertices, g.verts);
        CHECK_EQ(h->total_polygons, g.polys);
        std::vector<std::uint8_t> vertex_bytes;
        vertex_bytes.reserve(h->total_vertices * 12);
        std::uint64_t idx_sum = 0;
        for (const auto& geom : h->geometry) {
            for (const auto& v : geom.vertices) {
                const float xyz[3] = {v.x, v.y, v.z};
                const auto* p = reinterpret_cast<const std::uint8_t*>(xyz);
                vertex_bytes.insert(vertex_bytes.end(), p, p + 12);
            }
            for (const auto& poly : geom.polys) {
                idx_sum = idx_sum * 131 + poly.indices.size();
            }
        }
        CHECK_EQ(gm::fnv1a64(vertex_bytes), g.vfnv);
        CHECK_EQ(idx_sum, g.idxsum);
    }

    // Every shipped level must parse.
    std::size_t levels = 0;
    if (fs::is_directory(dir, ec)) {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file() || !gm::iequals_ascii(entry.path().extension().string(), ".sbw")) {
                continue;
            }
            std::ifstream in(entry.path(), std::ios::binary);
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>());
            const auto h = gm::parse_sbw(bytes, error);
            CHECK(h.has_value() && h->header.header_end < bytes.size() &&
                  h->file_end == bytes.size());
            ++levels;
        }
    }
    CHECK(levels == 0 || levels == 36);
}

void test_sou() {
    std::vector<std::uint8_t> blob = {'S', 'O', 'U', '1'};
    auto u32 = [&](std::uint32_t v) {
        blob.push_back(v & 0xFF);
        blob.push_back((v >> 8) & 0xFF);
        blob.push_back((v >> 16) & 0xFF);
        blob.push_back((v >> 24) & 0xFF);
    };
    u32(8000);
    u32(0);
    u32(6);
    for (int i = 0; i < 6; ++i) {
        blob.push_back(static_cast<std::uint8_t>(i));
    }
    std::string error;
    const auto sou = gm::parse_sou(blob, error);
    CHECK(sou.has_value());
    if (sou) {
        CHECK_EQ(sou->magic, std::string("SOU1"));
        CHECK_EQ(sou->rate, 8000u);
        CHECK_EQ(sou->flags, 0u);
        CHECK_EQ(sou->payload.size(), std::size_t{6});
    }
    blob.resize(18);
    CHECK(!gm::parse_sou(blob, error).has_value());

    // Real archive: SILENCE is a 400-byte SOU1 at 8 kHz.
    const fs::path idx = "/mnt/d/games/Gunmetal 3dfx/sounds/SOU.IDX";
    std::error_code ec;
    if (fs::is_regular_file(idx, ec)) {
        const auto big = gm::BigFile::open(idx, error);
        CHECK(big.has_value());
        if (big) {
            const gm::BigEntry* silence = nullptr;
            for (const auto& e : big->entries()) {
                if (e.name == "SILENCE") {
                    silence = &e;
                    break;
                }
            }
            CHECK(silence != nullptr);
            if (silence) {
                const auto bytes = big->read(*silence, error);
                CHECK(bytes.has_value());
                if (bytes) {
                    const auto real = gm::parse_sou(*bytes, error);
                    CHECK(real.has_value());
                    if (real) {
                        CHECK_EQ(real->magic, std::string("SOU1"));
                        CHECK_EQ(real->rate, 8000u);
                        CHECK_EQ(real->payload.size(), std::size_t{400});
                        CHECK_EQ(real->payload[0], std::uint8_t{0x4b});
                        CHECK_EQ(real->payload[1], std::uint8_t{0x0c});
                        CHECK_EQ(real->payload[2], std::uint8_t{0x20});
                    }
                }
            }
        }
    }
}

void test_sbt() {
    std::vector<std::uint8_t> blob(84, 0);
    std::memcpy(blob.data(), "BINARY SAVE", 11);
    auto u32 = [&](std::uint32_t v) {
        blob.push_back(v & 0xFF);
        blob.push_back((v >> 8) & 0xFF);
        blob.push_back((v >> 16) & 0xFF);
        blob.push_back((v >> 24) & 0xFF);
    };
    // SOB_TABLE with one entry
    blob.push_back(20);
    u32(1);
    blob.insert(blob.end(), {'T', 'E', 'S', 'T', 0, 0, 0, 0, 0, 0, 0, 0});
    u32(5);
    u32(0x1234);
    // POLY_HANDLE with one entry (4 columns)
    blob.push_back(15);
    u32(1);
    u32(1); u32(2); u32(3); u32(4);
    // SCRIPT: 0 things, 1 handle, 2 code bytes
    blob.push_back(11);
    u32(0);
    u32(1);
    for (int i = 0; i < 12; ++i) {
        blob.push_back(static_cast<std::uint8_t>(i));
    }
    u32(2);
    blob.push_back(0xAA);
    blob.push_back(0xBB);
    blob.push_back(0);
    blob.push_back(255);

    std::string error;
    const auto level = gm::parse_sbt(blob, error);
    CHECK(level.has_value());
    if (level) {
        CHECK_EQ(level->version, 0u);
        CHECK_EQ(level->records.size(), std::size_t{4});
        CHECK_EQ(level->records[0].type, std::uint8_t{20});
        CHECK_EQ(level->sobs.size(), std::size_t{1});
        CHECK_EQ(level->sobs[0].name, std::string("TEST"));
        CHECK_EQ(level->sobs[0].amount, 5u);
        CHECK_EQ(level->poly_handles.size(), std::size_t{1});
        CHECK_EQ(level->poly_handles[0].tag, 1u);
        CHECK_EQ(level->poly_handles[0].a, 2u);
        CHECK_EQ(level->poly_handles[0].b, 3u);
        CHECK_EQ(level->poly_handles[0].c, 4u);
        CHECK_EQ(level->script.handles, 1u);
        CHECK_EQ(level->script.code_size, 2u);
        CHECK_EQ(level->file_end, blob.size());
    }
    std::vector<std::uint8_t> cut(blob.begin(), blob.end() - 3);
    CHECK(!gm::parse_sbt(cut, error).has_value());

    // Real levels: Python-computed goldens.
    const fs::path dir = "/mnt/d/games/Gunmetal 3dfx";
    std::error_code ec;
    struct Golden {
        const char* name;
        std::vector<std::uint8_t> types;
        std::size_t poly, object, sector, sobs, things, handles, code;
    };
    const Golden golden[] = {
        {"level0.sbt", {12, 20, 15, 17, 5, 4, 11, 255}, 104, 0, 26, 67, 0, 350, 62497},
        {"level7.sbt", {12, 20, 15, 16, 17, 5, 4, 11, 255}, 187, 1, 39, 109, 0, 425, 99859},
        {"secrt1.sbt", {12, 20, 15, 16, 17, 5, 4, 11, 255}, 11, 7, 14, 104, 0, 392, 86089},
    };
    for (const auto& g : golden) {
        const fs::path path = dir / g.name;
        if (!fs::is_regular_file(path, ec)) {
            continue;
        }
        std::ifstream in(path, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
        const auto h = gm::parse_sbt(bytes, error);
        CHECK(h.has_value());
        if (!h) {
            continue;
        }
        CHECK_EQ(h->records.size(), g.types.size());
        for (std::size_t i = 0; i < g.types.size() && i < h->records.size(); ++i) {
            CHECK_EQ(h->records[i].type, g.types[i]);
        }
        CHECK_EQ(h->poly_handles.size(), g.poly);
        CHECK_EQ(h->object_handles.size(), g.object);
        CHECK_EQ(h->sector_handles.size(), g.sector);
        CHECK_EQ(h->sobs.size(), g.sobs);
        CHECK_EQ(h->script.things, g.things);
        CHECK_EQ(h->script.handles, g.handles);
        CHECK_EQ(h->script.code_size, g.code);
        CHECK_EQ(h->file_end, bytes.size());
    }

    std::size_t levels = 0;
    if (fs::is_directory(dir, ec)) {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file() ||
                !gm::iequals_ascii(entry.path().extension().string(), ".sbt")) {
                continue;
            }
            std::ifstream in(entry.path(), std::ios::binary);
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>());
            const auto h = gm::parse_sbt(bytes, error);
            CHECK(h.has_value() && h->file_end == bytes.size() &&
                  !h->records.empty() && h->records.back().type == 255);
            ++levels;
        }
    }
    CHECK(levels == 0 || levels == 36);
}

void test_sbc() {
    std::vector<std::uint8_t> blob = {'S', 'B', 'C', '1'};
    auto u32 = [&](std::uint32_t v) {
        blob.push_back(v & 0xFF);
        blob.push_back((v >> 8) & 0xFF);
        blob.push_back((v >> 16) & 0xFF);
        blob.push_back((v >> 24) & 0xFF);
    };
    u32(2);
    u32(0x10001); // 2x2 (w_log=1, h_log=1)
    for (int i = 0; i < 4; ++i) {
        blob.push_back(static_cast<std::uint8_t>(i * 10));
    }
    u32(0); // 1x1
    blob.push_back(77);

    std::string error;
    const auto sbc = gm::parse_sbc(blob, error);
    CHECK(sbc.has_value());
    if (sbc) {
        CHECK_EQ(sbc->magic, std::string("SBC1"));
        CHECK_EQ(sbc->records.size(), std::size_t{2});
        CHECK_EQ(sbc->records[0].width, 2);
        CHECK_EQ(sbc->records[0].height, 2);
        CHECK_EQ(sbc->records[0].pixels[3], std::uint8_t{30});
        CHECK_EQ(sbc->records[1].width, 1);
        CHECK_EQ(sbc->records[1].height, 1);
        CHECK_EQ(sbc->records[1].pixels[0], std::uint8_t{77});
        CHECK_EQ(sbc->file_end, blob.size());
    }
    std::vector<std::uint8_t> cut(blob.begin(), blob.end() - 1);
    CHECK(!gm::parse_sbc(cut, error).has_value());

    // Real: level0 golden + every shipped .sbc must parse to EOF.
    const fs::path dir = "/mnt/d/games/Gunmetal 3dfx";
    std::error_code ec;
    const fs::path level0 = dir / "level0.sbc";
    if (fs::is_regular_file(level0, ec)) {
        std::ifstream in(level0, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
        const auto s = gm::parse_sbc(bytes, error);
        CHECK(s.has_value());
        if (s) {
            CHECK_EQ(s->magic, std::string("SBC1"));
            CHECK_EQ(s->records.size(), std::size_t{405});
            CHECK_EQ(s->records[0].width, 16);
            CHECK_EQ(s->records[0].height, 8);
            CHECK_EQ(gm::fnv1a64(s->records[0].pixels), 0x6c79973b66efff25ull);
            CHECK_EQ(s->file_end, bytes.size());
        }
    }

    std::size_t files = 0;
    auto scan = [&](const fs::path& root) {
        if (!fs::is_directory(root, ec)) {
            return;
        }
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (!entry.is_regular_file() ||
                !gm::iequals_ascii(entry.path().extension().string(), ".sbc")) {
                continue;
            }
            std::ifstream in(entry.path(), std::ios::binary);
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>());
            const auto s = gm::parse_sbc(bytes, error);
            CHECK(s.has_value() && s->file_end == bytes.size());
            ++files;
        }
    };
    scan(dir);
    scan(dir / "SOBS");
    CHECK(files == 0 || files == 51);
}

void test_anim() {
    std::string text = "0 3\r\nBigGate1 200\r\nBigGate2 6000\r\nBigGate3 10\r\n";
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    std::string error;
    const auto soo = gm::parse_soo(bytes, error);
    CHECK(soo.has_value());
    if (soo) {
        CHECK_EQ(soo->mode, 0);
        CHECK_EQ(soo->declared, 3);
        CHECK_EQ(soo->frames.size(), std::size_t{3});
        CHECK_EQ(soo->frames[1].name, std::string("BigGate2"));
        CHECK_EQ(soo->frames[1].duration, 6000);
    }
    std::vector<std::uint8_t> bad = {'0', ' ', '3', ' ', 'A'};
    CHECK(!gm::parse_soo(bad, error).has_value());

    std::string soa_text = "1 72 2 1\r\nBaby1\r\nBaby2\r\n";
    std::vector<std::uint8_t> soa_bytes(soa_text.begin(), soa_text.end());
    const auto soa = gm::parse_soa(soa_bytes, error);
    CHECK(soa.has_value());
    if (soa) {
        CHECK_EQ(soa->mode, 1);
        CHECK_EQ(soa->period, 72);
        CHECK_EQ(soa->loop, 1);
        CHECK_EQ(soa->names.size(), std::size_t{2});
        CHECK_EQ(soa->names[1], std::string("Baby2"));
    }

    const fs::path dir = "/mnt/d/games/Gunmetal 3dfx";
    std::error_code ec;
    const fs::path biggate = dir / "Sobanim" / "BigGate.soo";
    if (fs::is_regular_file(biggate, ec)) {
        std::ifstream in(biggate, std::ios::binary);
        std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
        const auto s = gm::parse_soo(raw, error);
        CHECK(s.has_value());
        if (s) {
            CHECK_EQ(s->frames.size(), std::size_t{3});
            CHECK_EQ(s->frames.back().duration, 10);
        }
    }
    const fs::path baby = dir / "anims" / "Baby.SOA";
    if (fs::is_regular_file(baby, ec)) {
        std::ifstream in(baby, std::ios::binary);
        std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
        const auto a = gm::parse_soa(raw, error);
        CHECK(a.has_value());
        if (a) {
            CHECK_EQ(a->mode, 1);
            CHECK_EQ(a->period, 72);
            CHECK_EQ(a->declared, 2);
            CHECK_EQ(a->names.size(), std::size_t{2});
        }
    }

    std::size_t soo_count = 0;
    const fs::path soo_dir = dir / "Sobanim";
    if (fs::is_directory(soo_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(soo_dir, ec)) {
            if (!entry.is_regular_file() ||
                !gm::iequals_ascii(entry.path().extension().string(), ".soo")) {
                continue;
            }
            std::ifstream in(entry.path(), std::ios::binary);
            std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
            const auto s = gm::parse_soo(raw, error);
            CHECK(s.has_value() && !s->frames.empty());
            ++soo_count;
        }
    }
    CHECK(soo_count == 0 || soo_count == 40);

    std::size_t soa_count = 0;
    const fs::path soa_dir = dir / "anims";
    if (fs::is_directory(soa_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(soa_dir, ec)) {
            if (!entry.is_regular_file() ||
                !gm::iequals_ascii(entry.path().extension().string(), ".soa")) {
                continue;
            }
            std::ifstream in(entry.path(), std::ios::binary);
            std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
            const auto a = gm::parse_soa(raw, error);
            CHECK(a.has_value() && !a->names.empty());
            ++soa_count;
        }
    }
    CHECK(soa_count == 0 || soa_count == 77);
}

void test_sob() {
    auto fnv_sob = [](const gm::Sob& sob) {
        std::vector<std::uint8_t> raw;
        auto put = [&](const void* p, std::size_t n) {
            const auto* b = static_cast<const std::uint8_t*>(p);
            raw.insert(raw.end(), b, b + n);
        };
        for (const auto& v : sob.vertices) {
            const float xyz[3] = {v.x, v.y, v.z};
            put(xyz, 12);
        }
        for (const auto& f : sob.faces) {
            const std::int32_t hdr[2] = {f.flags, static_cast<std::int32_t>(f.indices.size())};
            put(hdr, 8);
            for (int index : f.indices) {
                put(&index, 4);
            }
            for (const auto* vec : {&f.origin, &f.u, &f.v, &f.normal, &f.spare}) {
                const float xyz[3] = {vec->x, vec->y, vec->z};
                put(xyz, 12);
            }
        }
        return gm::fnv1a64(raw);
    };

    std::string text = "3 1 0 0 0 1 0 0 0 1 0 "
                       "TEX none none none none none none none 0 3 0 1 2 "
                       "0 0 0 0 0 0 0 0 0 0 1 0 0 0 0";
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    std::string error;
    const auto sob = gm::parse_sob(bytes, error);
    CHECK(sob.has_value());
    if (sob) {
        CHECK_EQ(sob->vertices.size(), std::size_t{3});
        CHECK_EQ(sob->faces.size(), std::size_t{1});
        CHECK_EQ(sob->faces[0].texture(), std::string("TEX"));
        CHECK_EQ(sob->faces[0].indices.size(), std::size_t{3});
        CHECK_EQ(sob->faces[0].indices[2], 2);
        CHECK(std::fabs(sob->faces[0].normal.y - 1.0f) < 1e-6f);
    }

    // The optional lone -1 sentinel must also parse.
    std::string with_sentinel = text + " -1";
    std::vector<std::uint8_t> sentinel_bytes(with_sentinel.begin(), with_sentinel.end());
    const auto sob2 = gm::parse_sob(sentinel_bytes, error);
    CHECK(sob2.has_value());
    if (sob2) {
        CHECK_EQ(sob2->faces.size(), std::size_t{1});
    }

    std::vector<std::uint8_t> broken = {'3', ' ', '1', ' ', 'x'};
    CHECK(!gm::parse_sob(broken, error).has_value());

    const fs::path dir = "/mnt/d/games/Gunmetal 3dfx/SOBS";
    std::error_code ec;
    const fs::path clip = dir / "20mmclp.sob";
    if (fs::is_regular_file(clip, ec)) {
        std::ifstream in(clip, std::ios::binary);
        std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
        const auto s = gm::parse_sob(raw, error);
        CHECK(s.has_value());
        if (s) {
            CHECK_EQ(s->vertices.size(), std::size_t{8});
            CHECK_EQ(s->faces.size(), std::size_t{6});
            CHECK_EQ(fnv_sob(*s), 0x8ca3f5ae6f81d07eull);
        }
    }
    const fs::path moclass = dir / "MOCLASS.SOB";
    if (fs::is_regular_file(moclass, ec)) {
        std::ifstream in(moclass, std::ios::binary);
        std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
        const auto s = gm::parse_sob(raw, error);
        CHECK(s.has_value());
        if (s) {
            CHECK_EQ(s->vertices.size(), std::size_t{957});
            CHECK_EQ(s->faces.size(), std::size_t{1130});
            CHECK_EQ(fnv_sob(*s), 0x43876fd038d99bb0ull);
        }
    }

    std::size_t models = 0;
    if (fs::is_directory(dir, ec)) {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file() ||
                !gm::iequals_ascii(entry.path().extension().string(), ".sob")) {
                continue;
            }
            std::ifstream in(entry.path(), std::ios::binary);
            std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
            const auto s = gm::parse_sob(raw, error);
            CHECK(s.has_value() && !s->faces.empty());
            ++models;
        }
    }
    CHECK(models == 0 || models == 672);
}

void test_text() {
    std::string sog_text = "SND1\r\nSND2\r\n\r\nSND3\r\n";
    std::vector<std::uint8_t> sog_bytes(sog_text.begin(), sog_text.end());
    std::string error;
    const auto sog = gm::parse_sog(sog_bytes, error);
    CHECK(sog.has_value());
    if (sog) {
        CHECK_EQ(sog->names.size(), std::size_t{3});
        CHECK_EQ(sog->names[2], std::string("SND3"));
    }

    // The .sme cipher is a positional Caesar (i % 10).
    const std::string abc = "ABC";
    const auto abc_dec = gm::sml_decode(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()));
    CHECK_EQ(std::string(abc_dec.begin(), abc_dec.end()), std::string("AAA"));
    const std::string hello = "Hello";
    const auto enc = gm::sml_encode(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(hello.data()), hello.size()));
    const auto dec = gm::sml_decode(enc);
    CHECK_EQ(std::string(dec.begin(), dec.end()), hello);

    const std::string markup = "<d><r><f s=\"2\">dmills</f><s><p>Morning, <k>.</r>";
    const auto tags = gm::sml_tags(markup);
    CHECK_EQ(tags.size(), std::size_t{8});
    if (!tags.empty()) {
        CHECK_EQ(tags.front().name, std::string("d"));
        CHECK_EQ(tags[2].name, std::string("f"));
        CHECK(tags[2].raw.find("s=\"2\"") != std::string::npos);
    }
    const auto plain = gm::sml_plain(markup);
    CHECK(plain.find("Morning, Soldier.") != std::string::npos);

    // Real pair: decode(LEVEL1.SME) == level1.sml.
    const fs::path sml_dir = "/mnt/d/games/Gunmetal 3dfx/SML/brief";
    std::error_code ec;
    const fs::path sme = sml_dir / "LEVEL1.SME";
    const fs::path sml = sml_dir / "level1.sml";
    if (fs::is_regular_file(sme, ec) && fs::is_regular_file(sml, ec)) {
        std::ifstream a(sme, std::ios::binary);
        std::ifstream b(sml, std::ios::binary);
        std::vector<std::uint8_t> enc_bytes((std::istreambuf_iterator<char>(a)),
                                            std::istreambuf_iterator<char>());
        std::vector<std::uint8_t> plain_bytes((std::istreambuf_iterator<char>(b)),
                                              std::istreambuf_iterator<char>());
        CHECK(gm::sml_decode(enc_bytes) == plain_bytes);
    }

    // Corpus counts.
    std::size_t sogs = 0;
    const fs::path music = "/mnt/d/games/Gunmetal 3dfx/MUSIC";
    if (fs::is_directory(music, ec)) {
        for (const auto& entry : fs::directory_iterator(music, ec)) {
            if (entry.is_regular_file() &&
                gm::iequals_ascii(entry.path().extension().string(), ".sog")) {
                ++sogs;
            }
        }
    }
    CHECK(sogs == 0 || sogs == 651);

    std::size_t smes = 0;
    std::size_t smls = 0;
    const fs::path ui = "/mnt/d/games/Gunmetal 3dfx/SML";
    if (fs::is_directory(ui, ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(ui, ec)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const auto ext = entry.path().extension().string();
            if (gm::iequals_ascii(ext, ".sme")) {
                ++smes;
                std::ifstream in(entry.path(), std::ios::binary);
                std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                              std::istreambuf_iterator<char>());
                const auto dec_bytes = gm::sml_decode(raw);
                const std::string_view view(reinterpret_cast<const char*>(dec_bytes.data()),
                                            dec_bytes.size());
                CHECK(!gm::sml_tags(view).empty());
            } else if (gm::iequals_ascii(ext, ".sml")) {
                ++smls;
            }
        }
    }
    CHECK(smes == 0 || smes == 151);
    CHECK(smls == 0 || smls == 149);

    const fs::path soe = "/mnt/d/games/Gunmetal 3dfx/SOBS/railrock.soe";
    if (fs::is_regular_file(soe, ec)) {
        std::ifstream in(soe, std::ios::binary);
        std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
        const auto t = gm::parse_soe_text(raw, error);
        CHECK(t.has_value());
        if (t) {
            CHECK(t->tokens.size() > 20);
        }
    }
}

void test_level() {
    const fs::path dir = "/mnt/d/games/Gunmetal 3dfx";
    std::error_code ec;
    std::string error;
    if (!fs::is_regular_file(dir / "level0.sbw", ec)) {
        return;
    }
    const auto level = gm::load_level(dir, "level0", error);
    CHECK(level.has_value());
    if (!level) {
        return;
    }
    CHECK_EQ(level->name, std::string("level0"));
    CHECK(level->world.has_value());
    CHECK(level->entities.has_value());
    CHECK(level->lightmaps.has_value());
    if (level->world) {
        CHECK_EQ(level->world->total_vertices, std::size_t{2173});
        CHECK_EQ(level->world->total_polygons, std::size_t{1449});
    }
    if (level->entities) {
        CHECK_EQ(level->entities->poly_handles.size(), std::size_t{104});
        CHECK_EQ(level->entities->sobs.size(), std::size_t{67});
    }
    if (level->lightmaps) {
        CHECK_EQ(level->lightmaps->records.size(), std::size_t{405});
    }
    CHECK(!gm::load_level(dir, "no_such_level", error).has_value());
}

void test_sbv() {
    // Synthetic clip: 2x1, two one-byte frames.
    std::vector<std::uint8_t> clip = {'S', 'B', 'V', '0', 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0};
    const std::uint32_t table[4] = {0, 1, 67, 2};
    for (std::uint32_t v : table) {
        clip.push_back(static_cast<std::uint8_t>(v & 0xff));
        clip.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
        clip.push_back(0);
        clip.push_back(0);
    }
    clip.push_back(0xaa);
    clip.push_back(0xbb);

    std::string error;
    const auto parsed = gm::parse_sbv(clip, error);
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK_EQ(parsed->width, 2u);
        CHECK_EQ(parsed->height, 1u);
        CHECK_EQ(parsed->frame_count, 2u);
        CHECK_EQ(parsed->data_start, std::size_t{32});
        CHECK_EQ(parsed->table[1].timestamp_ms, 67u);
        CHECK_EQ(parsed->table[1].end_offset, 2u);
        CHECK(gm::sbv_offsets_valid(*parsed));
    }
    std::vector<std::uint8_t> bad = {'N', 'O', 'P', 'E'};
    CHECK(!gm::parse_sbv(bad, error).has_value());

    struct Golden {
        const char* name;
        std::uint32_t frames;
        std::size_t data_start;
        std::uint32_t duration;
        std::size_t file_size;
    };
    const Golden goldens[] = {
        {"Intro.sbv", 500, 4016, 33333, 2811092},
        {"orient.sbv", 4523, 36200, 301533, 35005220},
        {"winner.sbv", 1820, 14576, 121333, 15343896},
    };
    for (const auto& golden : goldens) {
        const fs::path path = fs::path("/mnt/d/games/Gunmetal 3dfx/Vid") / golden.name;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) {
            continue;
        }
        std::ifstream in(path, std::ios::binary);
        std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
        const auto s = gm::parse_sbv(raw, error);
        CHECK(s.has_value());
        if (!s) {
            continue;
        }
        CHECK_EQ(s->width, 320u);
        CHECK_EQ(s->height, 160u);
        CHECK_EQ(s->frame_count, golden.frames);
        CHECK_EQ(s->data_start, golden.data_start);
        CHECK_EQ(s->file_size, golden.file_size);
        CHECK_EQ(s->table.back().timestamp_ms, golden.duration);
        CHECK(gm::sbv_offsets_valid(*s));
    }
}

void test_cache() {
    const fs::path idx = "/mnt/d/games/Gunmetal 3dfx/PICS/SBM.IDX";
    std::error_code ec;
    if (!fs::is_regular_file(idx, ec)) {
        return;
    }
    std::string error;
    auto archive = gm::BigFile::open(idx, error);
    CHECK(archive.has_value());
    if (!archive || archive->entries().empty()) {
        return;
    }
    const std::string name = archive->entries().front().name;
    const auto direct = archive->read(archive->entries().front(), error);
    CHECK(direct.has_value());
    if (!direct) {
        return;
    }

    gm::AssetCache cache(64u * 1024u * 1024u);
    const auto first = cache.get(idx, name, error);
    CHECK(first != nullptr);
    if (!first) {
        return;
    }
    CHECK(*first == *direct);
    const auto second = cache.get(idx, name, error);
    CHECK(second.get() == first.get()); // same cached blob
    auto stats = cache.stats();
    CHECK_EQ(stats.misses, 1u);
    CHECK_EQ(stats.hits, 1u);
    CHECK_EQ(stats.entries, std::size_t{1});
    CHECK_EQ(stats.bytes, static_cast<std::uint64_t>(direct->size()));

    // Case-insensitive lookup resolves to the same entry.
    std::string lower = name;
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    CHECK_EQ(cache.get(idx, lower, error).get(), first.get());
    CHECK_EQ(cache.stats().hits, 2u);

    // A pinned blob is never evicted; once released the LRU evicts it.
    gm::AssetCache small(0);
    {
        const auto pinned = small.get(idx, name, error);
        CHECK(pinned != nullptr);
        CHECK_EQ(small.stats().evictions, 0u);
    }
    CHECK_EQ(small.stats().evictions, 0u);
    const auto other = small.get(idx, archive->entries().back().name, error);
    CHECK(other != nullptr);
    CHECK(small.stats().evictions >= 1u);

    cache.clear();
    CHECK_EQ(cache.stats().entries, std::size_t{0});
    CHECK_EQ(cache.stats().bytes, 0u);
}

void test_render() {
    const fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    std::error_code ec;
    if (!fs::is_directory(game, ec)) {
        return;
    }
    auto fnv_rgb = [](const std::vector<std::uint8_t>& rgb) {
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (std::uint8_t b : rgb) {
            h ^= b;
            h *= 0x100000001b3ull;
        }
        return h;
    };
    auto read = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
    };

    gm::TexturePool pool(game);
    std::string error;

    // 20mmclp.sob: 8 vertices / 6 faces, texture CLIP1.
    const fs::path clip_path = game / "SOBS/20mmclp.sob";
    if (fs::is_regular_file(clip_path, ec)) {
        const auto bytes = read(clip_path);
        const auto model = gm::parse_sob(bytes, error);
        CHECK(model.has_value());
        if (model) {
            gm::RenderOptions options;
            options.width = 160;
            options.height = 160;
            const auto image =
                gm::render_scene(gm::build_model_meshes(*model), options, &pool, nullptr);
            CHECK_EQ(image.width, 160);
            CHECK_EQ(image.rgb.size(), std::size_t{160 * 160 * 3});
            CHECK_EQ(fnv_rgb(image.rgb), 0xc376475e890d6879ull);
        }
    }

    // MOCLASS.SOB: the largest model (957 vertices / 1130 faces).
    const fs::path moclass_path = game / "SOBS/MOCLASS.SOB";
    if (fs::is_regular_file(moclass_path, ec)) {
        const auto bytes = read(moclass_path);
        const auto model = gm::parse_sob(bytes, error);
        CHECK(model.has_value());
        if (model) {
            gm::RenderOptions options;
            options.width = 160;
            options.height = 160;
            const auto image =
                gm::render_scene(gm::build_model_meshes(*model), options, &pool, nullptr);
            CHECK_EQ(fnv_rgb(image.rgb), 0xdff93879e03ce1abull);
        }
    }

    // A few single-face models: cheap coverage of different texture atlases.
    struct ModelGolden {
        const char* name;
        std::uint64_t fnv;
    };
    const ModelGolden model_goldens[] = {
        {"code492.sob", 0xd3fa091674e86a7aull},
        {"desertpc.sob", 0x1de0c4538c49b7c7ull},
        {"moonfb.sob", 0x4fb421bc5a2ff221ull},
    };
    for (const auto& golden : model_goldens) {
        const fs::path path = fs::path(game) / "SOBS" / golden.name;
        if (!fs::is_regular_file(path, ec)) {
            continue;
        }
        const auto bytes = read(path);
        const auto model = gm::parse_sob(bytes, error);
        CHECK(model.has_value());
        if (!model) {
            continue;
        }
        gm::RenderOptions options;
        options.width = 160;
        options.height = 160;
        const auto image =
            gm::render_scene(gm::build_model_meshes(*model), options, &pool, nullptr);
        CHECK_EQ(fnv_rgb(image.rgb), golden.fnv);
    }

    // level0 at 320x200, exactly as gm/render.py renders it (its lightmap UVs
    // are computed from the world vertex, hence lightmap_world = true).
    if (fs::is_regular_file(game / "level0.sbw", ec)) {
        const auto level = gm::load_level(game, "level0", error);
        CHECK(level.has_value());
        if (level && level->world) {
            const gm::Sbc* lightmaps = level->lightmaps ? &*level->lightmaps : nullptr;
            const auto meshes = gm::build_level_meshes(*level->world, lightmaps);
            gm::RenderOptions options;
            options.width = 320;
            options.height = 200;
            options.lightmap_world = true;
            const auto image = gm::render_scene(meshes, options, &pool, lightmaps);
            CHECK_EQ(image.width, 320);
            CHECK_EQ(image.height, 200);
            CHECK_EQ(fnv_rgb(image.rgb), 0x20e80d077fd4f6bcull);

            // The engine-faithful frame (lightmaps sampled in the polygon's
            // local frame) must differ, but stay close: same geometry, other
            // lightmap texels on some faces.
            options.lightmap_world = false;
            const auto local = gm::render_scene(meshes, options, &pool, lightmaps);
            CHECK(local.rgb != image.rgb);
            CHECK_EQ(local.rgb.size(), image.rgb.size());

            // LEVEL10 interior: the same eye/target as gm/render.py's pos/look,
            // derived from the scene bounds (the scene is far larger, so this
            // also exercises the z-buffer over more polygons).
            if (fs::is_regular_file(game / "LEVEL10.sbw", ec)) {
                const auto big = gm::load_level(game, "LEVEL10", error);
                CHECK(big.has_value());
                if (big && big->world) {
                    const gm::Sbc* maps = big->lightmaps ? &*big->lightmaps : nullptr;
                    const auto big_meshes = gm::build_level_meshes(*big->world, maps);
                    gm::Vec3d lo{1e30, 1e30, 1e30};
                    gm::Vec3d hi{-1e30, -1e30, -1e30};
                    for (const auto& mesh : big_meshes) {
                        for (const auto& v : mesh.vertices) {
                            lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
                            hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
                        }
                    }
                    const gm::Vec3d center{(lo.x + hi.x) / 2.0, (lo.y + hi.y) / 2.0,
                                           (lo.z + hi.z) / 2.0};
                    const double dx = hi.x - lo.x;
                    const double dy = hi.y - lo.y;
                    const double dz = hi.z - lo.z;
                    const double big_radius =
                        std::max(1e-6, 0.5 * std::sqrt(dx * dx + dy * dy + dz * dz));
                    gm::RenderOptions inner;
                    inner.width = 320;
                    inner.height = 200;
                    inner.lightmap_world = true;
                    inner.camera_override = true;
                    inner.eye = center;
                    inner.eye.z += big_radius * 0.6;
                    inner.target = center;
                    const auto image = gm::render_scene(big_meshes, inner, &pool, maps);
                    CHECK_EQ(fnv_rgb(image.rgb), 0x25e40d77af2f01baull);
                }
            }

            // Explicit camera (pos/look) and face clipping, both identical to
            // gm/render.py's output for the same parameters.
            options.camera_override = true;
            options.eye = {0.0, 0.0, 600.0};
            options.target = {0.0, 0.0, 0.0};
            const auto view = gm::render_scene(meshes, options, &pool, lightmaps);
            CHECK_EQ(fnv_rgb(view.rgb), 0x9e54d79f29ac47c6ull);

            options.clip = 300.0f;
            const auto clipped = gm::render_scene(meshes, options, &pool, lightmaps);
            CHECK_EQ(fnv_rgb(clipped.rgb), 0xa4ccdd84a43ca565ull);
            options.camera_override = false;
            options.clip = 0.0f;

            // A fourth of the frame must be covered (a blank render is a bug).
            std::size_t covered = 0;
            for (std::size_t i = 0; i < image.rgb.size(); i += 3) {
                const bool background =
                    (image.rgb[i] == 12 && image.rgb[i + 1] == 12 && image.rgb[i + 2] == 16) ||
                    (image.rgb[i] == 34 && image.rgb[i + 1] == 38 && image.rgb[i + 2] == 52);
                covered += background ? 0 : 1;
            }
            CHECK(covered > image.rgb.size() / 3 / 4);
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
    test_sbw();
    test_sou();
    test_sbt();
    test_sbc();
    test_anim();
    test_sob();
    test_text();
    test_level();
    test_sbv();
    test_cache();
    test_render();

    if (g_failures != 0) {
        std::fprintf(stderr, "gm_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("gm_tests: all checks passed\n");
    return 0;
}
