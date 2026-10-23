// Controls reader/state tests (docs/CONTROLS_CPP.md).
//
// Synthetic half: the 31-action table, the 123-name key table, bind/override,
// conflict policy, pressed/held/released edges, deadzone and inversion on an
// analog axis, diagonal normalisation, unknown-key preservation, malformed
// `.key` files, deadzone/dt/NaN negatives.
//
// Real half (game install, read-only, skipped when absent):
//   * all three shipped KEYCFG files — names, 45 pairs, 29/19/19 bound slots,
//     every key code, the identical 14-pair stack tail, `b` all zero;
//   * GUNMETAL.CFG — PERFECT/SPLIT1 names, axis presets 4/8/0, sensitivity
//     2200 ms, buttons 5/15/5/15 and four identical 0x18C controller records;
//   * the config's button/axis bindings applied to the action state.
//
// Goldens are pinned from the shipped files and from gundx.exe 1.36's tables.

#include "input/controls.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
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
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);      \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                \
    do {                                                                                     \
        const double va = (a);                                                               \
        const double vb = (b);                                                               \
        if (!(std::fabs(va - vb) <= (eps))) {                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s=%.17g != %s=%.17g\n", __FILE__, __LINE__,   \
                         #a, va, #b, vb);                                                    \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_TEXT(expr)                                                                     \
    do {                                                                                     \
        const std::string text = (expr);                                                     \
        if (text.empty()) {                                                                  \
            std::fprintf(stderr, "FAIL %s:%d: expected an error from %s\n", __FILE__,        \
                         __LINE__, #expr);                                                   \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

using gm::Binding;
using gm::ConflictPolicy;
using gm::Controls;

// --- synthetic fixtures ----------------------------------------------------

void put_u32(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value) {
    data[offset] = static_cast<std::uint8_t>(value);
    data[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    data[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    data[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

std::vector<std::uint8_t> synthetic_key(const std::string& name,
                                        const std::vector<std::uint32_t>& codes) {
    std::vector<std::uint8_t> data(gm::kKeyFileSize, 0);
    std::memcpy(data.data(), name.data(), std::min<std::size_t>(name.size(), 16));
    for (std::size_t i = 0; i < codes.size() && i < gm::kKeyPairCount; ++i) {
        put_u32(data, 16 + 8 * i, codes[i]);
    }
    // Same 14-pair tail as the supply, to prove it is preserved verbatim.
    put_u32(data, 16 + 8 * 31, 4084748653u);
    put_u32(data, 16 + 8 * 43, 2703360u);
    put_u32(data, 16 + 8 * 44, 1761392u);
    put_u32(data, 16 + 8 * 44 + 4, 2000535658u);
    return data;
}

fs::path write_temp(const fs::path& dir, const std::string& name,
                    const std::vector<std::uint8_t>& bytes) {
    const fs::path path = dir / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return path;
}

// --- tests -----------------------------------------------------------------

void test_action_table() {
    CHECK_EQ(gm::Controls::action_name(0), std::string_view("forward"));
    CHECK_EQ(gm::Controls::action_name(1), std::string_view("back"));
    CHECK_EQ(gm::Controls::action_name(5), std::string_view("fire"));
    CHECK_EQ(gm::Controls::action_name(9), std::string_view("strafe_left"));
    CHECK_EQ(gm::Controls::action_name(10), std::string_view("strafe_right"));
    CHECK_EQ(gm::Controls::action_name(30), std::string_view("linear_right"));
    CHECK_EQ(gm::Controls::action_name(31), std::string_view());
    CHECK_EQ(gm::Controls::action_slot("forward"), 0);
    CHECK_EQ(gm::Controls::action_slot("linear_right"), 30);
    CHECK_EQ(gm::Controls::action_slot("previous_weapon"), 14); // alias
    CHECK_EQ(gm::Controls::action_slot("nonsense"), -1);
}

void test_key_names() {
    CHECK_EQ(Controls::key_name(0x11), std::string("W"));
    CHECK_EQ(Controls::key_name(0x1F), std::string("S"));
    CHECK_EQ(Controls::key_name(0x1D), std::string("LCTRL"));
    CHECK_EQ(Controls::key_name(0x39), std::string("SPACE"));
    CHECK_EQ(Controls::key_name(0x52), std::string("NUM0"));
    CHECK_EQ(Controls::key_name(0x6B), std::string("RALT"));
    CHECK_EQ(Controls::key_name(0x6D), std::string("UP"));
    CHECK_EQ(Controls::key_name(0x6F), std::string("LEFT"));
    CHECK_EQ(Controls::key_name(0x70), std::string("RIGHT"));
    CHECK_EQ(Controls::key_name(0x72), std::string("DOWN"));
    CHECK_EQ(Controls::key_name(0x7A), std::string("MACRO"));
    CHECK_EQ(Controls::key_name(-1), std::string("NONE"));
    CHECK_EQ(Controls::key_name(123), std::string("KEY_123"));
    CHECK(Controls::valid_key(0));
    CHECK(Controls::valid_key(122));
    CHECK(!Controls::valid_key(123));
}

void test_defaults() {
    Controls controls;
    CHECK_EQ(controls.profile().name, std::string("PERFECT"));
    CHECK_EQ(controls.bindings().size(), std::size_t(29));
    CHECK_EQ(controls.map_key(0x11).value_or(""), std::string("forward"));
    CHECK_EQ(controls.map_key(0x1F).value_or(""), std::string("back"));
    CHECK_EQ(controls.map_key(0x6F).value_or(""), std::string("turn_left"));
    CHECK_EQ(controls.map_key(0x70).value_or(""), std::string("turn_right"));
    CHECK_EQ(controls.map_key(0x2E).value_or(""), std::string("look_center"));
    CHECK_EQ(controls.map_key(0x1D).value_or(""), std::string("fire"));
    CHECK_EQ(controls.map_key(0x6D).value_or(""), std::string("look_up"));
    CHECK_EQ(controls.map_key(0x72).value_or(""), std::string("look_down"));
    CHECK_EQ(controls.map_key(0x39).value_or(""), std::string("activate"));
    CHECK_EQ(controls.profile().pairs[29].a, 0u);
    CHECK_EQ(controls.profile().pairs[30].a, 0u);
    CHECK_NEAR(controls.deadzone(), 0.2, 0.0);
    CHECK(controls.has_action("fire"));
    CHECK(!controls.has_action("jump"));
}

void test_bind_and_override() {
    Controls controls;
    std::string error;

    Binding fire;
    fire.action = "fire";
    fire.key = 0x59; // KANA, unbound by PERFECT
    CHECK(controls.bind(fire, ConflictPolicy::reject, &error));
    CHECK(error.empty());
    CHECK_EQ(controls.map_key(0x59).value_or(""), std::string("fire"));
    CHECK(!controls.map_key(0x1D).has_value());

    // Adding another source for the same action keeps the key binding.
    Binding button;
    button.action = "fire";
    button.button = 2;
    CHECK(controls.bind(button, ConflictPolicy::reject, &error));
    CHECK_EQ(controls.map_button(2).value_or(""), std::string("fire"));
    CHECK_EQ(controls.map_key(0x59).value_or(""), std::string("fire"));

    Binding unknown;
    unknown.action = "jump";
    unknown.key = 0x39;
    CHECK(!controls.bind(unknown, ConflictPolicy::reject, &error));
    CHECK(error.find("unknown action") != std::string::npos);

    Binding empty_binding;
    empty_binding.action = "fire";
    CHECK(!controls.bind(empty_binding, ConflictPolicy::reject, &error));
    CHECK(error.find("exactly one") != std::string::npos);

    Binding two_sources;
    two_sources.action = "fire";
    two_sources.key = 0x59;
    two_sources.button = 3;
    CHECK(!controls.bind(two_sources, ConflictPolicy::reject, &error));
    CHECK(!controls.bind(Binding{"fire", 0, -1, -1, false}, ConflictPolicy::reject, &error));
}

void test_conflicts() {
    Controls controls;
    std::string error;
    CHECK(!controls.bind(Binding{"fire", 0x11, -1, -1, false}, ConflictPolicy::reject, &error));
    CHECK(error.find("forward") != std::string::npos);
    CHECK_EQ(controls.map_key(0x11).value_or(""), std::string("forward"));

    CHECK(controls.bind(Binding{"fire", 0x11, -1, -1, false}, ConflictPolicy::allow, &error));
    const std::vector<std::string> conflicts = controls.conflicts();
    CHECK(!conflicts.empty());
    bool mentions = false;
    for (const std::string& line : conflicts) {
        mentions = mentions || (line.find("forward") != std::string::npos &&
                                line.find("fire") != std::string::npos);
    }
    CHECK(mentions);
    CHECK_EQ(controls.map_key(0x11).value_or(""), std::string("forward")); // first wins

    // Opposite-signed axis bindings are a pair, not a conflict.
    Controls axes;
    CHECK(axes.bind(Binding{"turn_left", -1, 0, -1, true}, ConflictPolicy::reject, &error));
    CHECK(axes.bind(Binding{"turn_right", -1, 0, -1, false}, ConflictPolicy::reject, &error));
    CHECK(axes.conflicts().empty());
    CHECK(!axes.bind(Binding{"fire", -1, 0, -1, false}, ConflictPolicy::reject, &error));
}

void test_edges() {
    Controls controls;
    std::string error;
    CHECK(controls.update(0.0, &error));

    controls.set_key(0x11, true);
    CHECK(controls.update(0.25, &error));
    CHECK(controls.pressed("forward"));
    CHECK(controls.held("forward"));
    CHECK(!controls.released("forward"));
    CHECK_NEAR(controls.value("forward"), 1.0, 0.0);
    CHECK_NEAR(controls.action("forward").held_time, 0.25, 1e-12);

    CHECK(controls.update(0.25, &error));
    CHECK(!controls.pressed("forward"));
    CHECK(controls.held("forward"));
    CHECK_NEAR(controls.action("forward").held_time, 0.5, 1e-12);

    controls.set_key(0x11, false);
    CHECK(controls.update(0.25, &error));
    CHECK(!controls.pressed("forward"));
    CHECK(!controls.held("forward"));
    CHECK(controls.released("forward"));
    CHECK_NEAR(controls.value("forward"), 0.0, 0.0);
    CHECK_NEAR(controls.action("forward").held_time, 0.0, 0.0);

    CHECK(controls.update(0.25, &error));
    CHECK(!controls.released("forward"));

    // Two sources on one action: releasing one keeps the action held.
    CHECK(controls.bind(Binding{"fire", -1, -1, 4, false}));
    controls.set_key(0x1D, true);
    controls.set_button(4, true);
    CHECK(controls.update(0.0, &error));
    CHECK(controls.held("fire"));
    controls.set_button(4, false);
    CHECK(controls.update(0.0, &error));
    CHECK(controls.held("fire"));
    CHECK(!controls.released("fire"));
    controls.set_key(0x1D, false);
    CHECK(controls.update(0.0, &error));
    CHECK(controls.released("fire"));
}

void test_axis_deadzone_and_invert() {
    Controls controls;
    std::string error;
    CHECK(controls.set_deadzone(0.2, &error));
    CHECK(controls.bind(Binding{"look_up", -1, 0, -1, true}, ConflictPolicy::reject, &error));

    controls.set_axis(0, 0.2);
    CHECK(controls.update(0.0, &error));
    CHECK_NEAR(controls.value("look_up"), 0.0, 0.0);
    CHECK(!controls.held("look_up"));

    controls.set_axis(0, 0.6); // invert -> -0.6 -> deadzone -> -0.5
    CHECK(controls.update(0.0, &error));
    CHECK_NEAR(controls.value("look_up"), -0.5, 1e-12);
    CHECK(controls.held("look_up"));

    controls.set_axis(0, -0.6); // invert -> +0.6 -> deadzone -> +0.5
    CHECK(controls.update(0.0, &error));
    CHECK_NEAR(controls.value("look_up"), 0.5, 1e-12);

    controls.set_axis(0, -1.0);
    CHECK(controls.update(0.0, &error));
    CHECK_NEAR(controls.value("look_up"), 1.0, 1e-12);

    const auto sample = controls.map_axis(0, 0.6);
    CHECK(sample.has_value());
    CHECK_EQ(sample->action, std::string("look_up"));
    CHECK_NEAR(sample->value, -0.5, 1e-12); // invert applies to the raw value
    CHECK(!controls.map_axis(0, std::numeric_limits<double>::quiet_NaN()).has_value());
    CHECK(!controls.map_axis(1, 0.0).has_value());

    // A deadzone of 1 silences everything without dividing by zero.
    CHECK(controls.set_deadzone(1.0, &error));
    controls.set_axis(0, 1.0);
    CHECK(controls.update(0.0, &error));
    CHECK_NEAR(controls.value("look_up"), 0.0, 0.0);
}

void test_diagonal() {
    Controls controls;
    std::string error;
    controls.set_key(0x11, true); // W forward
    controls.set_key(0x20, true); // D strafe right
    CHECK(controls.update(0.0, &error));
    const gm::Vec2 move = controls.move_vector();
    CHECK_NEAR(std::hypot(move.x, move.y), 1.0, 1e-12);
    CHECK_NEAR(move.x, std::sqrt(0.5), 1e-12);
    CHECK_NEAR(move.y, std::sqrt(0.5), 1e-12);

    controls.set_key(0x1F, true); // S back cancels W
    CHECK(controls.update(0.0, &error));
    const gm::Vec2 opposite = controls.move_vector();
    CHECK_NEAR(opposite.x, 0.0, 0.0);
    CHECK_NEAR(opposite.y, 1.0, 1e-12);

    controls.set_key(0x11, false);
    controls.set_key(0x20, false);
    controls.set_key(0x1F, false);
    CHECK(controls.update(0.0, &error));
    CHECK_NEAR(std::hypot(controls.move_vector().x, controls.move_vector().y), 0.0, 0.0);
}

void test_negatives() {
    Controls controls;
    std::string error;

    CHECK(!controls.set_deadzone(1.01, &error));
    CHECK_TEXT(error);
    CHECK(!controls.set_deadzone(-0.01, &error));
    CHECK(!controls.set_deadzone(std::numeric_limits<double>::quiet_NaN(), &error));

    CHECK(!controls.update(-0.1, &error));
    CHECK_TEXT(error);
    CHECK(!controls.update(std::numeric_limits<double>::infinity(), &error));

    controls.set_key(0x11, true);
    CHECK(controls.update(0.0, &error));
    CHECK_NEAR(controls.value("forward"), 1.0, 0.0);
    controls.set_axis(0, std::numeric_limits<double>::quiet_NaN());
    CHECK(!controls.update(0.0, &error));
    CHECK_TEXT(error);
    // The failed update leaves the previous state untouched.
    CHECK_NEAR(controls.value("forward"), 1.0, 0.0);
    CHECK(controls.held("forward"));
    controls.set_axis(0, 0.0);
    CHECK(controls.update(0.0, &error));
}

void test_synthetic_key_file(const fs::path& dir) {
    const std::vector<std::uint32_t> codes = {
        0x11, 0x1F, 0x6F, 0x70, 0x2E, 0x1D, 0x6D, 0x72, 0x38, 0x1E,
        0x20, 0x12, 0x10, 0x1B, 0x1A, 0x39, 0x28, 0x27, 0x1C, 0x21,
        0x22, 0x31, 0x14, 0x2F, 0x13, 0x32, 0x30, 0x2D, 0x2C, 0x00,
        0x00,
    };
    const fs::path path = write_temp(dir, "TEST.key", synthetic_key("TEST", codes));
    Controls controls;
    std::string error;
    CHECK(controls.load_key_file(path, &error));
    CHECK(error.empty());
    CHECK_EQ(controls.profile().name, std::string("TEST"));
    CHECK_EQ(controls.profile().bytes_read, std::size_t(gm::kKeyFileSize));
    CHECK(!controls.profile().have_tail);
    CHECK_EQ(controls.profile().pairs[0].a, 0x11u);
    CHECK_EQ(controls.profile().pairs[28].a, 0x2Cu);
    CHECK_EQ(controls.profile().pairs[29].a, 0u);
    CHECK_EQ(controls.profile().pairs[31].a, 4084748653u);
    CHECK_EQ(controls.profile().pairs[43].a, 2703360u);
    CHECK_EQ(controls.profile().pairs[44].a, 1761392u);
    CHECK_EQ(controls.profile().pairs[44].b, 2000535658u);
    CHECK_EQ(controls.map_key(0x11).value_or(""), std::string("forward"));

    // Unknown but non-corrupt key: preserved, no error.
    std::vector<std::uint32_t> with_unknown = codes;
    with_unknown[5] = 200;
    const fs::path unknown_path =
        write_temp(dir, "UNKNOWN.key", synthetic_key("UNKNOWN", with_unknown));
    CHECK(controls.load_key_file(unknown_path, &error));
    CHECK_EQ(controls.unknown_keys().size(), std::size_t(1));
    CHECK_EQ(controls.unknown_keys()[0].slot, 5);
    CHECK_EQ(controls.unknown_keys()[0].code, 200u);
    CHECK_EQ(controls.profile().pairs[5].a, 200u);
    CHECK_EQ(controls.map_key(200).value_or(""), std::string("fire"));
    CHECK_EQ(Controls::key_name(200), std::string("KEY_200"));
    controls.set_key(200, true);
    CHECK(controls.update(0.0, &error));
    CHECK(controls.held("fire"));

    // Truncated file.
    std::vector<std::uint8_t> truncated(gm::kKeyFileSize, 0);
    std::memcpy(truncated.data(), "TRUNC", 5);
    const fs::path truncated_path =
        write_temp(dir, "TRUNC.key", std::vector<std::uint8_t>(truncated.begin(),
                                                               truncated.begin() + 100));
    CHECK(!controls.load_key_file(truncated_path, &error));
    CHECK(error.find("truncated") != std::string::npos);
    CHECK_EQ(controls.profile().name, std::string("UNKNOWN")); // unchanged

    // Corrupt code with the top bit set.
    std::vector<std::uint32_t> corrupt = codes;
    corrupt[3] = 0xFFFFFFFFu;
    const fs::path corrupt_path =
        write_temp(dir, "CORRUPT.key", synthetic_key("CORRUPT", corrupt));
    CHECK(!controls.load_key_file(corrupt_path, &error));
    CHECK(error.find("bad key code") != std::string::npos);
    CHECK_EQ(controls.profile().name, std::string("UNKNOWN"));

    // Missing file.
    CHECK(!controls.load_key_file(dir / "missing.key", &error));
    CHECK(error.find("cannot open") != std::string::npos);

    // Empty name.
    CHECK(!controls.load_key_file(write_temp(dir, "EMPTY.key", synthetic_key("", codes)), &error));
    CHECK(error.find("empty profile name") != std::string::npos);

    // Trailing bytes are read up to 0x178 and flagged.
    std::vector<std::uint8_t> longer = synthetic_key("LONG", codes);
    longer.push_back(0xAB);
    const fs::path longer_path = write_temp(dir, "LONG.key", longer);
    CHECK(controls.load_key_file(longer_path, &error));
    CHECK(controls.profile().have_tail);
    CHECK_EQ(controls.profile().bytes_read, std::size_t(gm::kKeyFileSize + 1));
}

void test_real_key_files(const fs::path& keycfg) {
    struct Golden {
        const char* file;
        const char* name;
        int bound;
        std::array<std::uint32_t, 31> codes;
    };
    const std::array<Golden, 3> goldens = {{
        {"PERFECT.key",
         "PERFECT",
         29,
         {0x11, 0x1F, 0x6F, 0x70, 0x2E, 0x1D, 0x6D, 0x72, 0x38, 0x1E, 0x20,
          0x12, 0x10, 0x1B, 0x1A, 0x39, 0x28, 0x27, 0x1C, 0x21, 0x22, 0x31,
          0x14, 0x2F, 0x13, 0x32, 0x30, 0x2D, 0x2C, 0x00, 0x00}},
        {"SPLIT1.KEY",
         "SPLIT1",
         19,
         {0x16, 0x32, 0x23, 0x25, 0x24, 0x1D, 0x17, 0x33, 0x38, 0x2F, 0x30,
          0x1E, 0x2C, 0x2A, 0x3A, 0x39, 0x0F, 0x29, 0x10, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {"SPLIT2.KEY",
         "SPLIT2",
         19,
         {0x48, 0x50, 0x4B, 0x4D, 0x4C, 0x52, 0x49, 0x51, 0x6B, 0x6F, 0x70,
          0x4E, 0x4A, 0x36, 0x1C, 0x4F, 0x1B, 0x1A, 0x66, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    }};

    // `KEYCFG/*.KEY` are user data: the game rewrites them when controls are
    // remapped.  Exact shipped values are checked only while the install still
    // matches docs/manifests/gunmetal-3dfx-136.sha256 (CMake then sets
    // GM_KEYCFG_SHIPPED); otherwise the parser invariants below still run.
    const bool shipped = std::getenv("GM_KEYCFG_SHIPPED") != nullptr;
    if (!shipped) {
        std::printf("controls: KEYCFG is user-modified, exact shipped values skipped\n");
    }

    for (const Golden& golden : goldens) {
        Controls controls;
        std::string error;
        const fs::path path = keycfg / golden.file;
        CHECK(controls.load_key_file(path, &error));
        if (!error.empty()) {
            std::fprintf(stderr, "  %s: %s\n", golden.file, error.c_str());
            continue;
        }
        CHECK_EQ(controls.profile().bytes_read, std::size_t(gm::kKeyFileSize));
        CHECK(!controls.profile().have_tail);
        CHECK(controls.unknown_keys().empty());
        CHECK_EQ(controls.profile().pairs.size(), std::size_t(gm::kKeyPairCount));

        // Parser self-consistency holds for any profile, shipped or remapped.
        int bound = 0;
        for (int slot = 0; slot < gm::kActionCount; ++slot) {
            const gm::KeyPair& pair = controls.profile().pairs[static_cast<std::size_t>(slot)];
            if (pair.a != 0) {
                ++bound;
                CHECK_EQ(controls.profile().bindings[static_cast<std::size_t>(slot)].key,
                         static_cast<int>(pair.a));
                CHECK_EQ(controls.map_key(static_cast<int>(pair.a)).value_or(""),
                         std::string(gm::Controls::action_name(slot)));
            }
        }
        if (!shipped) {
            continue;
        }
        CHECK_EQ(controls.profile().name, std::string(golden.name));
        for (int slot = 0; slot < gm::kActionCount; ++slot) {
            const gm::KeyPair& pair = controls.profile().pairs[static_cast<std::size_t>(slot)];
            CHECK_EQ(pair.a, golden.codes[static_cast<std::size_t>(slot)]);
            CHECK_EQ(pair.b, 0u);
        }
        CHECK_EQ(bound, golden.bound);

        // The same 14-pair stack tail in all three files.
        CHECK_EQ(controls.profile().pairs[31].a, 4084748653u);
        CHECK_EQ(controls.profile().pairs[31].b, 0u);
        for (int i = 32; i <= 42; ++i) {
            CHECK_EQ(controls.profile().pairs[static_cast<std::size_t>(i)].a, 0u);
            CHECK_EQ(controls.profile().pairs[static_cast<std::size_t>(i)].b, 0u);
        }
        CHECK_EQ(controls.profile().pairs[43].a, 2703360u);
        CHECK_EQ(controls.profile().pairs[44].a, 1761392u);
        CHECK_EQ(controls.profile().pairs[44].b, 2000535658u);
    }

    // Characteristic bindings of each shipped layout.
    Controls perfect;
    Controls split1;
    Controls split2;
    CHECK(perfect.load_key_file(keycfg / "PERFECT.key"));
    CHECK(split1.load_key_file(keycfg / "SPLIT1.KEY"));
    CHECK(split2.load_key_file(keycfg / "SPLIT2.KEY"));
    if (!shipped) {
        return;
    }
    CHECK_EQ(perfect.map_key(0x11).value_or(""), std::string("forward"));
    CHECK_EQ(perfect.map_key(0x2E).value_or(""), std::string("look_center"));
    CHECK_EQ(split1.map_key(0x16).value_or(""), std::string("forward"));
    CHECK_EQ(split1.map_key(0x32).value_or(""), std::string("back"));
    CHECK_EQ(split1.map_key(0x23).value_or(""), std::string("turn_left"));
    CHECK_EQ(split2.map_key(0x48).value_or(""), std::string("forward"));
    CHECK_EQ(split2.map_key(0x6B).value_or(""), std::string("strafe"));
    CHECK_EQ(split2.map_key(0x6F).value_or(""), std::string("strafe_left"));
    CHECK_EQ(split2.map_key(0x70).value_or(""), std::string("strafe_right"));
    CHECK_EQ(split2.map_key(0x4E).value_or(""), std::string("throttle_up"));
    CHECK_EQ(split2.map_key(0x4A).value_or(""), std::string("throttle_down"));
}

void test_real_config(const fs::path& path, const fs::path& keycfg) {
    const std::vector<std::uint8_t> original = [&] {
        std::ifstream in(path, std::ios::binary);
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                         std::istreambuf_iterator<char>());
    }();
    CHECK_EQ(original.size(), std::size_t(gm::kConfigSize));

    Controls controls;
    std::string error;
    CHECK(controls.load_config(path, &error));
    if (!error.empty()) {
        std::fprintf(stderr, "  GUNMETAL.CFG: %s\n", error.c_str());
        return;
    }
    const gm::ConfigProfile& config = controls.config();
    CHECK_EQ(config.key_names[0], std::string("PERFECT"));
    CHECK_EQ(config.key_names[1], std::string("SPLIT1"));
    CHECK_EQ(config.joystick_used_by, 1u);
    CHECK_EQ(config.axis_preset[0], 4u); // Linear Turn L/R
    CHECK_EQ(config.axis_preset[1], 8u); // Look Up/Down
    CHECK_EQ(config.axis_preset[2], 0u); // Not Used
    CHECK_EQ(config.sensitivity_ms[0], 2200u);
    CHECK_EQ(config.sensitivity_ms[1], 2200u);
    CHECK_EQ(config.sensitivity_ms[2], 2200u);
    CHECK_EQ(config.buttons[0], 5u);  // Fire
    CHECK_EQ(config.buttons[1], 15u); // Activate
    CHECK_EQ(config.buttons[2], 5u);
    CHECK_EQ(config.buttons[3], 15u);
    const std::array<double, 3> seconds = config.sensitivity_seconds();
    CHECK_NEAR(seconds[0], 2.2, 1e-12);
    CHECK_NEAR(seconds[1], 2.2, 1e-12);
    CHECK_NEAR(seconds[2], 2.2, 1e-12);
    CHECK_EQ(config.raw.size(), std::size_t(gm::kConfigSize));

    for (const gm::ControllerRecord& record : config.records) {
        CHECK_EQ(record.slot, 0u);
        CHECK_EQ(record.buttons[0], 0x20u);
        CHECK_EQ(record.buttons[1], 0x8000u);
        for (std::size_t i = 2; i < record.buttons.size(); ++i) {
            CHECK_EQ(record.buttons[i], 0xFFFFFFFFu);
        }
        CHECK_EQ(record.axes[0].mask_a, 4u);
        CHECK_EQ(record.axes[0].mask_b, 8u);
        CHECK_EQ(record.axes[1].mask_a, 1u);
        CHECK_EQ(record.axes[1].mask_b, 2u);
        CHECK_EQ(record.axes[2].mask_a, 0x800u);
        CHECK_EQ(record.axes[2].mask_b, 0x800u);
        for (int i = 3; i < 6; ++i) {
            CHECK_EQ(record.axes[static_cast<std::size_t>(i)].mask_a, 0xFFFFFFFFu);
        }
        CHECK_EQ(record.bits[0].bit, 6u);
        CHECK_EQ(record.bits[1].bit, 13u);
        CHECK_EQ(record.bits[2].bit, 7u);
        CHECK_EQ(record.bits[3].bit, 14u);
        for (int i = 4; i < 16; ++i) {
            CHECK_EQ(record.bits[static_cast<std::size_t>(i)].bit, 0xFFFFFFFFu);
            CHECK_EQ(record.bits[static_cast<std::size_t>(i)].lo, 0u);
            CHECK_EQ(record.bits[static_cast<std::size_t>(i)].hi, 0u);
        }
    }

    // Controller bindings were applied on top of the default PERFECT keys.
    CHECK_EQ(controls.map_key(0x11).value_or(""), std::string("forward"));
    CHECK_EQ(controls.map_button(0).value_or(""), std::string("fire"));
    CHECK_EQ(controls.map_button(1).value_or(""), std::string("activate"));
    CHECK_EQ(controls.map_button(2).value_or(""), std::string("fire"));
    CHECK_EQ(controls.map_button(3).value_or(""), std::string("activate"));
    CHECK_EQ(controls.conflicts().empty(), true);

    controls.set_axis(0, 1.0);
    CHECK(controls.update(0.0, &error));
    CHECK_NEAR(controls.value("turn_left"), -1.0, 1e-12);  // preset 4, inverted
    CHECK_NEAR(controls.value("turn_right"), 1.0, 1e-12);  // preset 4, positive
    CHECK_NEAR(controls.value("throttle_up"), 0.0, 1e-12); // axis 1 untouched
    controls.set_axis(0, 0.0);

    // Negatives: truncated file and bad magic.
    const fs::path dir = path.parent_path();
    const fs::path temp = fs::temp_directory_path() / "gm_controls_cfg";
    fs::create_directories(temp);
    CHECK(!controls.load_config(write_temp(temp, "trunc.cfg",
                                           std::vector<std::uint8_t>(original.begin(),
                                                                     original.begin() + 100)),
                                &error));
    CHECK(error.find("truncated") != std::string::npos);
    std::vector<std::uint8_t> bad_magic = original;
    bad_magic[20] = 'X';
    CHECK(!controls.load_config(write_temp(temp, "magic.cfg", bad_magic), &error));
    CHECK(error.find("magic") != std::string::npos);

    // The two config profile names exist on disk.
    CHECK(fs::exists(keycfg / (config.key_names[0] + ".key")) ||
          fs::exists(keycfg / (config.key_names[0] + ".KEY")));
    CHECK(fs::exists(keycfg / (config.key_names[1] + ".KEY")) ||
          fs::exists(keycfg / (config.key_names[1] + ".key")));
    fs::remove(temp / "trunc.cfg");
    fs::remove(temp / "magic.cfg");
    fs::remove(temp);
}

} // namespace

int main() {
    const fs::path temp = fs::temp_directory_path() / "gm_controls_tests";
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp, ec);

    test_action_table();
    test_key_names();
    test_defaults();
    test_bind_and_override();
    test_conflicts();
    test_edges();
    test_axis_deadzone_and_invert();
    test_diagonal();
    test_negatives();
    test_synthetic_key_file(temp);

    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_DIR")) {
        game = env;
    }
    const fs::path keycfg = game / "KEYCFG";
    if (fs::exists(keycfg / "PERFECT.key")) {
        test_real_key_files(keycfg);
        if (fs::exists(game / "GUNMETAL.CFG")) {
            test_real_config(game / "GUNMETAL.CFG", keycfg);
        } else {
            std::printf("controls: SKIP GUNMETAL.CFG (not installed)\n");
        }
    } else {
        std::printf("controls: SKIP real KEYCFG/GUNMETAL.CFG (%s missing)\n",
                    keycfg.string().c_str());
    }

    fs::remove_all(temp, ec);
    if (g_failures != 0) {
        std::fprintf(stderr, "controls: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("controls: ok\n");
    return 0;
}
