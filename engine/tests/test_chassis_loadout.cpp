// Chassis loadout (`M6.1.4`): `DEFINE_PLAYER_TYPES` record -> the nine fields
// the original applies (`docs/CHASSIS.md` §3/§4).
//
// Synthetic part: the five shipped chassis (four campaign + 57 CHEVY), the
// `*100` armour scaling, the shop price formula and the `ChassisMotion`
// mapping.  Real part: `level0.sbt` and `dm1.sbt` are parsed with
// `parse_sbt`/`decode_sbt_script`; every `DEFINE_PLAYER_TYPES` table in any
// IF/ELSE branch is collected and the documented numbers are pinned
// (LIGHT 500/0.003/80/100/1.7/16/10.0/10000/9, ...).  `dm1` has no CHEVY and
// every campaign chassis carries `op0 = 500000` in the DM branch, which is the
// only difference of that branch.
//
// Negatives: a short record, a non-finite field, a negative value, a
// non-integer or out-of-range model/sound index.
#include "game/chassis_loadout.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

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
            std::fprintf(stderr, "FAIL %s:%d: %s != %s (%lld vs %lld)\n", __FILE__, __LINE__, \
                         #a, #b, static_cast<long long>(va), static_cast<long long>(vb));     \
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
constexpr double kEps = 1e-12;

gm::PlayerTypeDef make(const std::string& name, double mass, double penalty,
                       double armour, double limit, double speed, double model,
                       double eye, double price, double sound) {
    gm::PlayerTypeDef def;
    def.name = name;
    def.strings = {name};
    def.values.assign(20, 0.0);
    def.values[gm::kChassisMass] = mass;
    def.values[gm::kChassisWeightPenalty] = penalty;
    def.values[gm::kChassisArmour] = armour;
    def.values[gm::kChassisArmourLimit] = limit;
    def.values[gm::kChassisMaxSpeed] = speed;
    def.values[gm::kChassisModel] = model;
    def.values[gm::kChassisEyeHeight] = eye;
    def.values[gm::kChassisPrice] = price;
    def.values[gm::kChassisSound] = sound;
    return def;
}

const gm::ChassisSpec* find(const std::vector<gm::ChassisSpec>& specs,
                            const std::string& name) {
    for (const gm::ChassisSpec& spec : specs) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

void test_synthetic() {
    // docs/CHASSIS.md §4, campaign (`else`) branch.
    const gm::PlayerTypeDef light = make("LIGHT CHASSIS", 500, 0.003, 80, 100, 1.7, 16, 10.0,
                                         10000, 9);
    const auto spec = gm::chassis_spec(light);
    CHECK(spec.has_value());
    if (spec) {
        CHECK(spec->name == "LIGHT CHASSIS");
        CHECK_NEAR(spec->mass, 500.0, kEps);
        CHECK_NEAR(spec->weight_penalty, 0.003, kEps);
        CHECK_NEAR(spec->armour, 8000.0, kEps);        // op2 * 100
        CHECK_NEAR(spec->armour_limit, 10000.0, kEps); // op3 * 100
        CHECK_NEAR(spec->max_speed, 1.7, kEps);
        CHECK_EQ(spec->model_index, 16);
        CHECK_NEAR(spec->eye_height, 10.0, kEps);
        CHECK_NEAR(spec->price, 10000.0, kEps);
        CHECK_EQ(spec->sound_index, 9);
        // 0x479dd0 with [0x818954] = 50 (100 shields) -> the shipped price.
        CHECK_NEAR(spec->shop_price(50.0), 26000.0, kEps);

        gm::ChassisMotion motion = gm::chassis_motion(*spec);
        CHECK_NEAR(motion.max_speed, 1.7, kEps);
        CHECK_NEAR(motion.weight_penalty, 0.003, kEps);
        CHECK_NEAR(motion.weight_limit, 500.0, kEps);
        // 0x4524d0: 1.7 - 0.003 * (615 - 500) = 1.355 (docs/CHASSIS.md §4).
        motion.weight = 615.0;
        CHECK_NEAR(gm::weight_limited_speed(motion), 1.355, kEps);
    }

    // The other four rows: shop price and the silent STEALTH.
    struct Row {
        const char* name;
        double mass, penalty, armour, limit, speed, model, eye, price, sound;
        double shop;
    };
    const Row rows[] = {
        {"MEDIUM CHASSIS", 1000, 0.0006, 130, 100, 1.0, 17, 11.0, 30000, 10, 56000},
        {"STEALTH CHASSIS", 500, 0.001, 90, 100, 1.0, 18, 11.0, 350000, -1, 368000},
        {"HEAVY CHASSIS", 1600, 0.0002, 200, 100, 0.6, 15, 12.0, 80000, 11, 120000},
        {"57 CHEVY", 9999, 0.0001, 300, 80, 1.7, 15, 14.0, 500000, 7, 560000},
    };
    for (const Row& row : rows) {
        const auto one = gm::chassis_spec(make(row.name, row.mass, row.penalty, row.armour,
                                               row.limit, row.speed, row.model, row.eye,
                                               row.price, row.sound));
        if (!one) {
            std::fprintf(stderr, "FAIL chassis_spec(%s) returned null\n", row.name);
            ++g_failures;
            continue;
        }
        CHECK_EQ(one->model_index, static_cast<int>(row.model));
        CHECK_EQ(one->sound_index, static_cast<int>(row.sound));
        CHECK_NEAR(one->shop_price(50.0), row.shop, kEps);
        CHECK_NEAR(one->eye_height, row.eye, kEps);
        CHECK_NEAR(one->mass, row.mass, kEps);
        if (std::string(row.name) == "STEALTH CHASSIS") {
            CHECK_EQ(one->sound_index, -1); // no engine sound
        }
    }
}

void test_negatives() {
    std::string error;
    gm::PlayerTypeDef short_def = make("X", 1, 0, 0, 0, 0, 0, 0, 0, 0);
    short_def.values.resize(11);
    CHECK(!gm::chassis_spec(short_def, &error));
    CHECK(error.find("need at least 12") != std::string::npos);

    gm::PlayerTypeDef nan = make("X", 1, 0, 0, 0, 0, 0, 0, 0, 0);
    nan.values[gm::kChassisEyeHeight] = std::numeric_limits<double>::quiet_NaN();
    error.clear();
    CHECK(!gm::chassis_spec(nan, &error));
    CHECK(error.find("finite") != std::string::npos);

    gm::PlayerTypeDef negative = make("X", 1, 0, 0, 0, 0, 0, 0, 0, 0);
    negative.values[gm::kChassisPrice] = -1;
    CHECK(!gm::chassis_spec(negative, &error));

    gm::PlayerTypeDef bad_model = make("X", 1, 0, 0, 0, 0, 256, 0, 0, 0);
    CHECK(!gm::chassis_spec(bad_model, &error));

    gm::PlayerTypeDef fractional_model = make("X", 1, 0, 0, 0, 0, 16.5, 0, 0, 0);
    CHECK(!gm::chassis_spec(fractional_model, &error));

    gm::PlayerTypeDef bad_sound = make("X", 1, 0, 0, 0, 0, 0, 0, 0, 300);
    CHECK(!gm::chassis_spec(bad_sound, &error));
}

void test_real_levels() {
    if (!fs::exists(kGame)) {
        std::printf("chassis_loadout: %s not found, real levels skipped\n",
                    kGame.string().c_str());
        return;
    }

    std::string error;
    const auto level0 = gm::chassis_from_level(kGame / "level0.sbt", &error);
    CHECK(level0.has_value());
    if (!level0) {
        std::fprintf(stderr, "FAIL level0: %s\n", error.c_str());
        return;
    }
    // level0 keeps the campaign table in `else` and adds `IF GOD_MODE`.
    const gm::ChassisSpec* light = find(*level0, "LIGHT CHASSIS");
    const gm::ChassisSpec* heavy = find(*level0, "HEAVY CHASSIS");
    const gm::ChassisSpec* chevy = find(*level0, "57 CHEVY");
    CHECK(light != nullptr);
    CHECK(heavy != nullptr);
    CHECK(chevy != nullptr);
    if (light != nullptr) {
        CHECK_NEAR(light->mass, 500.0, kEps);
        CHECK_NEAR(light->eye_height, 10.0, kEps);
        CHECK_EQ(light->model_index, 16);
        CHECK_EQ(light->sound_index, 9);
        CHECK_NEAR(light->shop_price(50.0), 26000.0, kEps);
    }
    if (heavy != nullptr) {
        CHECK_NEAR(heavy->mass, 1600.0, kEps);
        CHECK_NEAR(heavy->eye_height, 12.0, kEps);
        CHECK_EQ(heavy->model_index, 15);
        CHECK_NEAR(heavy->shop_price(50.0), 120000.0, kEps);
    }
    if (chevy != nullptr) {
        CHECK_NEAR(chevy->mass, 9999.0, kEps);
        CHECK_NEAR(chevy->eye_height, 14.0, kEps);
        CHECK_EQ(chevy->model_index, 15);
        CHECK_EQ(chevy->sound_index, 7);
        CHECK_NEAR(chevy->shop_price(50.0), 560000.0, kEps);
    }

    const auto dm1 = gm::chassis_from_level(kGame / "dm1.sbt", &error);
    CHECK(dm1.has_value());
    if (dm1) {
        // Every level carries the `IF GOD_MODE` branch, so CHEVY is in the file;
        // what changes is the branch that runs (`docs/CHASSIS.md` §2).
        CHECK(find(*dm1, "57 CHEVY") != nullptr);
        bool saw_dm_mass = false;
        bool saw_silent_stealth = false;
        for (const gm::ChassisSpec& spec : *dm1) {
            if (std::fabs(spec.mass - 500000.0) <= kEps) {
                saw_dm_mass = true; // the DM/SCAV branch raises the limit
            }
            if (spec.name == "STEALTH CHASSIS" && spec.sound_index == -1) {
                saw_silent_stealth = true;
            }
        }
        CHECK(saw_dm_mass);
        CHECK(saw_silent_stealth);
    }
}

} // namespace

int main() {
    test_synthetic();
    test_negatives();
    test_real_levels();
    if (g_failures == 0) {
        std::printf("chassis_loadout: ok\n");
    }
    return g_failures == 0 ? 0 : 1;
}
