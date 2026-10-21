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
#include "game/runtime.h"

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

// --- runtime fixtures ------------------------------------------------------
// Raw runtime arrays of the `.sbt` (docs/RECORDS.md §3/§4): a 0x20-byte
// SWITCH record and a 0xB8-byte DOOR record, with only the fields
// `WorldState::build` reads pinned.  They keep the runtime tests independent
// of the shipped game data.

std::vector<std::uint8_t> make_switch_payload(const std::vector<int>& targets) {
    std::vector<std::uint8_t> payload(4 + targets.size() * 0x20, 0);
    put_u32(payload, 0, static_cast<std::uint32_t>(targets.size()));
    for (std::size_t k = 0; k < targets.size(); ++k) {
        const std::size_t base = 4 + k * 0x20;
        put_i32(payload, base, 259); // kind, transformed like dm1 (>= 0x101)
        put_i32(payload, base + 4, targets[k]);
        put_i32(payload, base + 8, 128);
        put_i32(payload, base + 12, static_cast<int>(k));
    }
    return payload;
}

std::vector<std::uint8_t> make_runtime_door_payload(int id) {
    constexpr std::size_t kRecord = 0xB8;
    std::vector<std::uint8_t> payload(4 + kRecord, 0);
    put_u32(payload, 0, 1);
    const std::size_t base = 4;
    put_i32(payload, base + 0x68, 1);   // flags
    put_i32(payload, base + 0x6c, id);  // ids[0] = the GENERATE key
    put_i32(payload, base + 0x80, 0);   // state: closed
    put_i32(payload, base + 0x84, 0);   // sector
    put_i32(payload, base + 0x88, 0);   // object
    put_u32(payload, base + 0x8c, 0);   // phase
    put_i32(payload, base + 0x94, 1500); // phase units per tick
    return payload;
}

gm::SbtLevel make_runtime_level(int switch_target, bool with_door) {
    gm::SbtLevel level;
    if (with_door) {
        gm::SbtRecord doors;
        doors.type = 2;
        doors.payload = make_runtime_door_payload(100);
        level.records.push_back(std::move(doors));
    }
    gm::SbtRecord switches;
    switches.type = 1;
    switches.payload = make_switch_payload({switch_target});
    level.records.push_back(std::move(switches));
    return level;
}

/// One sector, one object: enough for the runtime door to bind.  The object
/// needs an empty `.sbw` geometry block because `CollisionWorld::build`
/// requires one geometry per object reference.
gm::SbwLevel make_runtime_geometry() {
    gm::SbwLevel geometry;
    geometry.header.sectors.resize(1);
    geometry.header.sectors[0].objects.resize(1);
    geometry.header.object_count = 1;
    geometry.geometry.push_back(gm::SbwGeometry{});
    return geometry;
}

/// Identity `.sbw` object transform (`test_weapons.cpp`): the header stores
/// origin/axis as four padded `vec3`s at `8 * i + 4`.
void write_identity_object_header(std::array<std::uint8_t, 96>& header) {
    header.fill(0);
    for (int point = 0; point < 4; ++point) {
        for (int component = 0; component < 3; ++component) {
            const float value =
                (point == 0) ? 0.0f : (component == point - 1 ? 1.0f : 0.0f);
            std::memcpy(header.data() + 8 * (point * 3 + component) + 4,
                        &value, sizeof(value));
        }
    }
}

/// Floor at z = 0 (a 400x400 square, two triangles) so a runtime grenade can
/// bounce; the world itself has no doors/lifts (`blast_runtime_level()`).
gm::SbwLevel make_runtime_floor_geometry() {
    gm::SbwLevel geometry;
    geometry.header.sectors.resize(1);
    const double s = 200.0;
    const std::array<gm::Vec3d, 4> corners = {
        gm::Vec3d{-s, s, 0.0}, gm::Vec3d{-s, -s, 0.0},
        gm::Vec3d{s, -s, 0.0}, gm::Vec3d{s, s, 0.0}};
    const int triangles[2][3] = {{0, 1, 2}, {0, 2, 3}};
    for (const auto& triangle : triangles) {
        gm::SbwObjectRef object;
        write_identity_object_header(object.header);
        geometry.header.sectors[0].objects.push_back(object);

        gm::SbwGeometry floor;
        for (int corner : triangle) {
            floor.vertices.push_back(gm::Vec3f{
                static_cast<float>(corners[static_cast<std::size_t>(corner)].x),
                static_cast<float>(corners[static_cast<std::size_t>(corner)].y),
                static_cast<float>(corners[static_cast<std::size_t>(corner)].z)});
        }
        floor.n_verts = 3;
        gm::SbwPoly poly;
        poly.indices = {0, 1, 2};
        floor.polys.push_back(poly);
        floor.n_polys = 1;
        geometry.geometry.push_back(std::move(floor));
    }
    geometry.header.object_count = 2;
    return geometry;
}

/// No DOOR/LIFT records, one SWITCH: `WorldState::build` rejects a completely
/// empty level, and the blast tests do not press anything.
gm::SbtLevel blast_runtime_level() { return make_runtime_level(999, false); }

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

namespace {

// --- fixed-tick runtime (docs/RUNTIME_CPP.md) ------------------------------

// The runtime tests stay self-contained: synthetic SWITCH/DOOR records above,
// things spawned directly through `GameRuntime::spawn_thing`, no game data.

void hash_u64(std::uint64_t& hash, std::uint64_t value) {
    hash ^= value;
    hash *= 0x100000001b3ULL;
}

void hash_double(std::uint64_t& hash, double value) {
    hash_u64(hash, std::bit_cast<std::uint64_t>(value));
}

/// One thing expires at tick 1 with event 77; a second tick must not replay it.
void test_runtime_events_not_duplicated() {
    gm::GameRuntime runtime;
    gm::Thing thing;
    thing.id = -1;
    thing.position = std::array<int, 3>{12, -34, 56};
    thing.expire_ticks = 1;
    thing.expire_event = 77;
    const int id = runtime.spawn_thing(thing);
    CHECK_EQ(id, 0);

    runtime.tick(1);
    CHECK_EQ(runtime.clock(), 1u);
    const gm::EventDispatchResult& first = runtime.last_events();
    CHECK(first.ok);
    CHECK_EQ(first.generated.size(), std::size_t(1));
    CHECK_EQ(first.sounds.size(), std::size_t(0));
    if (first.generated.size() == 1) {
        CHECK_EQ(first.generated[0].id, 77);
        CHECK_EQ(first.generated[0].x, 12);
        CHECK_EQ(first.generated[0].y, -34);
        CHECK_EQ(first.generated[0].z, 56);
        CHECK_EQ(first.generated[0].extra,
                 static_cast<std::uint32_t>(id - 0xfff8));
    }

    // The queue is drained by the same tick; tick 2 posts and dispatches
    // nothing, the expiry latch is still set.
    runtime.tick(1);
    CHECK_EQ(runtime.clock(), 2u);
    CHECK_EQ(runtime.last_events().generated.size(), std::size_t(0));
    CHECK_EQ(runtime.events().queue().size(), std::size_t(0));
    const gm::Thing* stored = runtime.things().by_id(id);
    CHECK(stored != nullptr);
    if (stored != nullptr) {
        CHECK(stored->expired);
        CHECK_EQ(stored->age, 2u);
    }

    // A zero-tick call is a no-op, not an extra pass.
    runtime.tick(0);
    CHECK_EQ(runtime.clock(), 2u);
    CHECK_EQ(runtime.last_events().generated.size(), std::size_t(0));
}

/// Expire and remove of a projectile both reach `last_projectile_results()`;
/// the dispatch sees the expire event exactly once.
void test_runtime_projectile_results() {
    gm::GameRuntime runtime;
    gm::ProjectileSpawn spawn;
    spawn.thing_id = 5;
    spawn.position = gm::Vec3d{0.0, 0.0, 100.0};
    spawn.direction = gm::Vec3d{1.0, 0.0, 0.0};
    spawn.speed = 1.0;
    spawn.life_ticks = 1;
    spawn.expire_event = 500;
    spawn.radius = 0.0; // no collision world, no sweep
    CHECK_EQ(runtime.spawn_projectile(spawn), 0);

    runtime.tick(1);
    {
        const std::span<const gm::ProjectileTickRecord> records =
            runtime.last_projectile_results();
        CHECK_EQ(records.size(), std::size_t(1));
        if (records.size() == 1) {
            CHECK(records[0].status == gm::ProjectileStatus::expired);
            CHECK(records[0].spent);
            CHECK(!records[0].dead);
            CHECK(!records[0].already_spent);
            CHECK_EQ(records[0].posted_events, std::size_t(1));
            CHECK_EQ(records[0].events.size(), std::size_t(1));
            if (records[0].events.size() == 1) {
                CHECK_EQ(records[0].events[0].id, 500u);
            }
        }
        CHECK_EQ(runtime.last_events().generated.size(), std::size_t(1));
        if (!runtime.last_events().generated.empty()) {
            CHECK_EQ(runtime.last_events().generated[0].id, 500);
        }
    }

    // The spent record is stepped again but reports a replay and posts nothing.
    runtime.tick(1);
    {
        const std::span<const gm::ProjectileTickRecord> records =
            runtime.last_projectile_results();
        CHECK_EQ(records.size(), std::size_t(1));
        if (records.size() == 1) {
            CHECK(records[0].already_spent);
            CHECK(records[0].events.empty());
            CHECK_EQ(records[0].posted_events, std::size_t(0));
        }
        CHECK_EQ(runtime.last_events().generated.size(), std::size_t(0));
    }

    // Removal marks the slot destroyed; the spawn has no destroy event.
    CHECK(runtime.remove_projectile(5));
    runtime.tick(1);
    {
        const std::span<const gm::ProjectileTickRecord> records =
            runtime.last_projectile_results();
        CHECK_EQ(records.size(), std::size_t(1)); // the slot is kept
        if (records.size() == 1) {
            CHECK(records[0].status == gm::ProjectileStatus::destroyed);
            CHECK(records[0].dead);
            CHECK(records[0].already_spent);
            CHECK(records[0].events.empty());
        }
        CHECK_EQ(runtime.last_events().generated.size(), std::size_t(0));
    }
    CHECK_EQ(runtime.projectiles().list().size(), std::size_t(1));

    // `clear_projectiles()` drops the fleet and the last result.
    runtime.clear_projectiles();
    CHECK_EQ(runtime.projectiles().list().size(), std::size_t(0));
    CHECK_EQ(runtime.last_projectile_results().size(), std::size_t(0));
}

/// grenade-like weapon for the blast bridge: the impact branch deals 1000 hull
/// (event 600), the expire branch 2000 (event 601), so a test can tell which
/// `Weapon` vector reached `apply_projectile_blast()`.
gm::Weapon blast_test_weapon() {
    gm::Weapon weapon;
    weapon.name = "GRENADE";
    gm::Blast impact;
    impact.hull = 1000;
    impact.radius = 50;
    impact.event = 600;
    weapon.impact_blasts.push_back(impact);
    gm::Blast expire = impact;
    expire.hull = 2000;
    expire.event = 601;
    weapon.expire_blasts.push_back(expire);
    return weapon;
}

/// Grenade blast bridge: `expire` blasts the player and the thing exactly once,
/// a second call without a new tick is a no-op, and the blast events go through
/// the runtime queue (the next tick dispatches them once).
void test_runtime_projectile_blasts_expire() {
    gm::GameRuntime runtime;

    gm::Thing crate;
    crate.id = -1;
    crate.position = std::array<int, 3>{0, 0, 20};
    crate.max_health = 10000;
    crate.health = 10000;
    CHECK_EQ(runtime.spawn_thing(crate), 0);

    const gm::Vec3d eye{0.0, 0.0, 20.0};
    CHECK(runtime.set_player_eyes(std::span<const gm::Vec3d>(&eye, 1)));

    gm::ProjectileSpawn grenade;
    grenade.thing_id = 40;
    grenade.position = gm::Vec3d{0.0, 0.0, 20.0};
    grenade.direction = gm::Vec3d{0.0, 0.0, 1.0};
    grenade.speed = 0.0;    // stays at the blast centre
    grenade.life_ticks = 1; // expires on the first step
    grenade.radius = 0.0;
    CHECK_EQ(runtime.spawn_projectile(grenade), 0);

    std::vector<gm::Vitals> players(1);
    players[0].armor = 10000;
    players[0].armor_max = 10000;
    std::vector<gm::ThingVitals> things;
    for (const gm::Thing& thing : runtime.things().things()) {
        things.push_back(gm::thing_vitals(thing));
    }

    runtime.tick(1);
    {
        const std::span<const gm::ProjectileTickRecord> records =
            runtime.last_projectile_results();
        CHECK_EQ(records.size(), std::size_t(1));
        if (records.size() == 1) {
            CHECK(records[0].status == gm::ProjectileStatus::expired);
            CHECK(records[0].spent);
            CHECK(!records[0].already_spent);
        }
    }
    CHECK_EQ(runtime.events().queue().size(), std::size_t(0));

    const gm::Weapon weapon = blast_test_weapon();
    const gm::ProjectileBlastContext context;
    const gm::ProjectileBlastBatch first =
        runtime.apply_projectile_blasts(weapon, context, players, things);
    CHECK_EQ(first.blasts, std::size_t(1));
    CHECK_EQ(first.already_applied, std::size_t(0));
    CHECK_EQ(first.posted_events, std::size_t(2));
    CHECK_EQ(first.damage.hits.size(), std::size_t(2)); // player 0 + thing 0
    if (first.damage.hits.size() == 2) {
        CHECK(first.damage.hits[0].hit.is_player);
        CHECK_EQ(first.damage.hits[0].hit.index, 0);
        CHECK_EQ(first.damage.hits[0].hit.hull, 2000); // expire branch
        CHECK(!first.damage.hits[1].hit.is_player);
        CHECK_EQ(first.damage.hits[1].hit.index, 0);
    }
    CHECK_EQ(players[0].armor, 8000); // 10000 - 2000
    CHECK_EQ(things[0].hp, 8000);
    // Two type-3 records (one per hit) wait for the next tick's dispatch.
    CHECK_EQ(runtime.events().queue().size(),
             std::size_t(2) * gm::EventQueue::kType3Size);

    // The same generation is never blasted twice.
    const gm::ProjectileBlastBatch repeat =
        runtime.apply_projectile_blasts(weapon, context, players, things);
    CHECK_EQ(repeat.blasts, std::size_t(0));
    CHECK_EQ(repeat.already_applied, std::size_t(1));
    CHECK_EQ(repeat.posted_events, std::size_t(0));
    CHECK(repeat.damage.hits.empty());
    CHECK_EQ(players[0].armor, 8000);
    CHECK_EQ(things[0].hp, 8000);
    CHECK_EQ(runtime.events().queue().size(),
             std::size_t(2) * gm::EventQueue::kType3Size);

    // The next tick dispatches the queued blast events exactly once.
    runtime.tick(1);
    CHECK_EQ(runtime.clock(), 2u);
    CHECK_EQ(runtime.last_events().generated.size(), std::size_t(2));
    for (const gm::Event& event : runtime.last_events().generated) {
        CHECK_EQ(event.id, 601);
    }
    CHECK_EQ(runtime.events().queue().size(), std::size_t(0));

    // A new generation replays the spent expiry; that is not a trigger, so no
    // second blast (the mark is fresh but the record is `already_spent`).
    const std::span<const gm::ProjectileTickRecord> replay_records =
        runtime.last_projectile_results();
    CHECK_EQ(replay_records.size(), std::size_t(1));
    if (replay_records.size() == 1) {
        CHECK(replay_records[0].already_spent);
    }
    const gm::ProjectileBlastBatch replay =
        runtime.apply_projectile_blasts(weapon, context, players, things);
    CHECK_EQ(replay.blasts, std::size_t(0));
    CHECK_EQ(replay.already_applied, std::size_t(0));
    CHECK(replay.damage.hits.empty());
    CHECK_EQ(players[0].armor, 8000);
    CHECK_EQ(things[0].hp, 8000);
}

/// Grenade bounce: the `impact` branch blasts once per bounce, never while the
/// round is flying, and a second (new) bounce on a later tick blasts again.
void test_runtime_projectile_blasts_bounce() {
    gm::GameRuntime runtime;
    std::string error;
    CHECK(runtime.build_world(blast_runtime_level(),
                              make_runtime_floor_geometry(), &error));
    CHECK(error.empty());

    gm::Thing crate;
    crate.id = -1;
    crate.position = std::array<int, 3>{30, 0, 10};
    crate.max_health = 10000;
    crate.health = 10000;
    CHECK_EQ(runtime.spawn_thing(crate), 0);

    const gm::Vec3d eye{0.0, 0.0, 10.0};
    CHECK(runtime.set_player_eyes(std::span<const gm::Vec3d>(&eye, 1)));

    gm::ProjectileSpawn grenade;
    grenade.thing_id = 41;
    grenade.position = gm::Vec3d{0.0, 0.0, 25.0};
    grenade.direction = gm::Vec3d{0.0, 0.0, -1.0};
    grenade.speed = 2.0;
    grenade.gravity = 1.0;
    grenade.level_gravity = 1.0;
    grenade.damping = 0.8;
    grenade.radius = 10.0; // contacts the floor at centre z = 10
    CHECK_EQ(runtime.spawn_projectile(grenade), 0);

    std::vector<gm::Vitals> players(1);
    players[0].armor = 10000;
    players[0].armor_max = 10000;
    std::vector<gm::ThingVitals> things;
    for (const gm::Thing& thing : runtime.things().things()) {
        things.push_back(gm::thing_vitals(thing));
    }

    const gm::Weapon weapon = blast_test_weapon();
    const gm::ProjectileBlastContext context;
    const auto tick_until_bounce = [&runtime](int max_ticks) {
        for (int i = 0; i < max_ticks; ++i) {
            runtime.tick(1);
            for (const gm::ProjectileTickRecord& record :
                 runtime.last_projectile_results()) {
                if (record.status == gm::ProjectileStatus::bounced) {
                    return true;
                }
            }
        }
        return false;
    };

    CHECK(tick_until_bounce(40));
    {
        const std::span<const gm::ProjectileTickRecord> records =
            runtime.last_projectile_results();
        CHECK_EQ(records.size(), std::size_t(1));
        if (records.size() == 1) {
            CHECK(records[0].status == gm::ProjectileStatus::bounced);
        }
    }

    const gm::ProjectileBlastBatch first =
        runtime.apply_projectile_blasts(weapon, context, players, things);
    CHECK_EQ(first.blasts, std::size_t(1));
    CHECK_EQ(first.already_applied, std::size_t(0));
    CHECK_EQ(first.posted_events, std::size_t(2));
    CHECK_EQ(first.damage.hits.size(), std::size_t(2));
    if (first.damage.hits.size() == 2) {
        CHECK_EQ(first.damage.hits[0].hit.hull, 1000); // impact branch, d=0
        // 30 units from the centre, radius 50: falloff to 1000*(50-30)/50.
        CHECK_EQ(first.damage.hits[1].hit.hull, 400);
    }
    CHECK_EQ(players[0].armor, 9000); // 10000 - 1000
    CHECK_EQ(things[0].hp, 9600);     // 10000 - 400
    CHECK_EQ(runtime.events().queue().size(),
             std::size_t(2) * gm::EventQueue::kType3Size);

    // A flying step between two impacts must not blast.
    runtime.tick(1);
    {
        const std::span<const gm::ProjectileTickRecord> records =
            runtime.last_projectile_results();
        CHECK_EQ(records.size(), std::size_t(1));
        if (records.size() == 1) {
            CHECK(records[0].status == gm::ProjectileStatus::flying);
        }
    }
    const gm::ProjectileBlastBatch flying =
        runtime.apply_projectile_blasts(weapon, context, players, things);
    CHECK_EQ(flying.blasts, std::size_t(0));
    CHECK(flying.damage.hits.empty());
    CHECK_EQ(players[0].armor, 9000);
    CHECK_EQ(things[0].hp, 9600);

    // Gravity brings the round back down: the new bounce of a new tick is a
    // new generation and blasts again (once).
    CHECK(tick_until_bounce(200));
    const gm::ProjectileBlastBatch second =
        runtime.apply_projectile_blasts(weapon, context, players, things);
    CHECK_EQ(second.blasts, std::size_t(1));
    CHECK_EQ(second.already_applied, std::size_t(0));
    CHECK_EQ(second.damage.hits.size(), std::size_t(2));
    CHECK_EQ(players[0].armor, 8000);
    CHECK_EQ(things[0].hp, 9200);

    const gm::ProjectileBlastBatch second_repeat =
        runtime.apply_projectile_blasts(weapon, context, players, things);
    CHECK_EQ(second_repeat.blasts, std::size_t(0));
    CHECK_EQ(second_repeat.already_applied, std::size_t(1));
    CHECK_EQ(players[0].armor, 8000);
    CHECK_EQ(things[0].hp, 9200);
}

/// Filling the staging queue must leave the unposted switch tail in the world
/// and the next tick must dispatch it exactly once.
void test_runtime_pending_switch_tail() {
    const gm::SbtLevel level = make_runtime_level(999, false);
    const gm::SbwLevel geometry;
    gm::GameRuntime runtime;
    std::string error;
    CHECK(runtime.build_world(level, geometry, &error));
    CHECK(error.empty());
    CHECK(runtime.has_world());
    CHECK_EQ(runtime.world().switches().size(), std::size_t(1));

    // Type-2 records are 7 bytes and the queue threshold is 0x7526, so the
    // 4280th press fills it and every later record stays pending.
    constexpr int kPresses = 4300;
    for (int i = 0; i < kPresses; ++i) {
        CHECK(runtime.press_switch(0, 7));
    }
    CHECK_EQ(runtime.world().stats().switch_interactions, std::size_t(kPresses));
    CHECK(runtime.events().queue().size() > 0);

    runtime.tick(1);
    const std::size_t first = runtime.last_events().generated.size();
    CHECK(first > 0);
    CHECK(first < static_cast<std::size_t>(kPresses)); // a tail is pending
    for (const gm::Event& event : runtime.last_events().generated) {
        CHECK_EQ(event.id, 999);
        CHECK_EQ(event.extra, static_cast<std::uint32_t>(-0x10000 + 7));
    }

    // `WorldState::tick` keeps the blocked tail; the second dispatch carries
    // the rest, so no press is lost and none is duplicated.
    runtime.tick(1);
    const std::size_t second = runtime.last_events().generated.size();
    CHECK(second > 0);
    CHECK_EQ(first + second, static_cast<std::size_t>(kPresses));
    for (const gm::Event& event : runtime.last_events().generated) {
        CHECK_EQ(event.id, 999);
    }

    runtime.tick(1);
    CHECK_EQ(runtime.last_events().generated.size(), std::size_t(0));
    CHECK_EQ(runtime.events().queue().size(), std::size_t(0));
}

/// One AI agent that can fire: its thing carries a single weapon table entry
/// (event 41, band 0..200, 91-tick cooldown) and the player eye stands 60
/// units away.  The agent itself never moves (A2/A3 off).
gm::Thing make_ai_shooter() {
    gm::Thing shooter;
    shooter.id = -1;
    shooter.position = std::array<int, 3>{0, 0, 0};
    shooter.radius = 4.0;
    shooter.model = "Cultist.sob";
    shooter.proximity.push_back(
        gm::ProximityEntry{41, 0, 200, 91, {1.0, 2.0, 3.0}});
    return shooter;
}

/// Movement-type 6 projectile type used as the AI bridge template.  Its `id`
/// is deliberately a value the runtime must not reuse.
gm::Thing ai_projectile_template() {
    gm::Thing projectile;
    projectile.id = 7;
    projectile.movement_type = 6;
    projectile.speed = 10.0;
    projectile.radius = 0.0;
    projectile.hits_left = 0;
    projectile.expire_ticks = 0;
    projectile.create_event = 900;
    projectile.expire_event = 901;
    projectile.destroy_event = 902;
    projectile.collide_event = 903;
    return projectile;
}

/// AI config of the bridge tests: weapon table + shot queue on, 910 units/s
/// (10 units/tick after the /91 conversion), bridge flag from the argument.
gm::GameRuntimeConfig ai_bridge_config(bool bridge) {
    gm::GameRuntimeConfig config;
    config.ai.use_weapon_table = true;
    config.ai.use_weapon_shots = true;
    config.ai.weapon_shot_speed = 910.0;
    config.ai_shots_to_projectiles = bridge;
    return config;
}

/// Spawn the shooter, attach the AI and put the player 60 units in front.
void ai_bridge_setup(gm::GameRuntime& runtime) {
    CHECK_EQ(runtime.spawn_thing(make_ai_shooter()), 0);
    CHECK_EQ(runtime.attach_ai(), std::size_t(1));
    const gm::Vec3d eye{0.0, 60.0, 10.0};
    CHECK(runtime.set_player_eyes(std::span<const gm::Vec3d>(&eye, 1)));
}

/// Tick until the AI queues its first weapon shot; returns the 1-based tick
/// number or 0 when none appeared within `max_ticks`.
int ai_tick_until_first_shot(gm::GameRuntime& runtime, int max_ticks) {
    for (int i = 0; i < max_ticks; ++i) {
        runtime.tick(1);
        if (runtime.ai().stats().weapon_shots > 0) {
            return i + 1;
        }
    }
    return 0;
}

/// Default `ai_shots_to_projectiles = false`: the AI still queues shots, but
/// the runtime spawns nothing and does not consume the queue (the caller owns
/// it), and the template is not applied.
void test_runtime_ai_shots_default_off() {
    gm::GameRuntime runtime(ai_bridge_config(false));
    ai_bridge_setup(runtime);
    runtime.set_ai_projectile_template(ai_projectile_template());
    CHECK(!runtime.config().ai_shots_to_projectiles);
    CHECK(runtime.ai_projectile_template().has_value());

    const int fired = ai_tick_until_first_shot(runtime, 140);
    CHECK(fired > 0);
    CHECK_EQ(runtime.ai().stats().weapon_shots, 1u);
    CHECK_EQ(runtime.ai().weapon_shots().size(), std::size_t(1));
    CHECK_EQ(runtime.projectiles().list().size(), std::size_t(0));
    CHECK_EQ(runtime.last_projectile_results().size(), std::size_t(0));
    CHECK_EQ(runtime.last_events().generated.size(), std::size_t(0));
    for (const gm::Event& event : runtime.last_events().generated) {
        CHECK(event.id != 900); // no projectile create event either
    }

    // A following tick without a new weapon event is still a no-op.
    runtime.tick(1);
    CHECK_EQ(runtime.projectiles().list().size(), std::size_t(0));
}

/// Opt-in bridge: one round per weapon event, spawned in the same tick, with a
/// runtime id, its create event dispatched once and no duplicate on re-ticks.
void test_runtime_ai_shots_to_projectiles() {
    gm::GameRuntime runtime(ai_bridge_config(true));
    ai_bridge_setup(runtime);
    runtime.set_ai_projectile_template(ai_projectile_template());
    CHECK(runtime.config().ai_shots_to_projectiles);
    const gm::Thing* stored = runtime.ai_projectile_template().has_value()
                                  ? &*runtime.ai_projectile_template()
                                  : nullptr;
    CHECK(stored != nullptr);
    if (stored != nullptr) {
        CHECK_EQ(stored->id, 7); // the setter keeps the value; spawns override it
        CHECK_EQ(stored->create_event, 900u);
    }

    const int fired = ai_tick_until_first_shot(runtime, 140);
    CHECK(fired > 0);
    CHECK_EQ(runtime.ai().stats().weapon_shots, 1u);
    CHECK_EQ(runtime.ai().weapon_shots().size(), std::size_t(0)); // drained
    CHECK_EQ(runtime.projectiles().list().size(), std::size_t(1));

    // The round was spawned after the AI phase and before the projectile
    // phase of the same tick, so it is already in the last results.
    {
        const std::span<const gm::ProjectileTickRecord> records =
            runtime.last_projectile_results();
        CHECK_EQ(records.size(), std::size_t(1));
        if (records.size() == 1) {
            CHECK(records[0].thing_id != 7);
            CHECK(records[0].thing_id >= gm::kAiProjectileRuntimeIdBase);
            CHECK(records[0].status == gm::ProjectileStatus::flying);
            CHECK(!records[0].spent);
            CHECK(!records[0].dead);
            CHECK_NEAR(records[0].speed, 10.0, 1e-12); // 910 / 91
        }
    }
    // Exactly one create event (900) of the round: the queue is empty after
    // the same tick's dispatch.
    CHECK_EQ(runtime.last_events().generated.size(), std::size_t(1));
    if (runtime.last_events().generated.size() == 1) {
        CHECK_EQ(runtime.last_events().generated[0].id, 900);
    }
    CHECK_EQ(runtime.events().queue().size(), std::size_t(0));

    // A tick without a weapon event spawns nothing and replays no event.
    runtime.tick(1);
    CHECK_EQ(runtime.ai().stats().weapon_shots, 1u);
    CHECK_EQ(runtime.projectiles().list().size(), std::size_t(1));
    CHECK_EQ(runtime.last_projectile_results().size(), std::size_t(1));
    CHECK_EQ(runtime.last_events().generated.size(), std::size_t(0));
    CHECK_EQ(runtime.events().queue().size(), std::size_t(0));

    // The second weapon event (91-tick cooldown) spawns a second round: the
    // template id is never reused, so both rounds stay alive and distinct.
    int second = 0;
    for (int i = 0; i < 200 && second == 0; ++i) {
        runtime.tick(1);
        if (runtime.ai().stats().weapon_shots >= 2) {
            second = i + 1;
        }
    }
    CHECK(second > 0);
    CHECK_EQ(runtime.projectiles().list().size(), std::size_t(2));
    CHECK_EQ(runtime.ai().weapon_shots().size(), std::size_t(0));
    CHECK_EQ(runtime.last_events().generated.size(), std::size_t(1));
    if (!runtime.last_events().generated.empty()) {
        CHECK_EQ(runtime.last_events().generated[0].id, 900);
    }
    CHECK_EQ(runtime.events().queue().size(), std::size_t(0));
    const std::vector<gm::ProjectileState>& fleet = runtime.projectiles().list();
    if (fleet.size() == 2) {
        CHECK(fleet[0].spawn.thing_id != fleet[1].spawn.thing_id);
        CHECK(fleet[0].spawn.thing_id >= gm::kAiProjectileRuntimeIdBase);
        CHECK(fleet[1].spawn.thing_id >= gm::kAiProjectileRuntimeIdBase);
        CHECK(!fleet[0].dead);
        CHECK(!fleet[1].dead);
    }
    CHECK_EQ(runtime.last_projectile_results().size(), std::size_t(2));
}

/// Bridge flag on but no template: nothing spawns and the queue is kept, so
/// setting the template later still turns the pending shot into a round.
void test_runtime_ai_shots_without_template() {
    gm::GameRuntime runtime(ai_bridge_config(true));
    ai_bridge_setup(runtime);
    CHECK(!runtime.ai_projectile_template().has_value());

    const int fired = ai_tick_until_first_shot(runtime, 140);
    CHECK(fired > 0);
    CHECK_EQ(runtime.projectiles().list().size(), std::size_t(0));
    CHECK_EQ(runtime.last_projectile_results().size(), std::size_t(0));
    CHECK_EQ(runtime.ai().weapon_shots().size(), std::size_t(1));
    CHECK_EQ(runtime.last_events().generated.size(), std::size_t(0));

    // The pending shot is not lost: the next tick with a template spawns it.
    runtime.set_ai_projectile_template(ai_projectile_template());
    runtime.tick(1);
    CHECK_EQ(runtime.ai().weapon_shots().size(), std::size_t(0));
    CHECK_EQ(runtime.projectiles().list().size(), std::size_t(1));
    CHECK_EQ(runtime.last_projectile_results().size(), std::size_t(1));
    CHECK_EQ(runtime.last_events().generated.size(), std::size_t(1));

    // Clearing the template stops the bridge again.
    runtime.clear_ai_projectile_template();
    CHECK(!runtime.ai_projectile_template().has_value());
    runtime.tick(1);
    CHECK_EQ(runtime.projectiles().list().size(), std::size_t(1));
}

/// Build one runtime with a door, two things, a player eye and a projectile,
/// then press the door switch.
void runtime_setup(gm::GameRuntime& runtime, const gm::SbtLevel& level,
                   const gm::SbwLevel& geometry) {
    std::string error;
    CHECK(runtime.build_world(level, geometry, &error));
    CHECK(error.empty());

    gm::Thing expiring;
    expiring.id = -1;
    expiring.position = std::array<int, 3>{10, 20, 30};
    expiring.radius = 4.0;
    expiring.expire_ticks = 5;
    expiring.expire_event = 42;
    CHECK_EQ(runtime.spawn_thing(expiring), 0);

    gm::Thing idle;
    idle.id = -1;
    idle.position = std::array<int, 3>{110, 20, 30};
    idle.radius = 4.0;
    idle.model = "Cultist.sob";
    CHECK_EQ(runtime.spawn_thing(idle), 1);

    CHECK_EQ(runtime.attach_ai(), std::size_t(2));
    const gm::Vec3d eye{11.0, 21.0, 31.0};
    CHECK(runtime.set_player_eyes(std::span<const gm::Vec3d>(&eye, 1)));

    gm::ProjectileSpawn spawn;
    spawn.thing_id = 9;
    spawn.position = gm::Vec3d{0.0, 0.0, 64.0};
    spawn.direction = gm::Vec3d{1.0, 0.0, 0.0};
    spawn.speed = 2.0;
    spawn.life_ticks = 7;
    spawn.expire_event = 500;
    CHECK_EQ(runtime.spawn_projectile(spawn), 0);

    CHECK(runtime.press_switch(0, 3));
}

/// Per-tick hash of everything the runtime owns that this slice changes.
/// Doubles are hashed by their bit pattern, so the comparison is exact.
std::vector<std::uint64_t> runtime_trace(gm::GameRuntime& runtime, int ticks) {
    std::vector<std::uint64_t> trace;
    trace.reserve(static_cast<std::size_t>(ticks));
    for (int i = 0; i < ticks; ++i) {
        runtime.tick(1);
        std::uint64_t hash = 0xcbf29ce484222325ULL;
        hash_u64(hash, runtime.clock());
        hash_u64(hash, runtime.last_events().generated.size());
        for (const gm::Event& event : runtime.last_events().generated) {
            hash_u64(hash, static_cast<std::uint32_t>(event.id));
            hash_u64(hash, event.extra);
        }
        hash_u64(hash, runtime.last_projectile_results().size());
        for (const gm::ProjectileTickRecord& record : runtime.last_projectile_results()) {
            hash_u64(hash, static_cast<std::uint64_t>(record.status));
            hash_u64(hash, record.spent ? 1u : 0u);
            hash_u64(hash, record.dead ? 1u : 0u);
            hash_double(hash, record.position.x);
            hash_double(hash, record.position.y);
            hash_double(hash, record.position.z);
            hash_double(hash, record.speed);
            hash_u64(hash, static_cast<std::uint64_t>(record.life_left));
        }
        for (const gm::Thing& thing : runtime.things().things()) {
            hash_u64(hash, thing.age);
            hash_u64(hash, thing.expired ? 1u : 0u);
            hash_u64(hash, thing.alive ? 1u : 0u);
        }
        for (const gm::Door& door : runtime.world().doors()) {
            hash_u64(hash, door.phase);
            hash_u64(hash, static_cast<std::uint64_t>(static_cast<int>(door.state)));
            hash_double(hash, door.open_fraction);
        }
        hash_u64(hash, runtime.world().stats().ticks);
        hash_u64(hash, runtime.ai().agents().size());
        for (const gm::AiAgent& agent : runtime.ai().agents()) {
            hash_u64(hash, static_cast<std::uint64_t>(agent.state));
            hash_double(hash, agent.position.x);
            hash_double(hash, agent.position.y);
            hash_double(hash, agent.position.z);
            hash_double(hash, agent.timer);
        }
        trace.push_back(hash);
    }
    return trace;
}

/// Same build + same steps -> the same per-tick state hash, and `reset()`
/// reproduces the run from scratch.
void test_runtime_determinism() {
    const gm::SbtLevel level = make_runtime_level(100, true);
    const gm::SbwLevel geometry = make_runtime_geometry();

    gm::GameRuntime first;
    runtime_setup(first, level, geometry);
    const std::vector<std::uint64_t> run_a = runtime_trace(first, 12);
    CHECK_EQ(first.world().doors().size(), std::size_t(1));
    if (!first.world().doors().empty()) {
        // The switch opened the door; 12 ticks move it by 12 * 1500.
        CHECK_EQ(first.world().doors()[0].phase, 18000u);
    }

    gm::GameRuntime second;
    runtime_setup(second, level, geometry);
    const std::vector<std::uint64_t> run_b = runtime_trace(second, 12);
    CHECK(run_a == run_b);

    first.reset();
    CHECK_EQ(first.clock(), 0u);
    CHECK(!first.has_world());
    CHECK(!first.has_things());
    CHECK_EQ(first.world().doors().size(), std::size_t(0));
    CHECK_EQ(first.things().things().size(), std::size_t(0));
    CHECK_EQ(first.projectiles().list().size(), std::size_t(0));
    CHECK_EQ(first.last_projectile_results().size(), std::size_t(0));

    runtime_setup(first, level, geometry);
    const std::vector<std::uint64_t> run_c = runtime_trace(first, 12);
    CHECK(run_a == run_c);
}

} // namespace

int main() {
    test_synthetic();
    for (const GoldenSave& golden : kSaves) {
        test_real_save(golden);
    }
    test_real_doors(kSaves[0]);
    test_negatives();
    test_vitals_bridge();
    test_runtime_events_not_duplicated();
    test_runtime_projectile_results();
    test_runtime_projectile_blasts_expire();
    test_runtime_projectile_blasts_bounce();
    test_runtime_ai_shots_default_off();
    test_runtime_ai_shots_to_projectiles();
    test_runtime_ai_shots_without_template();
    test_runtime_pending_switch_tail();
    test_runtime_determinism();

    if (g_failures != 0) {
        std::fprintf(stderr, "game_state: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("game_state: all checks passed\n");
    return 0;
}
