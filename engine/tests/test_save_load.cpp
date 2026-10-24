// Save -> live runtime (`M8.7`/`M9`, slice 2; docs/SAVE_LOAD.md).
//
// The shipped `SAVEGAME/saved0.sbt`/`saved1.sbt` are parsed and applied to a
// real `level0` runtime + `SimPlayer`:
//
//   1. the saved position/yaw land in `SimPlayer::state()` (bit for bit) and
//      the saved gauges/loadout land in the *runtime* (`player_vitals()`,
//      `player_arsenal()`), not just in the model;
//   2. block 8 `SECTOR_STUFF` reaches `VmState::sectors` through the proven
//      loader `vm_load_sector_stuff()` (`0x46b7b0`): 256 slots, 13 active,
//      field for field equal to the save;
//   3. the header level name is really loadable: `level0` gives 58 sectors,
//      192 objects, 25 doors, 24 switches and 121 runtime things;
//   4. 60 fixed ticks are deterministic (two fresh runtimes -> equal digest)
//      and the clock/state really advances;
//   5. a save with `armor == 0` loads as a dead player (`alive() == false`);
//   6. negatives: a truncated save, a missing level `.sbt`, a missing
//      `saved19.sbt`, slot 99, a save without block 9 / without a present
//      slot, out-of-range gauges and weapon slot 12 are refused with a text.
//
// Nothing is written to the install: every save is read, the two "broken"
// cases are edited in memory.
#include "game/save_load.h"

#include "core/path.h"
#include "game/chassis_loadout.h"
#include "game/runtime.h"
#include "game/sim_player.h"
#include "game/weapons.h"
#include "res/level.h"
#include "res/save.h"
#include "res/vm_exec.h"
#include "world/doors.h"
#include "world/things.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

const fs::path kGame = "/mnt/d/games/Gunmetal 3dfx";
constexpr std::size_t kShippedBlocks = 15;
constexpr std::size_t kSectorSlots = 256;
constexpr std::size_t kActiveSectorSlots = 13;
constexpr std::size_t kLevel0Sectors = 58;
constexpr std::size_t kLevel0Objects = 192;
constexpr std::size_t kLevel0Things = 121;
constexpr int kTicks = 60;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        ++g_checks;                                                                          \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        ++g_checks;                                                                          \
        const auto va = (a);                                                                 \
        const auto vb = (b);                                                                 \
        if (!(va == vb)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);     \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                \
    do {                                                                                     \
        ++g_checks;                                                                          \
        const double va = (a);                                                               \
        const double vb = (b);                                                               \
        if (!(std::fabs(va - vb) <= (eps))) {                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s=%.17g != %s=%.17g\n", __FILE__, __LINE__,   \
                         #a, va, #b, vb);                                                    \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

std::optional<gm::Save> load_shipped(const char* file, std::string& error) {
    const fs::path path = kGame / "SAVEGAME" / file;
    std::FILE* in = std::fopen(path.string().c_str(), "rb");
    if (in == nullptr) {
        error = "cannot open " + path.string();
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes;
    std::uint8_t buffer[1 << 16];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof buffer, in)) != 0) {
        bytes.insert(bytes.end(), buffer, buffer + read);
    }
    std::fclose(in);
    auto save = gm::parse_save(bytes, error);
    if (!save) {
        error = std::string(file) + ": " + error;
    }
    return save;
}

/// The level a save names, resolved through the install (`load_level` is
/// case-insensitive) plus its LIGHT chassis.
struct Level {
    std::optional<gm::LevelBundle> bundle;
    gm::ChassisSpec light;
};

bool load_save_level(const gm::Save& save, Level& out, std::string& error) {
    const std::string name = gm::save_level_name(save);
    if (name.empty()) {
        error = "the save header has no level name";
        return false;
    }
    out.bundle = gm::load_level(kGame, name, error);
    if (!out.bundle) {
        return false;
    }
    if (!out.bundle->world || !out.bundle->entities) {
        error = "level " + name + ": no .sbt/.sbw pair";
        return false;
    }
    const auto sbt = gm::resolve_ci(kGame, name + ".sbt");
    if (!sbt) {
        error = "level " + name + ": no .sbt under " + kGame.string();
        return false;
    }
    const auto chassis = gm::chassis_from_level(*sbt, &error);
    if (!chassis) {
        return false;
    }
    for (const gm::ChassisSpec& spec : *chassis) {
        if (spec.name.find("LIGHT") != std::string::npos) {
            out.light = spec;
            return true;
        }
    }
    error = "level " + name + ": no LIGHT chassis record";
    return false;
}

/// One built runtime + player of the save's level.  `ai_apply_aim` is the
/// `--sim` bridge, so a long tick run really moves things.
struct Fixture {
    std::unique_ptr<gm::GameRuntime> runtime;
    gm::SimPlayer player;
};

bool make_fixture(const gm::Save& save, Fixture& out, std::string& error) {
    Level level;
    if (!load_save_level(save, level, error)) {
        return false;
    }
    gm::GameRuntimeConfig config;
    config.ai_apply_aim = true;
    out.runtime = std::make_unique<gm::GameRuntime>(config);
    const gm::VmState vm = gm::vm_state_from_level(*level.bundle->entities);
    if (!out.runtime->build(*level.bundle->entities, vm, *level.bundle->world, &error)) {
        return false;
    }
    // Placeholder pose: `apply_save()` re-inits the player at the save's
    // feet/yaw.  Only the capsule/chassis config is taken from here.
    return out.player.init(gm::Vec3d{}, 0.0, level.light, &error);
}

// --- digest of a live runtime (the `--sim` fields, docs/RUNTIME_CPP.md) -----

void mix(std::uint64_t& digest, std::uint64_t value) {
    digest ^= value;
    digest *= 1099511628211ull;
}

void mix_real(std::uint64_t& digest, double value) {
    mix(digest, static_cast<std::uint64_t>(static_cast<std::int64_t>(value * 1000.0)));
}

std::uint64_t runtime_digest(const gm::GameRuntime& runtime, const gm::SimPlayer& player) {
    std::uint64_t digest = 1469598103934665603ull;
    mix(digest, runtime.clock());
    for (const gm::Thing& thing : runtime.things().things()) {
        mix(digest, static_cast<std::uint64_t>(thing.id));
        mix(digest, static_cast<std::uint64_t>(thing.alive ? 1 : 0));
        if (thing.world_position) {
            for (double axis : *thing.world_position) {
                mix_real(digest, axis);
            }
        }
    }
    for (std::size_t index = 0; index < runtime.things().things().size(); ++index) {
        const gm::ThingMotionState* state = runtime.things().motion_state(index);
        if (state == nullptr) {
            continue;
        }
        mix_real(digest, state->position.x);
        mix_real(digest, state->position.y);
        mix_real(digest, state->position.z);
        mix_real(digest, state->yaw);
    }
    for (const gm::Door& door : runtime.world().doors()) {
        mix(digest, static_cast<std::uint64_t>(door.id));
        mix(digest, door.phase);
        mix_real(digest, door.open_fraction);
    }
    mix_real(digest, player.state().pos.x);
    mix_real(digest, player.state().pos.y);
    mix_real(digest, player.state().pos.z);
    mix_real(digest, player.yaw());
    mix(digest, static_cast<std::uint64_t>(player.vitals().armor));
    return digest;
}

// --- 1. saved0 / saved1: what really reaches the runtime --------------------

/// Shared body of the two shipped saves; the per-file goldens are arguments.
void check_applied(const gm::Save& save, const char* file, const double (&position)[3],
                   double yaw, std::uint32_t ammo0, std::uint32_t ammo1, int weapon_slot) {
    const gm::SavePlayer& source = *save.player;
    Fixture fixture;
    std::string error;
    if (!make_fixture(save, fixture, error)) {
        std::fprintf(stderr, "save_load: %s fixture: %s\n", file, error.c_str());
        CHECK(false);
        return;
    }
    const double max_speed = fixture.player.config().max_speed;
    const double eye_height = fixture.player.config().eye_height;

    gm::LoadedSave loaded;
    const bool applied = gm::apply_save(save, *fixture.runtime, fixture.player, loaded, &error);
    CHECK(applied);
    if (!applied) {
        std::fprintf(stderr, "save_load: %s apply: %s\n", file, error.c_str());
        return;
    }

    // Header / level.
    CHECK_EQ(loaded.level, std::string("level0"));
    CHECK_EQ(loaded.blocks, kShippedBlocks);
    CHECK_EQ(loaded.sector_stuff, kSectorSlots);
    CHECK_EQ(loaded.sector_stuff_active, kActiveSectorSlots);
    CHECK(!loaded.sector_stuff_applied); // the base form has no VmState
    CHECK_EQ(loaded.unsupported.size(), static_cast<std::size_t>(16));

    // Pose: bit for bit the save fields, and the re-init kept the config.
    CHECK_EQ(fixture.player.state().pos.x, position[0]);
    CHECK_EQ(fixture.player.state().pos.y, position[1]);
    CHECK_EQ(fixture.player.state().pos.z, position[2]);
    CHECK_EQ(fixture.player.yaw(), yaw);
    CHECK_EQ(fixture.player.yaw(), source.angles[2]);
    CHECK_EQ(fixture.player.config().max_speed, max_speed);
    CHECK_EQ(fixture.player.config().eye_height, eye_height);
    CHECK_EQ(fixture.player.tick_index(), 0);

    // Gauges: the model and the runtime-owned span agree with the save.
    CHECK_EQ(fixture.player.vitals().armor, source.armor);
    CHECK_EQ(fixture.player.vitals().armor_max, source.armor_max);
    CHECK_EQ(fixture.player.vitals().shield_max, source.shield_max);
    CHECK_EQ(fixture.player.vitals().shields[0], source.shields[0]);
    CHECK_EQ(fixture.player.vitals().shields[3], source.shields[3]);
    CHECK_EQ(fixture.player.alive(), source.armor > 0);
    const std::span<const gm::Vitals> gauges = fixture.runtime->player_vitals();
    CHECK_EQ(gauges.size(), static_cast<std::size_t>(1));
    CHECK_EQ(gauges[0].armor, 10000);
    CHECK_EQ(gauges[0].armor_max, 10000);
    CHECK_EQ(gauges[0].shield_max, 8000);
    CHECK_EQ(gauges[0].shields[1], 8000);

    // Loadout: the runtime's own arsenal, not a copy in `LoadedSave`.
    const gm::Arsenal& arsenal = fixture.runtime->player_arsenal();
    CHECK_EQ(arsenal.ammo[0], ammo0);
    CHECK_EQ(arsenal.ammo[1], ammo1);
    CHECK_EQ(arsenal.mask, source.weapon_mask);
    CHECK_EQ(arsenal.current, weapon_slot);
    CHECK_EQ(loaded.arsenal.ammo[0], ammo0);

    std::printf("save_load: %s pos=(%.6f, %.6f, %.6f) yaw=%.6f armor=%d/%d "
                "shields=%d/%d/%d/%d ammo=%u,%u mask=0x%x slot=%d blocks=%zu "
                "sector_stuff=%zu/%zu unsupported=%zu\n",
                file, fixture.player.state().pos.x, fixture.player.state().pos.y,
                fixture.player.state().pos.z, fixture.player.yaw(),
                fixture.player.vitals().armor, fixture.player.vitals().armor_max,
                fixture.player.vitals().shields[0], fixture.player.vitals().shields[1],
                fixture.player.vitals().shields[2], fixture.player.vitals().shields[3],
                arsenal.ammo[0], arsenal.ammo[1], arsenal.mask, arsenal.current,
                loaded.blocks, loaded.sector_stuff_active, loaded.sector_stuff,
                loaded.unsupported.size());
}

void test_saved0(const gm::Save& save) {
    // Header title / level are the shipped ones.
    CHECK_EQ(save.header.title, std::string("A SAVED GAME"));
    CHECK_EQ(gm::save_level_name(save), std::string("level0"));
    // saved0: armor 10000/10000, shields 8000 x4, weapon slot 0, ammo 2000/90.
    const double position[3] = {1116.173751898651, 798.2076214059642, 48.0};
    check_applied(save, "saved0", position, 5.0736149946795805, 2000, 90, 0);
}

void test_saved1(const gm::Save& save) {
    CHECK_EQ(save.header.title, std::string("2"));
    const double position[3] = {1156.355385156283, 797.2758300679517, 48.0};
    check_applied(save, "saved1", position, 4.747052494679579, 1975, 87, 1);
}

// --- 2. level from the header, block 8 into the VmState --------------------

void test_level_and_sector_stuff(const gm::Save& save) {
    Level level;
    std::string error;
    CHECK(load_save_level(save, level, error));
    if (!level.bundle) {
        std::fprintf(stderr, "save_load: level: %s\n", error.c_str());
        return;
    }
    CHECK(level.bundle->world.has_value());
    CHECK(level.bundle->entities.has_value());
    CHECK_EQ(level.bundle->world->header.sectors.size(), kLevel0Sectors);
    CHECK_EQ(level.bundle->world->header.object_count, kLevel0Objects);

    gm::VmState vm = gm::vm_state_from_level(*level.bundle->entities);
    gm::LoadedSave loaded;
    const bool sector_ok = gm::apply_save_sector_stuff(save, vm, loaded, &error);
    CHECK(sector_ok);
    if (!sector_ok) {
        std::fprintf(stderr, "save_load: sector stuff: %s\n", error.c_str());
        return;
    }
    CHECK_EQ(loaded.sector_stuff, kSectorSlots);
    CHECK_EQ(loaded.sector_stuff_active, kActiveSectorSlots);
    CHECK(loaded.sector_stuff_applied);

    std::size_t vm_active = 0;
    for (const gm::VmSectorSlot& slot : vm.sectors) {
        const bool empty = slot.move[0] == 0.0 && slot.move[1] == 0.0 &&
                           slot.move[2] == 0.0 && slot.flags == 0 && slot.sound == 0 &&
                           slot.sound_count == 0 && slot.event_a == 0 && slot.event_b == 0 &&
                           slot.damage_a == 0 && slot.damage_b == 0;
        vm_active += empty ? 0 : 1;
    }
    CHECK_EQ(vm_active, kActiveSectorSlots);
    // Field-for-field equal to the save block (slot 5 is the first non-zero
    // one; slot 57 is the last).
    CHECK_EQ(vm.sectors[5].move[0], save.sector_stuff[5].move[0]);
    CHECK_EQ(vm.sectors[5].move[1], save.sector_stuff[5].move[1]);
    CHECK_EQ(vm.sectors[5].flags, save.sector_stuff[5].flags);
    CHECK_EQ(vm.sectors[57].sound, save.sector_stuff[57].sound);
    CHECK_EQ(vm.sectors[57].sound_count, save.sector_stuff[57].sound_count);
    // An empty save slot stays zero.
    CHECK_EQ(vm.sectors[0].flags, 0u);

    // A built runtime + the base `apply_save()` keep the "applied" flag, so
    // block 8 is not reported as unsupported.
    gm::GameRuntimeConfig config;
    gm::GameRuntime runtime(config);
    CHECK(runtime.build(*level.bundle->entities, vm, *level.bundle->world, &error));
    gm::SimPlayer player;
    CHECK(player.init(gm::Vec3d{}, 0.0, level.light, &error));
    CHECK(gm::apply_save(save, runtime, player, loaded, &error));
    CHECK(loaded.sector_stuff_applied);
    CHECK_EQ(loaded.unsupported.size(), static_cast<std::size_t>(15));
    for (const std::string& line : loaded.unsupported) {
        CHECK(line.find("block 8") == std::string::npos);
    }

    std::printf("save_load: level=%s sectors=%zu objects=%zu things=%zu doors=%zu "
                "switches=%zu sector_stuff=%zu slots, %zu active, vm=%zu, applied=%d\n",
                gm::save_level_name(save).c_str(),
                level.bundle->world->header.sectors.size(),
                level.bundle->world->header.object_count, runtime.things().things().size(),
                runtime.world().doors().size(), runtime.world().switches().size(),
                loaded.sector_stuff, loaded.sector_stuff_active, vm_active,
                loaded.sector_stuff_applied ? 1 : 0);
    CHECK_EQ(runtime.things().things().size(), kLevel0Things);

    // The weapon table of the level plus the saved loadout: the CLI path.
    const auto sbt = gm::resolve_ci(kGame, "level0.sbt");
    CHECK(sbt.has_value());
    if (sbt) {
        auto table = gm::Weapons::load(*sbt, &error);
        CHECK(table.has_value());
        if (table) {
            runtime.set_player_weapons(std::move(*table), loaded.arsenal);
            CHECK(runtime.has_player_weapons());
            CHECK_EQ(runtime.player_arsenal().ammo[0], 2000u);
            CHECK_EQ(runtime.player_arsenal().current, 0);
            CHECK(runtime.player_weapons()->at(0) != nullptr);
        }
    }
}

// --- 3. ticks: deterministic and really stepping ---------------------------

void test_ticks(const gm::Save& save) {
    Fixture first;
    Fixture second;
    std::string error;
    CHECK(make_fixture(save, first, error));
    CHECK(make_fixture(save, second, error));
    if (!first.runtime || !second.runtime) {
        return;
    }
    gm::LoadedSave loaded_a;
    gm::LoadedSave loaded_b;
    CHECK(gm::apply_save(save, *first.runtime, first.player, loaded_a, &error));
    CHECK(gm::apply_save(save, *second.runtime, second.player, loaded_b, &error));

    const std::uint64_t before = runtime_digest(*first.runtime, first.player);
    first.runtime->tick(kTicks);
    second.runtime->tick(kTicks);
    const std::uint64_t after_a = runtime_digest(*first.runtime, first.player);
    const std::uint64_t after_b = runtime_digest(*second.runtime, second.player);

    CHECK_EQ(first.runtime->clock(), static_cast<std::uint64_t>(kTicks));
    CHECK_EQ(second.runtime->clock(), static_cast<std::uint64_t>(kTicks));
    CHECK(after_a != before); // the ticks really advanced the state
    CHECK_EQ(after_a, after_b); // two fresh runs are identical
    const auto& motion = first.runtime->things().stats();
    CHECK(motion.motion_moved > 0); // the AI bridge really moved things

    std::printf("save_load: ticks=%d clock=%llu digest0=%016llx digest1=%016llx "
                "deterministic=%d motion_stepped=%zu motion_moved=%zu\n",
                kTicks, static_cast<unsigned long long>(first.runtime->clock()),
                static_cast<unsigned long long>(before),
                static_cast<unsigned long long>(after_a), after_a == after_b ? 1 : 0,
                motion.motion_steps, motion.motion_moved);
}

// --- 4. armor == 0 in the save is a dead player ----------------------------

void test_dead_save(const gm::Save& source) {
    gm::Save save = source;
    save.player->armor = 0;
    save.player->shields = {0, 0, 0, 0};
    Fixture fixture;
    std::string error;
    CHECK(make_fixture(save, fixture, error));
    if (!fixture.runtime) {
        return;
    }
    gm::LoadedSave loaded;
    const bool applied = gm::apply_save(save, *fixture.runtime, fixture.player, loaded, &error);
    CHECK(applied);
    CHECK_EQ(fixture.player.vitals().armor, 0);
    CHECK(!fixture.player.alive());
    const std::span<const gm::Vitals> gauges = fixture.runtime->player_vitals();
    CHECK_EQ(gauges.size(), static_cast<std::size_t>(1));
    CHECK_EQ(gauges[0].armor, 0);
    fixture.runtime->tick(3); // a dead player must not break the tick
    CHECK(!fixture.player.alive());
    std::printf("save_load: armor=0 -> alive=%d runtime_armor=%d clock=%llu\n",
                fixture.player.alive() ? 1 : 0, gauges[0].armor,
                static_cast<unsigned long long>(fixture.runtime->clock()));
}

// --- 5. negatives ----------------------------------------------------------

void test_negatives(const gm::Save& source) {
    std::string error;

    // Truncated save: the container check fails with a text.
    {
        std::FILE* in = std::fopen((kGame / "SAVEGAME" / "saved0.sbt").string().c_str(), "rb");
        CHECK(in != nullptr);
        std::vector<std::uint8_t> bytes;
        if (in != nullptr) {
            std::uint8_t buffer[1000];
            const std::size_t read = std::fread(buffer, 1, sizeof buffer, in);
            bytes.assign(buffer, buffer + read);
            std::fclose(in);
        }
        error.clear();
        const auto broken = gm::parse_save(bytes, error);
        CHECK(!broken);
        CHECK(!error.empty());
        std::printf("save_load: broken save -> \"%s\"\n", error.c_str());
    }

    // Missing level: `load_level` reports the absent `.sbw`/`.sbt`.
    {
        gm::Save save = source;
        save.header.level_name = "nolevel";
        error.clear();
        const auto level = gm::load_level(kGame, gm::save_level_name(save), error);
        CHECK(!level);
        CHECK(error.find("nolevel") != std::string::npos);
        std::printf("save_load: missing level -> \"%s\"\n", error.c_str());
    }

    // Missing save file and an out-of-range slot.
    {
        const auto missing = gm::resolve_save_path(kGame, "19", &error);
        CHECK(!missing);
        CHECK(error.find("not found") != std::string::npos);
        const auto out_of_range = gm::resolve_save_path(kGame, "99", &error);
        CHECK(!out_of_range);
        CHECK(error.find("out of range") != std::string::npos);
        const auto empty = gm::resolve_save_path(kGame, "", &error);
        CHECK(!empty);
        const auto good = gm::resolve_save_path(kGame, "1", &error);
        CHECK(good.has_value());
        std::printf("save_load: resolve slot19=missing slot99=refused slot1=ok\n");
    }

    // Apply negatives need a real runtime + player.
    Fixture fixture;
    if (!make_fixture(source, fixture, error)) {
        std::fprintf(stderr, "save_load: negative fixture: %s\n", error.c_str());
        CHECK(false);
        return;
    }
    gm::LoadedSave loaded;

    // Player block / present slot.
    {
        gm::Save save = source;
        save.player.reset();
        error.clear();
        CHECK(!gm::apply_save(save, *fixture.runtime, fixture.player, loaded, &error));
        CHECK(error.find("player block") != std::string::npos);
        std::printf("save_load: no block 9 -> \"%s\"\n", error.c_str());

        gm::Save no_slot = source;
        no_slot.player->has_slot = false;
        error.clear();
        CHECK(!gm::apply_save(no_slot, *fixture.runtime, fixture.player, loaded, &error));
        CHECK(error.find("present slot") != std::string::npos);
    }

    // Non-initialised player.
    {
        gm::SimPlayer player;
        error.clear();
        CHECK(!gm::apply_save(source, *fixture.runtime, player, loaded, &error));
        CHECK(error.find("init()") != std::string::npos);
    }

    // Out-of-range gauges and weapon slot.
    {
        gm::Save save = source;
        save.player->armor = save.player->armor_max + 1;
        error.clear();
        CHECK(!gm::apply_save(save, *fixture.runtime, fixture.player, loaded, &error));
        CHECK(!error.empty());
        std::printf("save_load: armor>max -> \"%s\"\n", error.c_str());

        gm::Save slot = source;
        slot.player->weapon_slot = 12;
        error.clear();
        CHECK(!gm::apply_save(slot, *fixture.runtime, fixture.player, loaded, &error));
        CHECK(error.find("weapon slot") != std::string::npos);
        std::printf("save_load: weapon slot 12 -> \"%s\"\n", error.c_str());
    }

    // Block 8 with a wrong size is refused (in-memory only).
    {
        Level level;
        if (!load_save_level(source, level, error)) {
            std::fprintf(stderr, "save_load: negative level: %s\n", error.c_str());
            CHECK(false);
            return;
        }
        gm::Save bad = source;
        bad.sector_stuff.pop_back();
        gm::VmState vm = gm::vm_state_from_level(*level.bundle->entities);
        error.clear();
        gm::LoadedSave sector;
        CHECK(!gm::apply_save_sector_stuff(bad, vm, sector, &error));
        CHECK(error.find("expected 256") != std::string::npos);
        std::printf("save_load: sector_stuff size -> \"%s\"\n", error.c_str());
    }
}

} // namespace

int main() {
    const fs::path saved0 = kGame / "SAVEGAME" / "saved0.sbt";
    const fs::path saved1 = kGame / "SAVEGAME" / "saved1.sbt";
    if (!fs::exists(saved0) || !fs::exists(saved1) || !fs::exists(kGame / "level0.sbt") ||
        !fs::exists(kGame / "level0.sbw")) {
        std::printf("save_load: saved0/saved1/level0 not installed, skipped\n");
        return 0;
    }

    std::string error;
    const auto save0 = load_shipped("saved0.sbt", error);
    const auto save1 = load_shipped("saved1.sbt", error);
    if (!save0 || !save1) {
        std::fprintf(stderr, "save_load: %s\n", error.c_str());
        return 1;
    }
    CHECK(save0->player.has_value());
    CHECK(save0->player->has_slot);
    CHECK(save1->player.has_value());
    CHECK(save1->player->has_slot);

    test_saved0(*save0);
    test_saved1(*save1);
    test_level_and_sector_stuff(*save0);
    test_ticks(*save0);
    test_dead_save(*save0);
    test_negatives(*save0);

    if (g_failures != 0) {
        std::fprintf(stderr, "save_load: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("save_load: %d checks, ok\n", g_checks);
    return 0;
}
