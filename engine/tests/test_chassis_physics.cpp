// Chassis physics (`M6.1.1`): mass, plateau speed, linear drag and the
// synthetic suspension of `game/chassis_physics.h`.
//
// Real data: every `.sbt` of the installation (36 files) is parsed with
// `parse_sbt`/`decode_sbt_script`; every `DEFINE_PLAYER_TYPES` record of every
// IF/ELSE branch is collected (17 per level, 21 in `level20` = 616 in total).
// Per record the test checks
//   * the eight doubles at record `+0x48` (op17..op24 = 13/400/1600/k/64/1/80/0.2
//     for LIGHT, 12.5/500/1600/0.92/... for MEDIUM/STEALTH, 12/600/1600/0.96/...
//     for HEAVY) - the block `0x81bfa0`..`0x81bfd8`, of which op20 (`0x81bfb8`)
//     is the retention `k` the drag is derived from;
//   * that op20 is finite and inside (0,1), so `chassis_params()` can be built.
// Then the distinct (name, mass) parameter sets of `level0` (nine: four
// campaign chassis, the same four with the DM/SCAV mass limit 500000, and
// `57 CHEVY`) are printed as a table and checked finite/in range.
//
// Synthetic part: acceleration to the proven plateau `op4*91/(1-k)`, the
// drag time to 10 % against `ln 10/drag`, the suspension decay (damped vs
// `suspension_d = 0`), the loaded-equilibrium ride height, gravity free fall,
// mass invariance, 1000-step bitwise determinism and the refusals
// (`dt <= 0`, `dt > 1/4`, a spring-unstable dt, `mass = 0`, non-finite
// parameters/state/input, unconfigured `step`).
#include "game/chassis_physics.h"

#include "res/sbt.h"
#include "res/sbtvm.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

bool same_double(double a, double b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

bool same_state(const gm::ChassisState& a, const gm::ChassisState& b) {
    return same_double(a.position.x, b.position.x) &&
           same_double(a.position.y, b.position.y) &&
           same_double(a.position.z, b.position.z) &&
           same_double(a.velocity.x, b.velocity.x) &&
           same_double(a.velocity.y, b.velocity.y) &&
           same_double(a.velocity.z, b.velocity.z) &&
           same_double(a.suspension_offset, b.suspension_offset) &&
           same_double(a.suspension_velocity, b.suspension_velocity);
}

bool finite_state(const gm::ChassisState& s) {
    return std::isfinite(s.position.x) && std::isfinite(s.position.y) &&
           std::isfinite(s.position.z) && std::isfinite(s.velocity.x) &&
           std::isfinite(s.velocity.y) && std::isfinite(s.velocity.z) &&
           std::isfinite(s.suspension_offset) && std::isfinite(s.suspension_velocity);
}

// --- real records ----------------------------------------------------------

struct ShippedRecord {
    std::string level;
    std::string name;
    gm::PlayerTypeDef def;
    gm::ChassisSpec spec;
    double retention = 0.0;
};

void walk(const std::vector<gm::SbtInsn>& insns, const std::string& level,
          std::vector<ShippedRecord>& out) {
    for (const gm::SbtInsn& insn : insns) {
        if (insn.name == "DEFINE_PLAYER_TYPES" && !insn.table.empty()) {
            for (const gm::SbtTableRecord& record : insn.table) {
                gm::PlayerTypeDef def;
                def.strings = record.strings;
                if (!def.strings.empty()) {
                    def.name = def.strings.front();
                }
                def.values.reserve(record.values.size());
                for (const gm::SbtValue& value : record.values) {
                    def.values.push_back(value.kind == gm::SbtValue::Kind::Real
                                             ? value.d
                                             : static_cast<double>(value.u));
                }
                std::string error;
                const auto spec = gm::chassis_spec(def, &error);
                if (!spec) {
                    std::fprintf(stderr, "chassis_physics: %s: %s\n", level.c_str(),
                                 error.c_str());
                    ++g_failures;
                    continue;
                }
                ShippedRecord row;
                row.level = level;
                row.name = def.name;
                row.def = std::move(def);
                row.spec = *spec;
                if (row.def.values.size() > gm::kChassisRetentionField) {
                    row.retention = row.def.values[gm::kChassisRetentionField];
                }
                out.push_back(std::move(row));
            }
        }
        if (!insn.body.empty()) {
            walk(insn.body, level, out);
        }
    }
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

bool read_level(const fs::path& path, const std::string& level,
                std::vector<ShippedRecord>& out) {
    const std::vector<std::uint8_t> bytes = read_file(path);
    if (bytes.empty()) {
        std::fprintf(stderr, "chassis_physics: cannot read %s\n", path.string().c_str());
        return false;
    }
    std::string error;
    const auto parsed = gm::parse_sbt(bytes, error);
    if (!parsed) {
        std::fprintf(stderr, "chassis_physics: %s: %s\n", level.c_str(), error.c_str());
        return false;
    }
    if (parsed->script_code.empty()) {
        std::fprintf(stderr, "chassis_physics: %s: no SCRIPT bytecode\n", level.c_str());
        return false;
    }
    const auto program = gm::decode_sbt_script(parsed->script_code, error);
    if (!program) {
        std::fprintf(stderr, "chassis_physics: %s: script: %s\n", level.c_str(),
                     error.c_str());
        return false;
    }
    walk(program->insns, level, out);
    return true;
}

/// The eight doubles at record `+0x48` (`0x81bfa0`..`0x81bfd8`), per chassis
/// name: `docs/PLAYER_GHIDRA.md` §4.1/§4.3 and `docs/FORMATS.md` §12.3.  Only
/// element 3 (the retention `k`, op20 = `0x81bfb8`) is used by the physics.
struct EightDoubles {
    const char* name;
    double values[8];
};

const EightDoubles kEightDoubles[] = {
    {"LIGHT CHASSIS", {13.0, 400.0, 1600.0, 0.90, 64.0, 1.0, 80.0, 0.2}},
    {"MEDIUM CHASSIS", {12.5, 500.0, 1600.0, 0.92, 64.0, 1.0, 80.0, 0.2}},
    {"STEALTH CHASSIS", {12.5, 500.0, 1600.0, 0.92, 64.0, 1.0, 80.0, 0.2}},
    {"HEAVY CHASSIS", {12.0, 600.0, 1600.0, 0.96, 64.0, 1.0, 80.0, 0.2}},
    {"57 CHEVY", {13.0, 400.0, 1600.0, 0.90, 64.0, 1.0, 80.0, 0.2}},
};

const EightDoubles* eight_doubles(const std::string& name) {
    for (const EightDoubles& row : kEightDoubles) {
        if (name == row.name) {
            return &row;
        }
    }
    return nullptr;
}

void test_record_sweep() {
    std::vector<fs::path> files;
    if (fs::exists(kGame)) {
        for (const fs::directory_entry& entry : fs::directory_iterator(kGame)) {
            if (entry.is_regular_file() && entry.path().extension() == ".sbt") {
                files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::printf("chassis_physics: %s not installed, real-record checks skipped\n",
                    kGame.string().c_str());
        return;
    }

    std::vector<ShippedRecord> records;
    std::size_t levels_ok = 0;
    for (const fs::path& path : files) {
        const std::size_t before = records.size();
        if (read_level(path, path.stem().string(), records)) {
            ++levels_ok;
            CHECK(records.size() > before);
        }
    }
    CHECK(levels_ok == files.size());
    CHECK(records.size() == 616); // 35 levels x 17 + level20 x 21
    std::printf("chassis_physics: levels %zu/%zu, records %zu\n", levels_ok, files.size(),
                records.size());

    std::size_t eight_ok = 0;
    std::size_t eight_bad_mapping = 0;
    std::size_t retentions_ok = 0;
    std::size_t per_level_17 = 0;
    std::size_t per_level_21 = 0;
    std::vector<std::string> names;
    for (const ShippedRecord& row : records) {
        const EightDoubles* expected = eight_doubles(row.name);
        CHECK(expected != nullptr);
        if (expected == nullptr) {
            continue;
        }
        CHECK(row.def.values.size() > gm::kChassisEightDoublesFirst +
                                         gm::kChassisEightDoublesCount - 1);
        // The block is eight `D` (double) fields of the signature.
        for (std::size_t i = 0; i < gm::kChassisEightDoublesCount; ++i) {
            const std::size_t field = gm::kChassisEightDoublesFirst + i;
            if (!gm::player_type_field_is_real(field)) {
                ++eight_bad_mapping;
            }
            CHECK(row.def.values[field] == expected->values[i]);
        }
        if (std::find(names.begin(), names.end(), row.name) == names.end()) {
            names.push_back(row.name);
        }
        ++eight_ok;
        const double k = row.def.values[gm::kChassisRetentionField];
        if (std::isfinite(k) && k == expected->values[3] && k > 0.0 && k < 1.0) {
            ++retentions_ok;
        }
    }
    CHECK(eight_bad_mapping == 0);
    CHECK(eight_ok == records.size());
    CHECK(retentions_ok == records.size());
    CHECK(names.size() == 5); // docs/CHASSIS.md §6: 5 names, not 8 chassis
    std::printf("chassis_physics: eight doubles op17..op24 %zu/%zu, retention k %zu/%zu, "
                "names %zu\n",
                eight_ok, records.size(), retentions_ok, records.size(), names.size());
    std::printf("chassis_physics: \"8\" in M6.1.1 = eight doubles per record, not eight "
                "chassis (docs/CHASSIS.md §6: 5 names; the level0 table has 9 sets)\n");

    // Per-level record counts: 17 in every level, 21 in level20 (two extra
    // unconditional four-record blocks, docs/CHASSIS.md §2).
    std::string current;
    std::size_t count = 0;
    auto flush = [&]() {
        if (current.empty()) {
            return;
        }
        if (count == 17) {
            ++per_level_17;
        } else if (count == 21) {
            ++per_level_21;
        } else {
            std::fprintf(stderr, "FAIL %s: %zu DEFINE_PLAYER_TYPES records\n",
                         current.c_str(), count);
            ++g_failures;
        }
    };
    for (const ShippedRecord& row : records) {
        if (row.level != current) {
            flush();
            current = row.level;
            count = 0;
        }
        ++count;
    }
    flush();
    CHECK(per_level_17 + per_level_21 == files.size());
    std::printf("chassis_physics: records per level: %zu x 17, %zu x 21 (level20)\n",
                per_level_17, per_level_21);

    // Retention per name, printed once (the drag input).
    for (const EightDoubles& row : kEightDoubles) {
        double k = 0.0;
        std::size_t seen = 0;
        for (const ShippedRecord& record : records) {
            if (record.name == row.name) {
                k = record.retention;
                ++seen;
            }
        }
        CHECK(seen > 0);
        CHECK(k == row.values[3]);
        std::printf("chassis_physics: k(%-15s) = %.2f over %zu records\n", row.name, k, seen);
    }
}

// --- parameter table -------------------------------------------------------

struct ParamsRow {
    std::string name;
    double mass = 0.0;
    double retention = 0.0;
    gm::ChassisParams params{};
};

/// Distinct (name, mass) sets of `level0`: campaign four, the DM/SCAV four
/// (mass limit 500000) and `57 CHEVY` (GOD_MODE only).
std::vector<ParamsRow> level0_table(const std::vector<ShippedRecord>& records) {
    std::vector<ParamsRow> rows;
    for (const ShippedRecord& record : records) {
        if (record.level != "level0") {
            continue;
        }
        bool seen = false;
        for (const ParamsRow& row : rows) {
            seen = seen || (row.name == record.name && row.mass == record.spec.mass);
        }
        if (seen) {
            continue;
        }
        const auto params = gm::chassis_params(record.spec, record.retention);
        CHECK(params.has_value());
        if (!params) {
            continue;
        }
        ParamsRow row;
        row.name = record.name;
        row.mass = record.spec.mass;
        row.retention = record.retention;
        row.params = *params;
        rows.push_back(std::move(row));
    }
    return rows;
}

void test_params_table(const std::vector<ShippedRecord>& records) {
    const std::vector<ParamsRow> rows = level0_table(records);
    std::printf("chassis_physics: level0 distinct parameter sets %zu "
                "(4 campaign + 4 DM/SCAV mass 500000 + 57 CHEVY)\n",
                rows.size());
    CHECK(rows.size() == 9);
    std::printf("  %-15s %8s %5s %5s %10s %10s %12s %6s %5s %5s %5s\n", "name", "mass", "k",
                "op4", "plateau", "drag", "accel", "k_s", "d_s", "ride", "g");
    for (const ParamsRow& row : rows) {
        const gm::ChassisParams& p = row.params;
        std::printf("  %-15s %8.0f %5.2f %5.2f %10.2f %10.5f %12.2f %6.1f %5.1f %5.1f %5.1f\n",
                    row.name.c_str(), row.mass, row.retention, p.max_speed * (1.0 - row.retention) /
                                                                    gm::kChassisTickHz,
                    p.max_speed, p.drag, p.accel, p.suspension_k, p.suspension_d,
                    p.ride_height, p.gravity);

        // Proven mapping: plateau = op4*91/(1-k), drag = -91*ln(k).
        CHECK_NEAR(p.drag, -gm::kChassisTickHz * std::log(row.retention), 0.0);
        CHECK_NEAR(p.accel, p.drag * p.max_speed, 0.0);

        // Plausible ranges, all finite.
        CHECK(std::isfinite(p.mass) && p.mass > 0.0 && p.mass <= 1.0e6);
        CHECK(std::isfinite(p.max_speed) && p.max_speed > 1.0 && p.max_speed < 1.0e5);
        CHECK(std::isfinite(p.drag) && p.drag > 0.0 && p.drag < 100.0);
        CHECK(std::isfinite(p.accel) && p.accel > 0.0 && p.accel < 1.0e6);
        CHECK(std::isfinite(p.suspension_k) && p.suspension_k > 0.0 && p.suspension_k < 1.0e4);
        CHECK(std::isfinite(p.suspension_d) && p.suspension_d >= 0.0 &&
              p.suspension_d < 100.0);
        CHECK(std::isfinite(p.ride_height) && p.ride_height > 0.0 && p.ride_height < 100.0);
        CHECK(std::isfinite(p.gravity) && p.gravity >= 0.0 && p.gravity <= 1.0e4);

        gm::ChassisPhysics physics;
        CHECK(physics.configure(p));
        CHECK(physics.configured());

        // `k` is what distinguishes the chassis in the eight-double block; the
        // DM/SCAV variants differ only in the mass limit.
        if (row.name == "LIGHT CHASSIS" || row.name == "57 CHEVY") {
            CHECK_NEAR(row.retention, 0.90, 0.0);
        } else if (row.name == "MEDIUM CHASSIS" || row.name == "STEALTH CHASSIS") {
            CHECK_NEAR(row.retention, 0.92, 0.0);
        } else if (row.name == "HEAVY CHASSIS") {
            CHECK_NEAR(row.retention, 0.96, 0.0);
        }
        if (row.mass == 500000.0) {
            CHECK(p.mass == 500000.0);
        }
    }

    // Planar input is clamped, never amplified; `speed()` ignores the
    // ballistic vertical channel.
    {
        gm::ChassisParams params = level0_table(records).front().params;
        gm::ChassisPhysics physics;
        CHECK(physics.configure(params));
        gm::ChassisState unit;
        gm::ChassisState strong;
        unit.velocity = {10.0, -5.0, 0.0};
        strong.velocity = unit.velocity;
        CHECK(physics.step(unit, gm::kChassisDt, gm::Vec3d{1.0, 0.0, 0.0}));
        CHECK(physics.step(strong, gm::kChassisDt, gm::Vec3d{5.0, 0.0, 0.0}));
        CHECK(same_state(unit, strong));
        gm::ChassisState mixed;
        mixed.velocity = {3.0, 4.0, 100.0};
        CHECK_NEAR(physics.speed(mixed), 5.0, 0.0);
        std::printf("chassis_physics: input 5.0 == 1.0 (clamp)=%d, speed(3,4,100)=%.1f\n",
                    same_state(unit, strong) ? 1 : 0, physics.speed(mixed));
    }

    // The shipped numbers of `docs/CHASSIS.md` §4, converted to units/second.
    for (const ParamsRow& row : rows) {
        if (row.name == "LIGHT CHASSIS" && row.mass == 500.0) {
            CHECK_NEAR(row.params.max_speed, 1547.0, 1e-6);
            CHECK_NEAR(row.params.mass, 500.0, 0.0);
        }
        if (row.name == "MEDIUM CHASSIS" && row.mass == 1000.0) {
            CHECK_NEAR(row.params.max_speed, 1137.5, 1e-6);
        }
        if (row.name == "HEAVY CHASSIS" && row.mass == 1600.0) {
            CHECK_NEAR(row.params.max_speed, 1365.0, 1e-6);
        }
        if (row.name == "57 CHEVY") {
            CHECK_NEAR(row.params.max_speed, 1547.0, 1e-6);
            CHECK_NEAR(row.params.mass, 9999.0, 0.0);
        }
    }
}

// --- synthetic dynamics ----------------------------------------------------

gm::ChassisParams light_params(const std::vector<ParamsRow>& rows) {
    for (const ParamsRow& row : rows) {
        if (row.name == "LIGHT CHASSIS" && row.mass == 500.0) {
            return row.params;
        }
    }
    std::fprintf(stderr, "FAIL: LIGHT CHASSIS (500) not in the level0 table\n");
    ++g_failures;
    return {};
}

void test_acceleration(const std::vector<ParamsRow>& rows) {
    std::printf("chassis_physics: acceleration, full planar input, 91 Hz\n");
    for (const ParamsRow& row : rows) {
        gm::ChassisPhysics physics;
        CHECK(physics.configure(row.params));
        gm::ChassisState state;
        const gm::Vec3d input{1.0, 0.0, 0.0};
        const int steps = 910; // 10 s
        double previous = 0.0;
        double fastest = 0.0;
        bool monotone = true;
        double t90 = -1.0;
        for (int i = 0; i < steps; ++i) {
            CHECK(physics.step(state, gm::kChassisDt, input));
            const double speed = physics.speed(state);
            monotone = monotone && speed >= previous;
            if (t90 < 0.0 && speed >= 0.9 * row.params.max_speed) {
                t90 = static_cast<double>(i + 1) * gm::kChassisDt;
            }
            previous = speed;
            fastest = std::max(fastest, speed);
        }
        const double speed = physics.speed(state);
        const double ratio = speed / row.params.max_speed;
        // The discrete semi-implicit form: (1/(1+drag*dt))^n.
        const double analytic =
            std::log(10.0) / std::log(1.0 + row.params.drag * gm::kChassisDt) *
            gm::kChassisDt;
        CHECK(monotone);
        CHECK(ratio > 0.9);
        CHECK(ratio <= 1.01);
        CHECK(fastest <= row.params.max_speed * (1.0 + 1e-12)); // no overshoot
        CHECK(t90 > 0.0);
        CHECK_NEAR(t90, analytic, 2.0 * gm::kChassisDt);
        std::printf("  %-15s %6.0f  v(10s)=%9.3f plateau=%9.3f (%6.2f%%)  t90=%.4f s "
                    "(analytic %.4f)\n",
                    row.name.c_str(), row.mass, speed, row.params.max_speed, 100.0 * ratio, t90,
                    analytic);
    }
}

void test_drag(const std::vector<ParamsRow>& rows) {
    std::printf("chassis_physics: drag, zero input from the plateau\n");
    for (const ParamsRow& row : rows) {
        gm::ChassisPhysics physics;
        CHECK(physics.configure(row.params));
        gm::ChassisState state;
        state.velocity = {row.params.max_speed, 0.0, 0.0};
        const gm::Vec3d input{0.0, 0.0, 0.0};
        double previous = physics.speed(state);
        bool monotone = true;
        int ticks = -1;
        for (int i = 0; i < 2000; ++i) {
            CHECK(physics.step(state, gm::kChassisDt, input));
            const double speed = physics.speed(state);
            monotone = monotone && speed < previous;
            previous = speed;
            if (speed <= 0.1 * row.params.max_speed) {
                ticks = i + 1;
                break;
            }
        }
        const double analytic =
            std::log(10.0) / std::log(1.0 + row.params.drag * gm::kChassisDt) *
            gm::kChassisDt;
        CHECK(monotone);
        CHECK(ticks > 0);
        const double measured = static_cast<double>(ticks) * gm::kChassisDt;
        CHECK_NEAR(measured, analytic, 2.0 * gm::kChassisDt);
        std::printf("  %-15s %6.0f  t10=%.4f s (%d ticks, analytic %.4f), v_end=%.4f\n",
                    row.name.c_str(), row.mass, measured, ticks, analytic,
                    physics.speed(state));
    }
}

/// Largest |suspension_offset| over `steps` steps from `state`.
double peak_offset(const gm::ChassisPhysics& physics, gm::ChassisState& state, int steps) {
    double peak = std::fabs(state.suspension_offset);
    for (int i = 0; i < steps; ++i) {
        CHECK(physics.step(state, gm::kChassisDt, gm::Vec3d{}));
        peak = std::max(peak, std::fabs(state.suspension_offset));
    }
    return peak;
}

void test_suspension(const std::vector<ParamsRow>& rows) {
    const gm::ChassisParams light = light_params(rows);
    std::printf("chassis_physics: suspension (k=%.1f 1/s^2, d=%.1f 1/s, omega=%.4f, "
                "zeta=%.4f)\n",
                light.suspension_k, light.suspension_d, std::sqrt(light.suspension_k),
                light.suspension_d / (2.0 * std::sqrt(light.suspension_k)));

    // Damped: a 0.5 m bump decays by far more than 2x in 2 s.
    {
        gm::ChassisPhysics physics;
        CHECK(physics.configure(light));
        gm::ChassisState state;
        state.suspension_offset = 0.5;
        const double first = peak_offset(physics, state, 64); // [0, 0.70] s
        gm::ChassisState later = state;
        const double second = peak_offset(physics, later, 119); // [0.70, 2.01] s
        const double ratio = first / second;
        // Envelope x(t) = A*e^(-d*t/2): the largest amplitude left at t = 0.70 s.
        const double envelope = 0.5 * std::exp(-0.5 * light.suspension_d * 0.70);
        CHECK(ratio >= 2.0);
        CHECK(second < 0.05);
        CHECK_NEAR(second, envelope, 0.1 * envelope);
        std::printf("  damped:   peak[0,0.70]=%.6f  peak[0.70,2.01]=%.9f  ratio=%.1f "
                    "(>=2), envelope(0.70 s)=%.9f\n",
                    first, second, ratio, envelope);
        // The force and the body height use the shipped mass and ride height.
        CHECK_NEAR(physics.suspension_force(0.5, 0.0),
                   light.mass * light.suspension_k * 0.5, 0.0);
        CHECK_NEAR(physics.suspension_force(0.0, 1.0),
                   light.mass * light.suspension_d, 0.0);
        CHECK_NEAR(physics.suspension_force(0.0, 0.0), 0.0, 0.0);
        gm::ChassisState at_rest;
        CHECK_NEAR(physics.body_height(at_rest), light.ride_height, 0.0);
        gm::ChassisState compressed;
        compressed.suspension_offset = 0.5;
        CHECK_NEAR(physics.body_height(compressed), light.ride_height - 0.5, 0.0);
        std::printf("  force(0.5,0)=%.1f N, force(0,1)=%.1f N, ride=%.2f -> body(0.5)=%.2f\n",
                    physics.suspension_force(0.5, 0.0), physics.suspension_force(0.0, 1.0),
                    light.ride_height, physics.body_height(compressed));
    }

    // Undamped: the same bump keeps its amplitude (symplectic Euler, so a
    // small bounded energy oscillation is allowed).
    {
        gm::ChassisParams undamped = light;
        undamped.suspension_d = 0.0;
        gm::ChassisPhysics physics;
        CHECK(physics.configure(undamped));
        gm::ChassisState state;
        state.suspension_offset = 0.5;
        const double first = peak_offset(physics, state, 64);
        gm::ChassisState later = state;
        const double second = peak_offset(physics, later, 119);
        const double ratio = second / first;
        // Specific spring energy 0.5*(v^2 + k*x^2) of the symplectic Euler
        // oscillator: bounded, no secular growth.
        auto energy = [&](const gm::ChassisState& s) {
            return 0.5 * (s.suspension_velocity * s.suspension_velocity +
                          undamped.suspension_k * s.suspension_offset *
                              s.suspension_offset);
        };
        const double e0 = 0.5 * undamped.suspension_k * 0.25;
        const double e1 = energy(later);
        CHECK(ratio > 0.95 && ratio < 1.05);
        // Symplectic Euler keeps a *bounded* O(omega*dt) energy oscillation
        // (omega*dt = 0.104, so a few percent), never a secular loss.
        CHECK_NEAR(e1, e0, 0.06 * e0);
        std::printf("  undamped: peak[0,0.70]=%.6f  peak[0.70,2.01]=%.6f  ratio=%.5f "
                    "(no decay), energy %.6f -> %.6f (%+.3f%%)\n",
                    first, second, ratio, e0, e1, 100.0 * (e1 - e0) / e0);
    }
}

void test_ride_height_and_gravity(const std::vector<ParamsRow>& rows) {
    const gm::ChassisParams light = light_params(rows);
    gm::ChassisPhysics physics;
    CHECK(physics.configure(light));
    gm::ChassisState state;
    const int steps = 182; // 2 s
    double max_offset = 0.0;
    for (int i = 0; i < steps; ++i) {
        CHECK(physics.step(state, gm::kChassisDt, gm::Vec3d{}));
        max_offset = std::max(max_offset, std::fabs(state.suspension_offset));
    }
    // Rest at input 0: the loaded equilibrium is offset 0 and the body sits at
    // ride_height; gravity only moves the ballistic vertical channel.
    CHECK(max_offset == 0.0);
    CHECK(std::fabs(state.suspension_velocity) == 0.0);
    CHECK_NEAR(physics.body_height(state), light.ride_height, 0.0);
    const double analytic_z = -light.gravity * gm::kChassisDt * gm::kChassisDt * 0.5 *
                              static_cast<double>(steps) * static_cast<double>(steps + 1);
    CHECK_NEAR(state.velocity.z, -light.gravity * static_cast<double>(steps) * gm::kChassisDt,
               1e-9);
    CHECK_NEAR(state.position.z, analytic_z, 1e-9);
    std::printf("chassis_physics: rest 2 s: max|offset|=%.1f, body_height=%.3f, "
                "dz=%.6f (g=%.0f), vz=%.6f\n",
                max_offset, physics.body_height(state), state.position.z, light.gravity,
                state.velocity.z);
}

void test_mass(const std::vector<ParamsRow>& rows) {
    const gm::ChassisParams light = light_params(rows);
    gm::ChassisParams heavy = light;
    heavy.mass = light.mass * 3.0;
    gm::ChassisPhysics a;
    gm::ChassisPhysics b;
    CHECK(a.configure(light));
    CHECK(b.configure(heavy));
    gm::ChassisState sa;
    gm::ChassisState sb;
    sa.suspension_offset = 0.2;
    sb.suspension_offset = 0.2;
    const gm::Vec3d input{0.6, 0.8, 0.0}; // unit length, planar
    for (int i = 0; i < 500; ++i) {
        CHECK(a.step(sa, gm::kChassisDt, input));
        CHECK(b.step(sb, gm::kChassisDt, input));
    }
    // The original integrator has no mass; forces scale with it and cancel.
    CHECK(same_state(sa, sb));
    CHECK_NEAR(b.suspension_force(0.2, 0.5), 3.0 * a.suspension_force(0.2, 0.5), 0.0);
    std::printf("chassis_physics: mass %.0f vs %.0f -> same trajectory=%d, force x3=%.1f\n",
                light.mass, heavy.mass, same_state(sa, sb) ? 1 : 0,
                b.suspension_force(0.2, 0.5));
}

void test_determinism(const std::vector<ParamsRow>& rows) {
    // HEAVY is the slowest chassis (k = 0.96), so 1000 steps cover both the
    // fast and the slow regimes.
    gm::ChassisParams params = light_params(rows);
    for (const ParamsRow& row : rows) {
        if (row.name == "HEAVY CHASSIS" && row.mass == 1600.0) {
            params = row.params;
        }
    }
    gm::ChassisPhysics physics;
    CHECK(physics.configure(params));

    auto run = [&](gm::ChassisState& state) {
        for (int tick = 0; tick < 1000; ++tick) {
            gm::Vec3d input{};
            input.x = (tick % 200 < 120) ? 1.0 : -0.5;
            input.y = (tick % 97 == 0) ? 0.75 : 0.0;
            if (tick == 300) {
                state.suspension_offset = 0.5; // a bump in the middle of the run
            }
            CHECK(physics.step(state, gm::kChassisDt, input));
            CHECK(finite_state(state));
        }
    };
    gm::ChassisState first;
    gm::ChassisState second;
    run(first);
    run(second);
    CHECK(same_state(first, second));
    std::printf("chassis_physics: 1000 steps x2 bit-identical=%d  pos=(%.17g, %.17g, "
                "%.17g) vel=(%.17g, %.17g, %.17g) offset=%.17g\n",
                same_state(first, second) ? 1 : 0, first.position.x, first.position.y,
                first.position.z, first.velocity.x, first.velocity.y, first.velocity.z,
                first.suspension_offset);
}

void test_negatives(const std::vector<ParamsRow>& rows) {
    const gm::ChassisParams good = light_params(rows);
    int refused = 0;
    auto expect_refused = [&](const gm::ChassisParams& params, const gm::ChassisState& state,
                              double dt, gm::Vec3d input, const char* needle = nullptr) {
        gm::ChassisPhysics physics;
        CHECK(physics.configure(params));
        gm::ChassisState probe = state;
        std::string error;
        const bool ok = physics.step(probe, dt, input, &error);
        CHECK(!ok);
        CHECK(!error.empty());
        if (needle != nullptr) {
            CHECK(error.find(needle) != std::string::npos);
        }
        CHECK(same_state(probe, state)); // refused steps never mutate the state
        ++refused;
    };

    // configure() refusals: nothing is installed and step() refuses.
    {
        const double bad[] = {0.0, -1.0, kNaN, kInf};
        for (double value : bad) {
            gm::ChassisParams params = good;
            params.mass = value;
            gm::ChassisPhysics physics;
            std::string error;
            CHECK(!physics.configure(params, &error));
            CHECK(error.find("mass") != std::string::npos);
            CHECK(!physics.configured());
            gm::ChassisState untouched;
            CHECK(!physics.step(untouched, gm::kChassisDt, gm::Vec3d{}));
            ++refused;
        }
        struct Field {
            const char* name;
            void (*set)(gm::ChassisParams&, double);
        };
        const Field fields[] = {
            {"max_speed", [](gm::ChassisParams& p, double v) { p.max_speed = v; }},
            {"drag", [](gm::ChassisParams& p, double v) { p.drag = v; }},
            {"accel", [](gm::ChassisParams& p, double v) { p.accel = v; }},
            {"suspension_k", [](gm::ChassisParams& p, double v) { p.suspension_k = v; }},
            {"suspension_d", [](gm::ChassisParams& p, double v) { p.suspension_d = v; }},
            {"ride_height", [](gm::ChassisParams& p, double v) { p.ride_height = v; }},
            {"gravity", [](gm::ChassisParams& p, double v) { p.gravity = v; }},
        };
        for (const Field& field : fields) {
            gm::ChassisParams params = good;
            field.set(params, -1.0);
            gm::ChassisPhysics physics;
            std::string error;
            CHECK(!physics.configure(params, &error));
            CHECK(error.find(field.name) != std::string::npos);
            ++refused;
            params = good;
            field.set(params, kNaN);
            error.clear();
            CHECK(!physics.configure(params, &error));
            CHECK(error.find(field.name) != std::string::npos);
            ++refused;
        }
        // accel/suspension_d/gravity accept 0.
        gm::ChassisParams zeros = good;
        zeros.accel = 0.0;
        zeros.suspension_d = 0.0;
        zeros.gravity = 0.0;
        gm::ChassisPhysics physics;
        CHECK(physics.configure(zeros));
        CHECK(physics.configured());
    }

    // step() refusals: dt, state, input.
    const gm::ChassisState rest;
    const double bad_dt[] = {0.0, -1.0 / 91.0, kNaN, kInf, gm::kChassisMaxDt + 0.01};
    for (double dt : bad_dt) {
        expect_refused(good, rest, dt, gm::Vec3d{1.0, 0.0, 0.0}, "dt");
    }
    // Explicit spring limit: omega*dt >= 2.
    {
        gm::ChassisParams stiff = good;
        stiff.suspension_k = 4.0 * gm::kChassisTickHz * gm::kChassisTickHz;
        expect_refused(stiff, rest, gm::kChassisDt, gm::Vec3d{}, "spring");
    }
    {
        gm::ChassisState bad;
        bad.position.x = kNaN;
        expect_refused(good, bad, gm::kChassisDt, gm::Vec3d{}, "state");
        bad = gm::ChassisState{};
        bad.velocity.y = kInf;
        expect_refused(good, bad, gm::kChassisDt, gm::Vec3d{}, "state");
        bad = gm::ChassisState{};
        bad.suspension_offset = kNaN;
        expect_refused(good, bad, gm::kChassisDt, gm::Vec3d{}, "state");
        bad = gm::ChassisState{};
        bad.suspension_velocity = kInf;
        expect_refused(good, bad, gm::kChassisDt, gm::Vec3d{}, "state");
    }
    {
        gm::Vec3d bad{1.0, 0.0, 0.0};
        bad.x = kNaN;
        expect_refused(good, rest, gm::kChassisDt, bad, "input");
        bad = gm::Vec3d{0.0, 1.0, 0.0};
        bad.y = kInf;
        expect_refused(good, rest, gm::kChassisDt, bad, "input");
        bad = gm::Vec3d{0.0, 0.0, 1.0};
        bad.z = kNaN; // ignored by the drive, still validated
        expect_refused(good, rest, gm::kChassisDt, bad, "input");
    }
    // Unconfigured object.
    {
        gm::ChassisPhysics physics;
        gm::ChassisState state;
        std::string error;
        CHECK(!physics.step(state, gm::kChassisDt, gm::Vec3d{}, &error));
        CHECK(error.find("configure") != std::string::npos);
        ++refused;
    }
    // chassis_params() refusals: bad retention, bad mass/speed.
    {
        gm::ChassisSpec spec;
        spec.name = "X";
        spec.mass = 500.0;
        spec.max_speed = 1.7;
        std::string error;
        const double bad_retention[] = {0.0, 1.0, -0.9, kNaN};
        for (double k : bad_retention) {
            CHECK(!gm::chassis_params(spec, k, &error));
            CHECK(error.find("retention") != std::string::npos);
            ++refused;
        }
        gm::ChassisSpec bad = spec;
        bad.mass = 0.0;
        CHECK(!gm::chassis_params(bad, 0.9, &error));
        CHECK(error.find("mass") != std::string::npos);
        ++refused;
        bad = spec;
        bad.max_speed = 0.0;
        CHECK(!gm::chassis_params(bad, 0.9, &error));
        CHECK(error.find("max speed") != std::string::npos);
        ++refused;
    }
    std::printf("chassis_physics: negatives refused with a text: %d cases\n", refused);
}

} // namespace

int main() {
    test_record_sweep();

    // The synthetic tests need the real chassis numbers; without the
    // installation they use the documented LIGHT values (`docs/CHASSIS.md` §4).
    std::vector<ShippedRecord> records;
    if (fs::exists(kGame)) {
        std::vector<fs::path> files;
        for (const fs::directory_entry& entry : fs::directory_iterator(kGame)) {
            if (entry.is_regular_file() && entry.path().extension() == ".sbt") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& path : files) {
            if (path.stem() == "level0" || path.stem() == "dm1") {
                CHECK(read_level(path, path.stem().string(), records));
            }
        }
    }
    if (!records.empty()) {
        test_params_table(records);
    }
    std::vector<ParamsRow> rows = level0_table(records);
    if (rows.empty()) {
        std::printf("chassis_physics: level0 not installed, synthetic tests use the "
                    "documented LIGHT numbers\n");
        gm::ChassisSpec light;
        light.name = "LIGHT CHASSIS";
        light.mass = 500.0;
        light.max_speed = 1.7;
        const auto params = gm::chassis_params(light, 0.9);
        CHECK(params.has_value());
        if (params) {
            ParamsRow row;
            row.name = light.name;
            row.mass = light.mass;
            row.retention = 0.9;
            row.params = *params;
            rows.push_back(row);
        }
    }
    test_acceleration(rows);
    test_drag(rows);
    test_suspension(rows);
    test_ride_height_and_gravity(rows);
    test_mass(rows);
    test_determinism(rows);
    test_negatives(rows);

    if (g_failures != 0) {
        std::fprintf(stderr, "chassis_physics: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("chassis_physics: ok\n");
    return 0;
}
