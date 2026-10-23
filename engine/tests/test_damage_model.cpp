// Damage intake (`0x453b50`) against the numbers of docs/DAMAGE.md.
//
// The Python reference is `python3 tools/re/damage.py --calc --plate N
// --side 0 --repeat 0`; the ship values are armor 10000 and four plates of
// 8000, so the hit counts below are the exact "how many shots does a shield
// of 80 take" answers the doc quotes.
#include "game/weapons.h"

#include <cstdio>
#include <cstdint>
#include <limits>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        const auto va = (a);                                                         \
        const auto vb = (b);                                                         \
        if (!(va == vb)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s (%lld) != %s (%lld)\n", __FILE__,   \
                         __LINE__, #a, static_cast<long long>(va), #b,               \
                         static_cast<long long>(vb));                                \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

gm::Vitals shipped_vitals() {
    gm::Vitals vitals;
    vitals.armor = 10000;
    vitals.armor_max = 10000;
    vitals.shield_max = 8000;
    vitals.shields = {8000, 8000, 8000, 8000};
    return vitals;
}

/// `damage.py --calc --plate P --side 0 --repeat 0`: hit the front quadrant
/// until the armor is gone and return how many hits it took.
int hits_to_kill(std::int32_t plate) {
    gm::Vitals vitals = shipped_vitals();
    for (int hits = 0; hits < 1000; ++hits) {
        const gm::GaugeDamageResult result =
            gm::apply_damage_gauge(vitals, plate, 0, 0, 0);
        if (!result.ok) {
            return -1;
        }
        if (result.dead) {
            return hits + 1;
        }
    }
    return -2;
}

void test_hit_counts() {
    // 45 = 8000/400 (front) + 10000/400 (armor) -- exactly what the doc says.
    CHECK_EQ(hits_to_kill(400), 45);
    // 12 = ceil(8000/1500)=6 hits (the 6th spills 1000) + ceil(9000/1500)=6.
    CHECK_EQ(hits_to_kill(1500), 12);
    // 3 = 2 hits break the plate (12000 > 8000) and the 3rd takes the armor.
    CHECK_EQ(hits_to_kill(6000), 3);
}

void test_first_hit() {
    gm::Vitals vitals = shipped_vitals();
    const gm::GaugeDamageResult result = gm::apply_damage_gauge(vitals, 400, 0, 0, 0);
    CHECK(result.ok);
    CHECK(!result.dead);
    CHECK_EQ(result.plate_absorbed, 400);
    CHECK_EQ(result.hull_absorbed, 0);
    CHECK_EQ(result.overflow, 0);
    CHECK_EQ(vitals.shields[0], 7600);
    CHECK_EQ(vitals.shields[1], 8000);
    CHECK_EQ(vitals.shields[2], 8000);
    CHECK_EQ(vitals.shields[3], 8000);
    CHECK_EQ(vitals.armor, 10000);
}

void test_front_then_spill() {
    gm::Vitals vitals = shipped_vitals();
    for (int i = 0; i < 20; ++i) {
        gm::apply_damage_gauge(vitals, 400, 0, 0, 0);
    }
    CHECK_EQ(vitals.shields[0], 0);
    CHECK_EQ(vitals.armor, 10000); // the plate absorbed everything so far
    const gm::GaugeDamageResult spill = gm::apply_damage_gauge(vitals, 400, 0, 0, 0);
    CHECK(spill.ok);
    CHECK_EQ(spill.plate_absorbed, 0);
    CHECK_EQ(spill.hull_absorbed, 400);
    CHECK_EQ(vitals.armor, 9600);
}

void test_flag2_full_per_plate() {
    // `flags & 2`: the same plate damage is applied to every quadrant, with no
    // direction and no spill to the hull.
    gm::Vitals vitals = shipped_vitals();
    const gm::GaugeDamageResult result = gm::apply_damage_gauge(vitals, 400, 0, 2, -1);
    CHECK(result.ok);
    CHECK_EQ(result.plate_absorbed, 1600);
    for (int side = 0; side < 4; ++side) {
        CHECK_EQ(vitals.shields[static_cast<std::size_t>(side)], 7600);
    }
    CHECK_EQ(vitals.armor, 10000);
}

void test_no_direction_quarter() {
    // `flags & 1` (no direction) without bit 2: plate>>2 per quadrant.
    gm::Vitals vitals = shipped_vitals();
    const gm::GaugeDamageResult result = gm::apply_damage_gauge(vitals, 400, 0, 1, 0);
    CHECK(result.ok);
    for (int side = 0; side < 4; ++side) {
        CHECK_EQ(vitals.shields[static_cast<std::size_t>(side)], 7900);
    }
    CHECK_EQ(vitals.armor, 10000);
}

void test_death() {
    gm::Vitals vitals = shipped_vitals();
    gm::GaugeDamageResult result;
    for (int i = 0; i < 100 && vitals.armor > 0; ++i) {
        result = gm::apply_damage_gauge(vitals, 400, 0, 0, 0);
    }
    CHECK(result.ok);
    CHECK(result.dead);
    CHECK_EQ(vitals.armor, 0);
}

void test_multipliers() {
    // `mult_plate`/`mult_hull` scale the incoming damage before it is applied.
    gm::Vitals vitals = shipped_vitals();
    vitals.mult_plate = 0.5;
    const gm::GaugeDamageResult result = gm::apply_damage_gauge(vitals, 400, 0, 0, 0);
    CHECK(result.ok);
    CHECK_EQ(result.plate_absorbed, 200);
    CHECK_EQ(vitals.shields[0], 7800);

    // `flags & 4`: already scaled, so the multipliers are skipped.
    gm::Vitals scaled = shipped_vitals();
    scaled.mult_plate = 0.5;
    const gm::GaugeDamageResult again = gm::apply_damage_gauge(scaled, 400, 0, 4, 0);
    CHECK(again.ok);
    CHECK_EQ(again.plate_absorbed, 400);
}

void test_blast_falloff() {
    gm::Blast blast;
    blast.plate = 400;
    blast.hull = 100;
    blast.radius = 1000;
    // damage * (radius - distance) / radius, integer division.
    CHECK_EQ(gm::blast_damage(blast, 0), 400);
    CHECK_EQ(gm::blast_damage(blast, 250), 300);
    CHECK_EQ(gm::blast_damage(blast, 999), 0);   // 400/1000 truncates to 0
    CHECK_EQ(gm::blast_damage(blast, 1000), 0);
    CHECK_EQ(gm::blast_hull_damage(blast, 500), 50);
    // A negative plate is a status code and passes through unchanged.
    blast.plate = -1;
    CHECK_EQ(gm::blast_damage(blast, 500), -1);
    blast.plate = 400;
    blast.radius = 0; // no divide by zero; only the centre hits
    CHECK_EQ(gm::blast_damage(blast, 0), 0);
    CHECK_EQ(gm::blast_damage(blast, 5), 0);
}

} // namespace

int main() {
    test_hit_counts();
    test_first_hit();
    test_front_then_spill();
    test_flag2_full_per_plate();
    test_no_direction_quarter();
    test_death();
    test_multipliers();
    test_blast_falloff();
    if (g_failures != 0) {
        std::fprintf(stderr, "damage_model: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("damage_model: ok\n");
    return 0;
}
