// C++ game state bridge (docs/GAME_STATE.md).
//
// Synthetic part: a hand-built save with one player, two doors and an opaque
// block 19 exercises `from_save` -> `apply_to_player` -> `apply_to_doors` ->
// `update_save` -> `to_save` and a byte-exact no-op round-trip.
//
// Real part: saved0.sbt / saved1.sbt from the game install pin the player
// position/angles/gauges/ammo, the level clock, and all 25 door records; the
// no-op rebuild must reproduce both files byte-for-byte (same FNV as
// docs/SAVE_WRITER.md) and position/ammo/door edits must survive a re-parse.
//
// Negatives: a broken container, a save without block 9, a missing present
// slot, a foreign level, and out-of-range player/door values.
#include "game/state.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "player/controller.h"
#include "render/collide.h"
#include "res/save.h"
#include "res/sbt.h"
#include "res/sbw.h"
#include "world/doors.h"

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

const fs::path kGame = "/mnt/d/games/Gunmetal 3dfx";

struct GoldenSave {
    const char* file;
    std::uint64_t fnv;
    std::uint32_t level_clock;
    std::array<double, 3> position;
    std::array<double, 3> angles;
    std::int32_t history_index;
    std::int32_t weapon_slot;
    std::array<std::uint32_t, 2> ammo_first_two;
};

constexpr std::array<GoldenSave, 2> kSaves = {{
    {"saved0.sbt", 0xb994929515f53af5ULL, 4288,
     {{1116.173751898651, 798.2076214059642, 48.0}},
     {{0.0, 0.0, 5.0736149946795805}}, 194, 0, {{2000, 90}}},
    {"saved1.sbt", 0x0d038197e152cd65ULL, 8148,
     {{1156.355385156283, 797.2758300679517, 48.0}},
     {{0.0, 0.0, 4.747052494679579}}, 130, 1, {{1975, 87}}},
}};

std::vector<std::uint8_t> read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> out;
    if (size > 0) {
        out.resize(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(out.data()), size);
    }
    return out;
}

std::uint64_t fnv1a(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (std::uint8_t b : bytes) {
        hash ^= b;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

void put_u32(std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x) {
    v[off] = static_cast<std::uint8_t>(x & 0xff);
    v[off + 1] = static_cast<std::uint8_t>((x >> 8) & 0xff);
    v[off + 2] = static_cast<std::uint8_t>((x >> 16) & 0xff);
    v[off + 3] = static_cast<std::uint8_t>((x >> 24) & 0xff);
}

void put_i32(std::vector<std::uint8_t>& v, std::size_t off, std::int32_t x) {
    put_u32(v, off, static_cast<std::uint32_t>(x));
}

void put_f64(std::vector<std::uint8_t>& v, std::size_t off, double x) {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(x);
    for (std::size_t i = 0; i < 8; ++i) {
        v[off + i] = static_cast<std::uint8_t>((bits >> (8 * i)) & 0xff);
    }
}

gm::SaveBlock make_block(std::uint8_t id) {
    gm::SaveBlock block;
    block.id = id;
    return block;
}

/// Serialise one 0xB8 runtime door record.  Only the fields the state bridge
/// reads are pinned; the rest stay zero (the record still has the full size).
void encode_door(std::vector<std::uint8_t>& out, std::size_t base,
                 const gm::SaveDoor& door) {
    for (std::size_t i = 0; i < 9; ++i) {
        put_f64(out, base + 8 * i, door.box[i]);
    }
    for (std::size_t i = 0; i < 3; ++i) {
        put_f64(out, base + 0x48 + 8 * i, door.delta[i]);
    }
    put_f64(out, base + 0x60, door.constant);
    put_i32(out, base + 0x68, door.flags);
    for (std::size_t i = 0; i < 5; ++i) {
        put_i32(out, base + 0x6c + 4 * i, door.ids[i]);
    }
    put_i32(out, base + 0x80, door.state);
    put_i32(out, base + 0x84, door.sector);
    put_i32(out, base + 0x88, door.object);
    put_u32(out, base + 0x8c, door.phase_prev);
    put_u32(out, base + 0x90, door.phase_next);
    put_i32(out, base + 0x94, door.step);
    put_i32(out, base + 0x98, door.param5);
    put_i32(out, base + 0x9c, door.param6);
    for (std::size_t i = 0; i < 3; ++i) {
        put_i32(out, base + 0xa0 + 4 * i, door.v[i]);
        put_i32(out, base + 0xac + 4 * i, door.n[i]);
    }
}

gm::SaveDoor make_door(int sector, int object, int id, int flags, int state,
                       std::uint32_t phase, std::int32_t step) {
    gm::SaveDoor door;
    door.box = {{10.0, 20.0, 30.0, 10.0, 40.0, 30.0, 0.0, 20.0, 5.0}};
    door.flags = flags;
    door.ids = {{id, 0, 0, 0, 0}};
    door.state = state;
    door.sector = sector;
    door.object = object;
    door.phase_prev = phase;
    door.phase_next = phase;
    door.step = step;
    door.constant = 0.03;
    return door;
}

gm::SavePlayer make_player(double x, double y, double z, double yaw) {
    gm::SavePlayer player;
    player.present[0] = true;
    player.has_slot = true;
    player.slot_raw[0].assign(gm::kSavePlayerStructSize, 0);
    player.position = {{x, y, z}};
    player.angles = {{0.0, 0.0, yaw}};
    player.history_index = 9;
    player.exists = 0x1234u;
    player.status_present = 1;
    player.object_id = 16;
    player.armor_max = 10000;
    player.armor = 7500;
    player.shield_max = 8000;
    player.shields = {{8000, 4000, 2000, 1000}};
    player.weapon_slot = 2;
    player.weapon_mask = 4;
    player.ammo = {{10, 20, 30, 40, 0, 0, 0, 0, 0, 0, 0, 0}};
    player.pair_headers = {{"LEVEL0", ""}};
    return player;
}

/// Synthetic container: door block 2, player block 9 and an opaque block 19
/// (`ITEM`, fixed 0x7080 bytes) that the bridge must copy untouched.
gm::Save make_synthetic_save() {
    gm::Save save;
    save.header.title = "SYNTH";
    save.header.level_name = "LEVEL0";
    save.header.fields[1] = 1234;

    save.blocks.push_back(make_block(2));
    save.blocks.push_back(make_block(3));
    save.blocks.push_back(make_block(9));
    gm::SaveBlock item = make_block(19);
    item.raw.assign(0x7080, 0x5a);
    save.blocks.push_back(std::move(item));

    save.player = make_player(10.0, -20.0, 30.0, 0.25);
    save.doors.push_back(make_door(2, 3, 11, 3, 0, 0, 700));
    save.doors.push_back(make_door(4, 5, 12, 3, -1, 100, 800));

    gm::SaveThing thing;
    thing.slot = 5;
    thing.raw.fill(0x11);
    save.thing_present[5] = true;
    save.things.push_back(thing);
    return save;
}

std::vector<std::uint8_t> serialize(const gm::Save& save) {
    std::string error;
    const std::optional<std::vector<std::uint8_t>> bytes = save.to_bytes(error);
    if (!bytes) {
        std::fprintf(stderr, "FAIL: to_bytes: %s\n", error.c_str());
        ++g_failures;
        return {};
    }
    return *bytes;
}

std::optional<gm::Save> parse_bytes(const std::vector<std::uint8_t>& bytes,
                                    const char* what) {
    std::string error;
    std::optional<gm::Save> save = gm::parse_save(bytes, error);
    if (!save) {
        std::fprintf(stderr, "FAIL %s: parse: %s\n", what, error.c_str());
        ++g_failures;
    }
    return save;
}

// --- synthetic -------------------------------------------------------------

void test_synthetic() {
    gm::Save source = make_synthetic_save();
    const std::vector<std::uint8_t> original = serialize(source);
    CHECK(!original.empty());

    std::optional<gm::Save> parsed = parse_bytes(original, "synthetic");
    if (!parsed) {
        return;
    }
    CHECK_EQ(parsed->player.has_value(), true);
    CHECK_EQ(parsed->doors.size(), std::size_t(2));
    CHECK_EQ(parsed->things.size(), std::size_t(1));
    CHECK_EQ(parsed->thing_present[5], true);
    CHECK(parsed->find_block(19) != nullptr);

    std::string error;
    auto state = gm::GameState::from_save(*parsed, "level0", error);
    CHECK(state.has_value());
    if (!state) {
        std::fprintf(stderr, "FAIL synthetic: from_save: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    CHECK_EQ(state->level_name, std::string("LEVEL0"));
    CHECK_EQ(state->level_clock, std::uint32_t(1234));
    CHECK_EQ(state->has_player, true);
    CHECK_EQ(state->doors.size(), std::size_t(2));
    CHECK_EQ(state->things.size(), std::size_t(1));
    CHECK_EQ(state->things[0].slot, std::size_t(5));
    CHECK_EQ(state->thing_present[5], true);
    CHECK(state->validate(&error));

    // Round-trip without changes: both the direct writer and to_save() must
    // reproduce the synthetic container byte-for-byte, opaque block included.
    CHECK(state->update_save(*parsed, &error));
    const std::vector<std::uint8_t> rebuilt = serialize(*parsed);
    CHECK(rebuilt == original);
    CHECK_EQ(fnv1a(rebuilt), fnv1a(original));

    std::optional<gm::Save> templated = state->to_save(error);
    CHECK(templated.has_value());
    if (templated) {
        CHECK(serialize(*templated) == original);
    }

    // Save -> runtime player.
    gm::PlayerState player;
    player.pos = gm::Vec3d{1.0, 2.0, 3.0};
    player.vel = gm::Vec3d{9.0, 9.0, 9.0};
    player.height = 10.0;
    player.eye_height = 10.0;
    player.on_ground = true;
    CHECK(state->apply_to_player(player, &error));
    CHECK_NEAR(player.pos.x, 10.0, 0.0);
    CHECK_NEAR(player.pos.y, -20.0, 0.0);
    CHECK_NEAR(player.pos.z, 30.0, 0.0);
    CHECK_NEAR(player.yaw, 0.25, 0.0);
    CHECK_NEAR(player.vel.x, 0.0, 0.0);
    CHECK_NEAR(player.vel.y, 0.0, 0.0);
    CHECK_NEAR(player.vel.z, 0.0, 0.0);
    CHECK_EQ(player.on_ground, false);
    CHECK_NEAR(player.height, 10.0, 0.0);
    CHECK_NEAR(player.eye_height, 10.0, 0.0);

    // Save -> runtime doors.  The synthetic level carries the same two door
    // records, so the `(sector, object)` lookup must find both.
    gm::SbtLevel level;
    gm::SbtRecord doors;
    doors.type = 2;
    doors.name = "DOOR";
    doors.payload.assign(4 + 2 * gm::kSaveDoorRecordSize, 0);
    put_u32(doors.payload, 0, 2);
    for (std::size_t i = 0; i < state->doors.size(); ++i) {
        encode_door(doors.payload, 4 + i * gm::kSaveDoorRecordSize, state->doors[i]);
    }
    level.records.push_back(std::move(doors));

    gm::SbwLevel world_level;
    gm::WorldState world;
    std::string world_error;
    CHECK(world.build(level, world_level, &world_error));
    CHECK_EQ(world.doors().size(), std::size_t(2));

    CHECK_EQ(state->apply_to_doors(world, &error), std::size_t(2));
    CHECK_EQ(world.doors()[0].sector, 2);
    CHECK_EQ(world.doors()[0].object, 3);
    CHECK_EQ(world.doors()[0].id, 11);
    CHECK_EQ(world.doors()[0].step, 700);
    CHECK_EQ(world.doors()[0].state, gm::DoorState::Closed);
    CHECK_NEAR(world.doors()[0].closed[0], 10.0, 0.0);
    CHECK_NEAR(world.doors()[0].open[1], 40.0, 0.0);
    CHECK_NEAR(world.doors()[0].size[2], 5.0, 0.0);
    CHECK_EQ(world.doors()[1].sector, 4);
    CHECK_EQ(world.doors()[1].object, 5);
    CHECK_EQ(world.doors()[1].step, 800);
    CHECK_EQ(world.doors()[1].state, gm::DoorState::Closing);
    CHECK_EQ(world.doors()[1].phase, std::uint32_t(100));

    // Edit through the value API, then read the edit back from a re-parsed
    // container.  The opaque block and the untouched door must survive.
    state->position[0] += 1.25;
    state->ammo[0] = 777;
    state->doors[0].state = 1;
    state->doors[0].phase_prev = 0;
    state->doors[0].phase_next = 32768;
    CHECK(state->update_save(*parsed, &error));
    const std::vector<std::uint8_t> edited = serialize(*parsed);
    CHECK(fnv1a(edited) != fnv1a(original));

    std::optional<gm::Save> check = parse_bytes(edited, "synthetic edited");
    if (check) {
        CHECK(check->player.has_value());
        if (check->player) {
            CHECK_NEAR(check->player->position[0], 11.25, 0.0);
            CHECK_NEAR(check->player->position[1], -20.0, 0.0);
            CHECK_EQ(check->player->ammo[0], std::uint32_t(777));
            CHECK_EQ(check->player->ammo[1], std::uint32_t(20));
            CHECK_EQ(check->player->armor, std::int32_t(7500));
        }
        CHECK_EQ(check->doors.size(), std::size_t(2));
        if (check->doors.size() == 2) {
            CHECK_EQ(check->doors[0].state, 1);
            CHECK_EQ(check->doors[0].phase_prev, std::uint32_t(0));
            CHECK_EQ(check->doors[0].phase_next, std::uint32_t(32768));
            CHECK_EQ(check->doors[1].state, -1);
            CHECK_EQ(check->doors[1].sector, 4);
            CHECK_EQ(check->doors[1].object, 5);
        }
        CHECK_EQ(check->things.size(), std::size_t(1));
        if (check->things.size() == 1) {
            CHECK_EQ(check->things[0].slot, std::size_t(5));
            CHECK_EQ(check->things[0].raw[0], std::uint8_t(0x11));
            CHECK_EQ(check->things[0].raw.back(), std::uint8_t(0x11));
        }
        const gm::SaveBlock* item = check->find_block(19);
        CHECK(item != nullptr);
        if (item) {
            CHECK_EQ(item->raw.size(), std::size_t(0x7080));
            CHECK_EQ(item->raw.front(), std::uint8_t(0x5a));
            CHECK_EQ(item->raw.back(), std::uint8_t(0x5a));
        }
    }
}

// --- real shipped saves ----------------------------------------------------

void test_real_save(const GoldenSave& golden) {
    const std::vector<std::uint8_t> bytes = read_all(kGame / "SAVEGAME" / golden.file);
    if (bytes.empty()) {
        std::printf("game_state: %s not installed, skipping\n", golden.file);
        return;
    }
    CHECK_EQ(bytes.size(), std::size_t(243368));
    CHECK_EQ(fnv1a(bytes), golden.fnv);

    std::optional<gm::Save> parsed = parse_bytes(bytes, golden.file);
    if (!parsed) {
        return;
    }

    std::string error;
    // Path/extension/case variants of the level id are equivalent.
    auto state = gm::GameState::from_save(*parsed, "LEVEL0.sbt", error);
    CHECK(state.has_value());
    if (!state) {
        std::fprintf(stderr, "FAIL %s: from_save: %s\n", golden.file, error.c_str());
        ++g_failures;
        return;
    }
    CHECK(state->validate(&error));
    CHECK_EQ(state->level_name, std::string("LEVEL0"));
    CHECK_EQ(state->fields[1], golden.level_clock);
    CHECK_EQ(state->level_clock, golden.level_clock);
    CHECK_EQ(state->has_player, true);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK_NEAR(state->position[i], golden.position[i], 1e-12);
        CHECK_NEAR(state->angles[i], golden.angles[i], 1e-12);
    }
    CHECK_EQ(state->history_index, golden.history_index);
    CHECK_EQ(state->armor_max, std::int32_t(10000));
    CHECK_EQ(state->armor, std::int32_t(10000));
    CHECK_EQ(state->shield_max, std::int32_t(8000));
    for (std::int32_t v : state->shields) {
        CHECK_EQ(v, std::int32_t(8000));
    }
    CHECK_EQ(state->weapon_slot, golden.weapon_slot);
    CHECK_EQ(state->weapon_mask, std::uint32_t(3));
    CHECK_EQ(state->ammo[0], golden.ammo_first_two[0]);
    CHECK_EQ(state->ammo[1], golden.ammo_first_two[1]);
    for (std::size_t i = 2; i < state->ammo.size(); ++i) {
        CHECK_EQ(state->ammo[i], std::uint32_t(0));
    }

    // Door goldens shared by both shipped saves.
    CHECK_EQ(state->doors.size(), std::size_t(25));
    if (state->doors.size() == 25) {
        const gm::SaveDoor& d0 = state->doors[0];
        CHECK_EQ(d0.flags, 3);
        CHECK_EQ(d0.ids[0], 2);
        CHECK_EQ(d0.state, 0);
        CHECK_EQ(d0.sector, 4);
        CHECK_EQ(d0.object, 1);
        CHECK_EQ(d0.phase_prev, std::uint32_t(0));
        CHECK_EQ(d0.phase_next, std::uint32_t(0));
        CHECK_EQ(d0.step, 700);
        CHECK_EQ(d0.param5, 300);
        CHECK_NEAR(d0.box[0], 664.0, 0.0);
        CHECK_NEAR(d0.box[5], 172.0, 0.0);

        const gm::SaveDoor& d20 = state->doors[20];
        CHECK_EQ(d20.flags, 2);
        CHECK_EQ(d20.sector, 48);
        CHECK_EQ(d20.object, 6);
        CHECK_EQ(d20.step, 800);
        CHECK_EQ(d20.n[2], 135);

        const gm::SaveDoor& d24 = state->doors[24];
        CHECK_EQ(d24.flags, 3);
        CHECK_EQ(d24.ids[1], 32);
        CHECK_EQ(d24.sector, 54);
        CHECK_EQ(d24.object, 1);
        CHECK_EQ(d24.step, 800);
        CHECK_EQ(d24.param5, 400);
    }

    // Things are opaque but must be loaded, validated and written back.
    CHECK_EQ(state->things.size(), std::size_t(6));
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK_EQ(state->thing_present[i], true);
    }
    for (std::size_t i = 0; i < state->things.size(); ++i) {
        CHECK_EQ(state->things[i].slot, i);
        CHECK_EQ(state->things[i].raw.size(), gm::kSaveThingRecordSize);
    }
    CHECK_EQ(state->thing_present[6], false);

    // No-op write-back: byte-exact, unknown blocks and header counters intact.
    CHECK(state->update_save(*parsed, &error));
    const std::vector<std::uint8_t> rebuilt = serialize(*parsed);
    CHECK(rebuilt.size() == bytes.size());
    CHECK(rebuilt == bytes);
    CHECK_EQ(fnv1a(rebuilt), golden.fnv);
    if (rebuilt != bytes) {
        const std::size_t limit = std::min(rebuilt.size(), bytes.size());
        for (std::size_t i = 0; i < limit; ++i) {
            if (rebuilt[i] != bytes[i]) {
                std::fprintf(stderr, "FAIL %s: first diff at 0x%zx\n", golden.file, i);
                break;
            }
        }
    }

    std::optional<gm::Save> templated = state->to_save(error);
    CHECK(templated.has_value());
    if (templated) {
        CHECK(serialize(*templated) == bytes);
    }

    // Apply to the runtime player.
    gm::PlayerState player;
    CHECK(state->apply_to_player(player, &error));
    CHECK_NEAR(player.pos.x, golden.position[0], 0.0);
    CHECK_NEAR(player.pos.y, golden.position[1], 0.0);
    CHECK_NEAR(player.pos.z, golden.position[2], 0.0);
    CHECK_NEAR(player.yaw, golden.angles[2], 0.0);
    CHECK_NEAR(player.vel.x, 0.0, 0.0);
    CHECK_EQ(player.on_ground, false);

    // Edits survive a save -> bytes -> parse cycle.
    state->position[0] += 1.25;
    state->ammo[0] += 7;
    state->doors[0].state = 1;
    state->doors[0].phase_next = 32768;
    CHECK(state->update_save(*parsed, &error));
    const std::vector<std::uint8_t> edited = serialize(*parsed);
    std::optional<gm::Save> check = parse_bytes(edited, golden.file);
    if (check) {
        CHECK(check->player.has_value());
        if (check->player) {
            CHECK_NEAR(check->player->position[0], golden.position[0] + 1.25, 0.0);
            CHECK_NEAR(check->player->position[1], golden.position[1], 0.0);
            CHECK_EQ(check->player->ammo[0], golden.ammo_first_two[0] + 7);
            CHECK_EQ(check->player->ammo[1], golden.ammo_first_two[1]);
            CHECK_EQ(check->player->armor, std::int32_t(10000));
        }
        CHECK_EQ(check->doors.size(), std::size_t(25));
        if (check->doors.size() == 25) {
            CHECK_EQ(check->doors[0].state, 1);
            CHECK_EQ(check->doors[0].phase_prev, std::uint32_t(0));
            CHECK_EQ(check->doors[0].phase_next, std::uint32_t(32768));
            CHECK_EQ(check->doors[20].step, 800);
        }
    }
}

// --- real level0 doors -----------------------------------------------------

void test_real_doors(const GoldenSave& golden) {
    if (!fs::exists(kGame / "level0.sbt") || !fs::exists(kGame / "level0.sbw")) {
        std::printf("game_state: level0 not installed, skipping door apply\n");
        return;
    }
    const std::vector<std::uint8_t> save_bytes =
        read_all(kGame / "SAVEGAME" / golden.file);
    const std::vector<std::uint8_t> sbt_bytes = read_all(kGame / "level0.sbt");
    const std::vector<std::uint8_t> sbw_bytes = read_all(kGame / "level0.sbw");
    if (save_bytes.empty() || sbt_bytes.empty() || sbw_bytes.empty()) {
        return;
    }

    std::string error;
    std::optional<gm::Save> parsed = gm::parse_save(save_bytes, error);
    CHECK(parsed.has_value());
    std::optional<gm::SbtLevel> level = gm::parse_sbt(sbt_bytes, error);
    CHECK(level.has_value());
    std::optional<gm::SbwLevel> world_level = gm::parse_sbw(sbw_bytes, error);
    CHECK(world_level.has_value());
    if (!parsed || !level || !world_level) {
        std::fprintf(stderr, "FAIL %s: parse: %s\n", golden.file, error.c_str());
        ++g_failures;
        return;
    }

    gm::WorldState world;
    CHECK(world.build(*level, *world_level, &error));
    CHECK_EQ(world.doors().size(), std::size_t(25));

    auto state = gm::GameState::from_save(*parsed, "level0", error);
    CHECK(state.has_value());
    if (!state) {
        std::fprintf(stderr, "FAIL %s: from_save: %s\n", golden.file, error.c_str());
        ++g_failures;
        return;
    }

    CHECK_EQ(state->apply_to_doors(world, &error), std::size_t(25));
    if (world.doors().size() == 25) {
        CHECK_EQ(world.doors()[0].sector, 4);
        CHECK_EQ(world.doors()[0].object, 1);
        CHECK_EQ(world.doors()[0].id, 2);
        CHECK_EQ(world.doors()[0].flags, 3);
        CHECK_EQ(world.doors()[0].kind, 3);
        CHECK_EQ(world.doors()[0].step, 700);
        CHECK_EQ(world.doors()[0].state, gm::DoorState::Closed);
        CHECK_EQ(world.doors()[0].phase, std::uint32_t(0));
        CHECK_EQ(world.doors()[20].sector, 48);
        CHECK_EQ(world.doors()[20].object, 6);
        CHECK_EQ(world.doors()[20].step, 800);
        CHECK_EQ(world.doors()[24].sector, 54);
        CHECK_EQ(world.doors()[24].object, 1);
        CHECK_EQ(world.doors()[24].step, 800);
    }

    // A saved half-open door lands on the runtime door with the same address.
    state->doors[0].state = 1;
    state->doors[0].phase_next = 32768;
    CHECK_EQ(state->apply_to_doors(world, &error), std::size_t(25));
    if (world.doors().size() == 25) {
        CHECK_EQ(world.doors()[0].state, gm::DoorState::Opening);
        CHECK_EQ(world.doors()[0].phase, std::uint32_t(32768));
        CHECK_NEAR(world.doors()[0].open_fraction, 0.5, 0.0);
        CHECK_EQ(world.doors()[1].state, gm::DoorState::Closed);
    }
}

// --- negatives -------------------------------------------------------------

void test_negatives() {
    std::string error;

    // Broken container: truncation and an unknown block id both stop at
    // parse_save, before a GameState can exist.
    {
        std::string parse_error;
        std::vector<std::uint8_t> broken(100, 0);
        CHECK(!gm::parse_save(broken, parse_error).has_value());
        CHECK(!parse_error.empty());
    }
    {
        const std::vector<std::uint8_t> bytes = read_all(kGame / "SAVEGAME" / "saved0.sbt");
        if (!bytes.empty()) {
            std::string parse_error;
            std::optional<gm::Save> save = gm::parse_save(bytes, parse_error);
            CHECK(save.has_value());
            if (save) {
                std::vector<std::uint8_t> broken = bytes;
                broken[save->blocks.front().offset] = 0x77; // unknown block id
                parse_error.clear();
                CHECK(!gm::parse_save(broken, parse_error).has_value());
                CHECK(parse_error.find("unknown record type 119") != std::string::npos);
            }
        }
    }

    // Save without block 9.
    {
        gm::Save save = make_synthetic_save();
        save.player.reset();
        for (auto it = save.blocks.begin(); it != save.blocks.end(); ++it) {
            if (it->id == 9) {
                save.blocks.erase(it);
                break;
            }
        }
        std::vector<std::uint8_t> bytes = serialize(save);
        std::optional<gm::Save> parsed = parse_bytes(bytes, "no player");
        if (parsed) {
            CHECK_EQ(parsed->player.has_value(), false);
            error.clear();
            CHECK(!gm::GameState::from_save(*parsed, "LEVEL0", error).has_value());
            CHECK(error.find("no player") != std::string::npos);
        }
    }

    // Player block present but no present slot.
    {
        gm::Save save = make_synthetic_save();
        save.player = gm::SavePlayer{};
        std::vector<std::uint8_t> bytes = serialize(save);
        std::optional<gm::Save> parsed = parse_bytes(bytes, "empty player");
        if (parsed) {
            CHECK(parsed->player.has_value());
            if (parsed->player) {
                CHECK_EQ(parsed->player->has_slot, false);
            }
            error.clear();
            CHECK(!gm::GameState::from_save(*parsed, "LEVEL0", error).has_value());
            CHECK(error.find("present slot") != std::string::npos);
        }
    }

    // Foreign level id.
    {
        gm::Save save = make_synthetic_save();
        std::vector<std::uint8_t> bytes = serialize(save);
        std::optional<gm::Save> parsed = parse_bytes(bytes, "level");
        if (parsed) {
            error.clear();
            CHECK(!gm::GameState::from_save(*parsed, "level1", error).has_value());
            CHECK(error.find("level mismatch") != std::string::npos);
        }
    }

    // update_save refuses a container whose header claims another level.
    {
        gm::Save save = make_synthetic_save();
        std::vector<std::uint8_t> bytes = serialize(save);
        std::optional<gm::Save> parsed = parse_bytes(bytes, "level update");
        if (parsed) {
            auto state = gm::GameState::from_save(*parsed, "LEVEL0", error);
            CHECK(state.has_value());
            if (state) {
                gm::Save other = *parsed;
                other.header.level_name = "LEVEL1";
                error.clear();
                CHECK(!state->update_save(other, &error));
                CHECK(error.find("does not match") != std::string::npos);
            }
        }
    }

    // Out-of-range player/door values are rejected by validate() and never
    // reach the runtime or the writer.
    gm::Save source = make_synthetic_save();
    std::vector<std::uint8_t> bytes = serialize(source);
    std::optional<gm::Save> parsed = parse_bytes(bytes, "oob");
    if (!parsed) {
        return;
    }
    auto base = gm::GameState::from_save(*parsed, "LEVEL0", error);
    CHECK(base.has_value());
    if (!base) {
        return;
    }

    auto rejects = [&](const gm::GameState& state, const char* needle) {
        std::string message;
        if (state.validate(&message)) {
            std::fprintf(stderr, "FAIL: expected validate() to reject \"%s\"\n", needle);
            ++g_failures;
            return;
        }
        if (message.find(needle) == std::string::npos) {
            std::fprintf(stderr, "FAIL: \"%s\" does not contain \"%s\"\n",
                         message.c_str(), needle);
            ++g_failures;
        }
        gm::Save copy = *parsed;
        std::string write_error;
        CHECK(!state.update_save(copy, &write_error));
    };

    {
        gm::GameState state = *base;
        state.weapon_slot = 12;
        rejects(state, "weapon slot 12");
    }
    {
        gm::GameState state = *base;
        state.shields[0] = state.shield_max + 1;
        rejects(state, "shield 0");
    }
    {
        gm::GameState state = *base;
        state.position[0] = std::numeric_limits<double>::quiet_NaN();
        rejects(state, "not finite");
    }
    {
        gm::GameState state = *base;
        state.doors[0].sector = -1;
        rejects(state, "sector -1");
    }
    {
        gm::GameState state = *base;
        state.doors[0].phase_next = gm::kDoorPhaseFull + 1;
        rejects(state, "phase outside");
    }
    {
        gm::GameState state = *base;
        state.things[0].slot = 9;
        rejects(state, "not present");
    }

    // A state that was never built from a save cannot produce a template.
    {
        gm::GameState empty;
        error.clear();
        CHECK(!empty.to_save(error).has_value());
        CHECK(error.find("not built from a save") != std::string::npos);
        gm::PlayerState player;
        CHECK(!empty.apply_to_player(player, &error));
        gm::WorldState world;
        CHECK_EQ(empty.apply_to_doors(world, &error), std::size_t(0));
    }
}

} // namespace

void test_vitals_bridge() {
    gm::GameState state;
    state.armor_max = 10000;
    state.armor = 7500;
    state.shield_max = 5000;
    state.shields = {1000, 2000, 3000, 4000};

    const gm::Vitals vitals = state.vitals();
    CHECK_EQ(vitals.armor, 7500);
    CHECK_EQ(vitals.armor_max, 10000);
    CHECK_EQ(vitals.shield_max, 5000);
    CHECK_EQ(vitals.shields[0], 1000);
    CHECK_EQ(vitals.shields[3], 4000);
    CHECK_NEAR(vitals.mult_plate, 1.0, 0.0);
    CHECK_NEAR(vitals.mult_hull, 1.0, 0.0);

    gm::Vitals next = vitals;
    next.armor = 2500;
    next.shields[2] = 500;
    std::string error;
    CHECK(state.apply_vitals(next, &error));
    CHECK(error.empty());
    CHECK_EQ(state.armor, 2500);
    CHECK_EQ(state.shields[2], 500);

    gm::Vitals bad = next;
    bad.armor = 20000;
    CHECK(!state.apply_vitals(bad, &error));
    CHECK(!error.empty());
    CHECK_EQ(state.armor, 2500); // rejected update leaves the state intact
}

int main() {
    test_synthetic();
    for (const GoldenSave& golden : kSaves) {
        test_real_save(golden);
    }
    test_real_doors(kSaves[0]);
    test_negatives();
    test_vitals_bridge();

    if (g_failures != 0) {
        std::fprintf(stderr, "game_state: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("game_state: all checks passed\n");
    return 0;
}
