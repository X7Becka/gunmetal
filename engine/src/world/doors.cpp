#include "world/doors.h"

#include "game/events.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <utility>

#include "render/collide.h"
#include "res/scene.h"
#include "res/sbtvm.h"

namespace gm {
namespace {

/// World centroid of one `.sbw` polygon (`0x43577c`): the mean of its
/// vertices after the object transform `0x434887` (`origin + B * local`, the
/// same 12 doubles `render/collide.cpp` decodes).  Returns false for a
/// missing sector/object/polygon or an out-of-range vertex index.
bool polygon_centroid(const SbwLevel& world, int sector, int object, int polygon,
                      std::array<double, 3>& out) {
    if (sector < 0 || object < 0 || polygon < 0) {
        return false;
    }
    const auto& sectors = world.header.sectors;
    if (static_cast<std::size_t>(sector) >= sectors.size()) {
        return false;
    }
    const SbwSectorRef& source = sectors[static_cast<std::size_t>(sector)];
    if (static_cast<std::size_t>(object) >= source.objects.size()) {
        return false;
    }
    std::size_t geometry_index = 0;
    for (int s = 0; s < sector; ++s) {
        geometry_index += sectors[static_cast<std::size_t>(s)].objects.size();
    }
    geometry_index += static_cast<std::size_t>(object);
    if (geometry_index >= world.geometry.size()) {
        return false;
    }
    const SbwGeometry& geometry = world.geometry[geometry_index];
    if (static_cast<std::size_t>(polygon) >= geometry.polys.size()) {
        return false;
    }
    const SbwPoly& poly = geometry.polys[static_cast<std::size_t>(polygon)];
    if (poly.indices.empty()) {
        return false;
    }
    const std::array<std::uint8_t, 96>& header =
        source.objects[static_cast<std::size_t>(object)].header;
    double v[12];
    for (int i = 0; i < 12; ++i) {
        std::memcpy(&v[i], header.data() + 8 * i, sizeof(double));
        if (!std::isfinite(v[i])) {
            return false;
        }
    }
    const Vec3d origin{v[0], v[1], v[2]};
    const Vec3d bx{v[3], v[4], v[5]};
    const Vec3d by{v[6], v[7], v[8]};
    const Vec3d bz{v[9], v[10], v[11]};
    Vec3d sum{};
    for (const std::uint16_t index : poly.indices) {
        if (static_cast<std::size_t>(index) >= geometry.vertices.size()) {
            return false;
        }
        const Vec3f& vertex = geometry.vertices[static_cast<std::size_t>(index)];
        const Vec3d local{vertex.x, vertex.y, vertex.z};
        sum = sum + origin + bx * local.x + by * local.y + bz * local.z;
    }
    const double count = static_cast<double>(poly.indices.size());
    out = {sum.x / count, sum.y / count, sum.z / count};
    return true;
}

// --- little-endian readers over a byte payload -----------------------------
bool read_u32(const std::vector<std::uint8_t>& data, std::size_t offset,
              std::uint32_t& value) {
    if (offset > data.size() || data.size() - offset < 4) {
        return false;
    }
    std::memcpy(&value, data.data() + offset, 4);
    return true;
}

bool read_i32(const std::vector<std::uint8_t>& data, std::size_t offset, int& value) {
    std::uint32_t raw = 0;
    if (!read_u32(data, offset, raw)) {
        return false;
    }
    std::memcpy(&value, &raw, 4);
    return true;
}

bool read_f64(const std::vector<std::uint8_t>& data, std::size_t offset, double& value) {
    if (offset > data.size() || data.size() - offset < 8) {
        return false;
    }
    std::memcpy(&value, data.data() + offset, 8);
    return std::isfinite(value);
}

int as_i32(std::uint32_t value) {
    int out = 0;
    std::memcpy(&out, &value, 4);
    return out;
}

std::uint32_t clamp_phase(std::int64_t phase) {
    if (phase <= 0) {
        return 0;
    }
    if (phase >= static_cast<std::int64_t>(kDoorPhaseFull)) {
        return kDoorPhaseFull;
    }
    return static_cast<std::uint32_t>(phase);
}

bool all_finite(const std::array<double, 3>& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

/// How many ticks a door needs to travel from its phase to *target*.
std::uint64_t ticks_to_phase(std::uint32_t phase, std::uint32_t target, std::uint32_t step) {
    const std::uint32_t distance = target > phase ? target - phase : phase - target;
    if (distance == 0) {
        return 0;
    }
    return (static_cast<std::uint64_t>(distance) + step - 1) / step;
}

// --- record -> Door ---------------------------------------------------------

bool parse_door_record(const std::vector<std::uint8_t>& payload, std::size_t offset,
                       Door& door) {
    if (offset > payload.size() || payload.size() - offset < 0xB8) {
        return false;
    }
    int flags = 0;
    if (!read_i32(payload, offset + 0x68, flags)) {
        return false;
    }
    door.flags = flags;
    door.kind = flags & 0xF;
    for (int k = 0; k < 5; ++k) {
        if (!read_i32(payload, offset + 0x6C + static_cast<std::size_t>(k) * 4,
                      door.ids[static_cast<std::size_t>(k)])) {
            return false;
        }
    }
    int state = 0;
    if (!read_i32(payload, offset + 0x80, state)) {
        return false;
    }
    if (state < -1 || state > 1) {
        return false;
    }
    door.state = static_cast<DoorState>(state);
    if (!read_i32(payload, offset + 0x84, door.sector) ||
        !read_i32(payload, offset + 0x88, door.object)) {
        return false;
    }
    std::uint32_t phase = 0;
    if (!read_u32(payload, offset + 0x8C, phase)) {
        return false;
    }
    door.phase = std::min(phase, kDoorPhaseFull);
    door.phase_prev = door.phase;
    door.phase_next = door.phase;
    if (!read_i32(payload, offset + 0x94, door.step)) {
        return false;
    }
    if (door.step < 0) {
        door.step = 0;
    }
    // Door dwell/timer (`docs/DOOR_FIELDS.md`): +0x98 is the auto-close
    // delay copied into the timer (+0x9c) when the door finishes opening.
    int dwell = 0;
    if (!read_i32(payload, offset + 0x98, dwell)) {
        return false;
    }
    door.hold_ticks = static_cast<std::uint32_t>(std::max(dwell, 0));
    int timer = 0;
    if (!read_i32(payload, offset + 0x9C, timer)) {
        return false;
    }
    door.timer = timer > 0 ? static_cast<std::uint32_t>(timer) : 0u;
    for (int k = 0; k < 3; ++k) {
        if (!read_f64(payload, offset + static_cast<std::size_t>(k) * 8,
                      door.closed[static_cast<std::size_t>(k)]) ||
            !read_f64(payload, offset + 0x18 + static_cast<std::size_t>(k) * 8,
                      door.open[static_cast<std::size_t>(k)]) ||
            !read_f64(payload, offset + 0x30 + static_cast<std::size_t>(k) * 8,
                      door.size[static_cast<std::size_t>(k)]) ||
            !read_f64(payload, offset + 0x48 + static_cast<std::size_t>(k) * 8,
                      door.axis[static_cast<std::size_t>(k)])) {
            return false;
        }
    }
    if (!read_f64(payload, offset + 0x60, door.sound_level)) {
        return false;
    }
    // +0xa0..+0xa8 are v0/v1/v2, +0xac..+0xb4 n0/n1/n2 (`0x44e250`).
    for (int k = 0; k < 3; ++k) {
        int volume = 0;
        int sample = 0;
        if (!read_i32(payload, offset + 0xA0 + static_cast<std::size_t>(k) * 4, volume) ||
            !read_i32(payload, offset + 0xAC + static_cast<std::size_t>(k) * 4, sample)) {
            return false;
        }
        DoorSound sound;
        sound.volume = volume;
        sound.sample = sample;
        (k == 0 ? door.start_sound : (k == 1 ? door.move_sound : door.stop_sound)) = sound;
    }
    door.event_open = door.ids[3];
    door.event_close = door.ids[4];
    door.sliding = false;
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(door.open[static_cast<std::size_t>(k)] -
                      door.closed[static_cast<std::size_t>(k)]) > 1e-6) {
            door.sliding = true;
        }
    }
    door.linked = door.ids[3] != 0;
    return true;
}

// --- SCRIPT -> Door / Switch ------------------------------------------------

Door door_from_scene(const SceneDoor& src) {
    Door door;
    door.flags = src.flags;
    door.kind = src.flags & 0xF;
    door.ids = src.ids;
    // The loader folds the size into both corners (+0x00 += +0x30 at
    // 0x44e368..0x44e3f6), so the runtime record — and the save — hold
    // ``corner + size``.  The SCRIPT operands are the raw corners.
    for (int k = 0; k < 3; ++k) {
        door.size[static_cast<std::size_t>(k)] = src.values[static_cast<std::size_t>(6 + k)];
    }
    for (int k = 0; k < 3; ++k) {
        door.closed[static_cast<std::size_t>(k)] =
            src.values[static_cast<std::size_t>(k)] + door.size[static_cast<std::size_t>(k)];
        door.open[static_cast<std::size_t>(k)] =
            src.values[static_cast<std::size_t>(3 + k)] + door.size[static_cast<std::size_t>(k)];
        // A1..A3 are radians; the record keeps them divided by 65535.
        door.axis[static_cast<std::size_t>(k)] =
            src.angles[static_cast<std::size_t>(k)] * kDoorPhaseMaxToFraction;
    }
    door.sector = src.params[0];
    door.object = src.params[1];
    door.step = src.params[2];
    if (door.step < 0) {
        door.step = 0;
    }
    // The scene layer's ``state`` tuple is (state word, phase); +0x80/+0x8c.
    door.state = static_cast<DoorState>(std::clamp(src.params[4], -1, 1));
    door.phase = clamp_phase(src.params[3]);
    door.phase_prev = door.phase;
    door.phase_next = door.phase;
    door.hold_ticks = static_cast<std::uint32_t>(std::max(src.params[5], 0));
    door.timer = src.params[6] > 0 ? static_cast<std::uint32_t>(src.params[6]) : 0u;
    door.start_sound = {src.tail[1], src.tail[0]}; // n0/v0
    door.move_sound = {src.tail[3], src.tail[2]};  // n1/v1
    door.stop_sound = {src.tail[5], src.tail[4]};  // n2/v2
    door.event_open = door.ids[3];
    door.event_close = door.ids[4];
    door.sliding = false;
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(door.open[static_cast<std::size_t>(k)] -
                      door.closed[static_cast<std::size_t>(k)]) > 1e-6) {
            door.sliding = true;
        }
    }
    door.linked = door.ids[3] != 0;
    return door;
}

int effective_door_id(const std::array<int, 5>& ids) {
    for (int k = 0; k < 3; ++k) {
        if (ids[static_cast<std::size_t>(k)] != 0) {
            return ids[static_cast<std::size_t>(k)];
        }
    }
    return ids[0];
}

void finalise_door(Door& door) {
    door.id = effective_door_id(door.ids);
    door.phase = std::min(door.phase, kDoorPhaseFull);
    door.phase_prev = door.phase;
    door.phase_next = door.engine_phase();
    door.open_fraction = static_cast<double>(door.phase) * kDoorPhaseToFraction;
    if (!all_finite(door.closed) || !all_finite(door.open) || !all_finite(door.size)) {
        door.closed = {0.0, 0.0, 0.0};
        door.open = {0.0, 0.0, 0.0};
        door.size = {0.0, 0.0, 0.0};
    }
    if (!all_finite(door.axis) || !std::isfinite(door.sound_level)) {
        // A non-finite angle operand would poison the basis; the engine reads
        // it blindly, the model refuses to.
        door.axis = {0.0, 0.0, 0.0};
    }
    door.event_open = door.ids[3];
    door.event_close = door.ids[4];
}

// --- ADD_LIFT ---------------------------------------------------------------
//
// ``0x45c9a0`` hands the 37-dword block to ``0x44e800`` as cdecl operands.
// Operands 9..16 are **doubles** (2 dwords each: the six basis values), so the
// integer words shift by 8 relative to a naive one-dword-per-operand reading:
//
//   head[0] kind        head[1] SOB index     head[2..4] x/y/z
//   head[5..7] pos delta (scaled by height)   head[8..19] six doubles
//   head[20] speed      head[21] dwell        head[22] height  head[23] wait
//   head[24] direction  head[25] stop index   head[26..28] call ids
//   head[29] call event head[30] call radius
//   head[31..36] V,N,V,N,V,N: the three sound (volume, sample) pairs.
//
// ``docs/DOOR_FIELDS.md`` §3.2 used to claim ``head[16] = speed``; that word is
// the low half of the fourth basis double (e.g. 1413754135 in dm3).

void collect_lifts(const std::vector<SbtInsn>& insns, const std::vector<std::uint8_t>& code,
                   std::vector<Lift>& lifts) {
    for (const SbtInsn& insn : insns) {
        if (insn.name == "ADD_LIFT") {
            Lift lift;
            lift.script = true;
            lift.id = static_cast<int>(lifts.size());
            const std::size_t head = static_cast<std::size_t>(insn.offset) + 1;
            if (head <= code.size() && code.size() - head >= 0x94) {
                for (std::size_t k = 0; k < lift.head.size(); ++k) {
                    std::memcpy(&lift.head[k], code.data() + head + k * 4, 4);
                }
                lift.kind = as_i32(lift.head[0]);
                lift.sob = as_i32(lift.head[1]);
                lift.x = as_i32(lift.head[2]);
                lift.y = as_i32(lift.head[3]);
                lift.z = as_i32(lift.head[4]);
                for (int k = 0; k < 3; ++k) {
                    lift.pos_delta[static_cast<std::size_t>(k)] =
                        as_i32(lift.head[static_cast<std::size_t>(5 + k)]);
                }
                for (int k = 0; k < 3; ++k) {
                    std::memcpy(&lift.basis[static_cast<std::size_t>(k)],
                                &lift.head[static_cast<std::size_t>(8 + k * 2)], 8);
                }
                for (int k = 0; k < 3; ++k) {
                    std::uint64_t bits = 0;
                    std::memcpy(&bits, &lift.head[static_cast<std::size_t>(14 + k * 2)], 8);
                    double angle = 0.0;
                    std::memcpy(&angle, &bits, 8);
                    lift.basis[static_cast<std::size_t>(3 + k)] =
                        angle * kDoorPhaseMaxToFraction;
                }
                lift.speed = std::max(as_i32(lift.head[20]), 0);
                lift.dwell = as_i32(lift.head[21]);
                lift.height = as_i32(lift.head[22]);
                lift.wait = as_i32(lift.head[23]);
                lift.direction = as_i32(lift.head[24]);
                lift.stop_index = as_i32(lift.head[25]);
                for (int k = 0; k < 3; ++k) {
                    lift.call_ids[static_cast<std::size_t>(k)] =
                        as_i32(lift.head[static_cast<std::size_t>(26 + k)]);
                }
                lift.call_event = as_i32(lift.head[29]);
                lift.call_radius = as_i32(lift.head[30]);
                lift.call_sound = {as_i32(lift.head[32]), as_i32(lift.head[31])};
                lift.move_sound = {as_i32(lift.head[34]), as_i32(lift.head[33])};
                lift.arrive_sound = {as_i32(lift.head[36]), as_i32(lift.head[35])};
                lift.height_tick = lift.height;
                lift.target = lift.height;
            }
            const std::size_t count = std::min<std::size_t>(insn.count,
                                                           insn.ints.size() / 3);
            lift.stops.reserve(count);
            for (std::size_t k = 0; k < count; ++k) {
                LiftStop stop;
                stop.floor_id = as_i32(insn.ints[k * 3]);
                stop.height = as_i32(insn.ints[k * 3 + 1]);
                stop.sound = as_i32(insn.ints[k * 3 + 2]);
                lift.stops.push_back(stop);
            }
            lifts.push_back(std::move(lift));
        }
        collect_lifts(insn.body, code, lifts);
    }
}

// --- record 18 -> Lift ------------------------------------------------------

bool parse_lift_record(const std::vector<std::uint8_t>& payload, std::size_t offset,
                       std::size_t index, Lift& lift) {
    if (offset > payload.size() || payload.size() - offset < 0xA8) {
        return false;
    }
    lift.id = static_cast<int>(index);
    if (!read_i32(payload, offset + 0x34, lift.kind) ||
        !read_i32(payload, offset + 0x38, lift.sob) ||
        !read_i32(payload, offset + 0x3C, lift.x) ||
        !read_i32(payload, offset + 0x40, lift.y) ||
        !read_i32(payload, offset + 0x44, lift.z) ||
        !read_i32(payload, offset + 0x54, lift.speed) ||
        !read_i32(payload, offset + 0x58, lift.dwell) ||
        !read_i32(payload, offset + 0x5C, lift.height) ||
        !read_i32(payload, offset + 0x60, lift.height_tick) ||
        !read_i32(payload, offset + 0x64, lift.wait) ||
        !read_i32(payload, offset + 0x68, lift.direction) ||
        !read_i32(payload, offset + 0x6C, lift.stop_index) ||
        !read_i32(payload, offset + 0x7C, lift.call_event) ||
        !read_i32(payload, offset + 0x80, lift.call_radius)) {
        return false;
    }
    for (int k = 0; k < 3; ++k) {
        if (!read_i32(payload, offset + 0x48 + static_cast<std::size_t>(k) * 4,
                      lift.pos_delta[static_cast<std::size_t>(k)]) ||
            !read_i32(payload, offset + 0x70 + static_cast<std::size_t>(k) * 4,
                      lift.call_ids[static_cast<std::size_t>(k)])) {
            return false;
        }
    }
    for (int k = 0; k < 6; ++k) {
        if (!read_f64(payload, offset + static_cast<std::size_t>(k) * 8,
                      lift.basis[static_cast<std::size_t>(k)])) {
            return false;
        }
    }
    int volume_a = 0;
    int volume_b = 0;
    int volume_c = 0;
    int sample_a = -1;
    int sample_b = -1;
    int sample_c = -1;
    if (!read_i32(payload, offset + 0x84, volume_a) ||
        !read_i32(payload, offset + 0x88, volume_b) ||
        !read_i32(payload, offset + 0x8C, volume_c) ||
        !read_i32(payload, offset + 0x90, sample_a) ||
        !read_i32(payload, offset + 0x94, sample_b) ||
        !read_i32(payload, offset + 0x98, sample_c)) {
        return false;
    }
    lift.call_sound = {sample_a, volume_a};
    lift.move_sound = {sample_b, volume_b};
    lift.arrive_sound = {sample_c, volume_c};
    int moving = 0;
    if (!read_i32(payload, offset + 0x68, moving)) {
        return false;
    }
    lift.moving = moving != 0;
    lift.target = lift.height_tick;
    if (lift.speed < 0) {
        lift.speed = 0;
    }
    return true;
}

} // namespace

const char* door_state_name(DoorState state) {
    switch (state) {
    case DoorState::Closing:
        return "closing";
    case DoorState::Opening:
        return "opening";
    case DoorState::Closed:
    default:
        return "closed";
    }
}

// --- leaf / cabin geometry --------------------------------------------------
//
// Evidence: the tick 0x44f2e0, the commit 0x44f3f0, the transform update
// 0x44dfc0 (door) and the cabin update 0x44e6f0 (lift).  Both funnel into
// generic transform writers: 0x434887 (object origin/basis) and 0x41e025
// (basis from three angles).  See docs/DOOR_FIELDS.md §1.2 and §3.3.

namespace {

/// sin/cos wrappers of the engine (0x480314 = FSIN, 0x480954 = FCOS).
double eng_sin(double v) { return std::sin(v); }
double eng_cos(double v) { return std::cos(v); }

} // namespace

int door_rotation_axis(const Door& door) {
    // 0x44dfc0 tests delta[2] first, then delta[0]; delta[1] is the fallback.
    if (door.axis[2] != 0.0) {
        return 2;
    }
    if (door.axis[0] != 0.0) {
        return 0;
    }
    if (door.axis[1] != 0.0) {
        return 1;
    }
    return -1;
}

DoorTransform door_transform_at(const Door& door, std::uint32_t engine_phase) {
    DoorTransform transform;
    const std::uint32_t phase = std::min(engine_phase, kDoorPhaseMax);
    const double fraction = static_cast<double>(phase) * kDoorPhaseMaxToFraction;
    for (std::size_t k = 0; k < 3; ++k) {
        transform.center[k] =
            door.closed[k] + (door.open[k] - door.closed[k]) * fraction;
    }

    // Identity unless one of the three angle operands is non-zero.
    transform.basis = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    const int axis = door_rotation_axis(door);
    transform.axis = axis;
    if (axis >= 0) {
        // Unit axis as 0x44dfc0 passes it: -x, +y, -z.
        const double unit[3] = {axis == 0 ? -1.0 : 0.0, axis == 1 ? 1.0 : 0.0,
                                axis == 2 ? -1.0 : 0.0};
        const double angle = door.axis[static_cast<std::size_t>(axis)] *
                             static_cast<double>(phase);
        transform.angle = angle;
        const double c = eng_cos(angle);
        const double s = eng_sin(angle);
        const double t = 1.0 - c;
        const double x = unit[0];
        const double y = unit[1];
        const double z = unit[2];
        // 0x434887 ("angle -angle"): the matrix it builds is R(-angle)
        // about the unit axis, written row-major.
        transform.basis[0] = x * x * t + c;
        transform.basis[1] = x * y * t + z * s;
        transform.basis[2] = x * z * t - y * s;
        transform.basis[3] = x * y * t - z * s;
        transform.basis[4] = y * y * t + c;
        transform.basis[5] = y * z * t + x * s;
        transform.basis[6] = x * z * t + y * s;
        transform.basis[7] = y * z * t - x * s;
        transform.basis[8] = z * z * t + c;
    }
    // origin = center - basis^T * size (the three columns of `basis`).
    for (std::size_t row = 0; row < 3; ++row) {
        double shifted = 0.0;
        for (std::size_t k = 0; k < 3; ++k) {
            shifted += door.size[k] * transform.basis[k * 3 + row];
        }
        transform.origin[row] = transform.center[row] - shifted;
    }
    return transform;
}

DoorTransform door_transform(const Door& door) {
    return door_transform_at(door, door.engine_phase());
}

std::array<double, 3> door_motion_delta(const Door& door, std::uint32_t phase_prev,
                                        std::uint32_t phase_next) {
    const std::int64_t delta = static_cast<std::int64_t>(
                                   std::min(phase_next, kDoorPhaseMax)) -
                               static_cast<std::int64_t>(
                                   std::min(phase_prev, kDoorPhaseMax));
    const double fraction = static_cast<double>(delta) * kDoorPhaseMaxToFraction;
    return {(door.open[0] - door.closed[0]) * fraction,
            (door.open[1] - door.closed[1]) * fraction,
            (door.open[2] - door.closed[2]) * fraction};
}

double lift_fraction(const Lift& lift) {
    const int height = std::clamp(lift.height, 0, static_cast<int>(kDoorPhaseMax));
    return static_cast<double>(height) * kDoorPhaseMaxToFraction;
}

LiftTransform lift_transform(const Lift& lift) {
    LiftTransform transform;
    const double fraction = lift_fraction(lift);
    const int base[3] = {lift.x, lift.y, lift.z};
    for (std::size_t k = 0; k < 3; ++k) {
        transform.origin[k] = static_cast<double>(base[k]) +
                              static_cast<double>(lift.pos_delta[k]) * fraction;
    }
    // 0x44e6f0 combines the (base, delta) pairs into three angles and hands
    // them to 0x41e025 in the order (value[1], value[0], value[2]).
    const double value[3] = {lift.basis[0] + lift.basis[3] * fraction,
                             lift.basis[1] + lift.basis[4] * fraction,
                             lift.basis[2] + lift.basis[5] * fraction};
    transform.angles = {value[1], value[0], value[2]};
    const double s1 = eng_sin(transform.angles[0]);
    const double c1 = eng_cos(transform.angles[0]);
    const double s2 = eng_sin(transform.angles[1]);
    const double c2 = eng_cos(transform.angles[1]);
    const double s3 = eng_sin(transform.angles[2]);
    const double c3 = eng_cos(transform.angles[2]);
    transform.basis[0] = c3 * c1 - s3 * s2 * s1;
    transform.basis[1] = -s3 * c1 - c3 * s2 * s1;
    transform.basis[2] = -c2 * s1;
    transform.basis[3] = s3 * c2;
    transform.basis[4] = c3 * c2;
    transform.basis[5] = -s2;
    transform.basis[6] = s3 * s2 * c1 + c3 * s1;
    transform.basis[7] = c3 * s2 * c1 - s3 * s1;
    transform.basis[8] = c2 * c1;
    return transform;
}

// --- WorldState -------------------------------------------------------------

void WorldState::clear() {
    doors_.clear();
    switches_.clear();
    lifts_.clear();
    events_.clear();
    switch_event_sources_.clear();
    switch_event_cursor_ = 0;
    switch_post_blocked_ = false;
    door_targets_.clear();
    lift_targets_.clear();
    sound_requests_.clear();
    sound_cursor_ = 0;
    stats_ = WorldStats{};
}

void WorldState::log_event(WorldEvent event) {
    events_.push_back(event);
    switch_event_sources_.push_back(std::nullopt);
}

// --- sound requests ----------------------------------------------------------
//
// The engine does not mix anything here: it fills a 3D-sound queue
// (`0x62ce18`, stride `0x2c`, at most 256 entries) and the mixer
// (`0x46bed0`) resolves the entries later.  The model does the same thing
// with a plain vector: the world only *describes* what has to sound, the
// consumer owns the audio.

void WorldState::queue_sound(SoundRequest request) {
    if (request.source != SoundSource::DoorMoveStop &&
        (request.sample < 0 || request.sample >= kDoorSampleCount)) {
        // The engine queues the request with any sample and lets the mixer
        // drop it (`0x46b840`/the mixer resolve the index through
        // `0x4ca720`).  A portable queue must not carry a broken index.
        return;
    }
    if (sound_requests_.size() >= kMaxSoundRequests) {
        ++stats_.sounds_dropped;
        return;
    }
    sound_requests_.push_back(request);
    ++stats_.sounds_queued;
}

void WorldState::queue_door_sound(std::size_t index, const Door& door, SoundSource source,
                                  const DoorSound& sound) {
    if (!door.bound) {
        // 0x434419(sector, object) must return the leaf object, otherwise the
        // engine plays nothing at all.
        return;
    }
    SoundRequest request;
    request.sample = sound.sample;
    request.volume = sound.volume;
    // The engine reads the leaf's origin (object `+0x00/+0x08/+0x10`) and
    // truncates it to int (0x40e0e0).  `door_transform` is exactly what
    // 0x44dfc0 -> 0x434887 wrote there.
    request.position = door_transform(door).origin;
    request.loop = source == SoundSource::DoorMove;
    request.source = source;
    request.class_id = request.loop ? static_cast<int>(kDoorSoundClassBase) +
                                          static_cast<int>(index)
                                    : 0;
    queue_sound(request);
}

void WorldState::queue_door_move_stop(std::size_t index, const Door& door) {
    if (door.move_sound.silent()) {
        // No loop was ever started for this door; the engine still sends the
        // stop request every idle tick (0x46ba90 is unconditional), but a
        // portable queue must not carry a request for a sound that cannot
        // exist.
        return;
    }
    SoundRequest request;
    request.sample = -1;
    request.volume = 0;
    request.loop = true;
    request.source = SoundSource::DoorMoveStop;
    request.class_id = static_cast<int>(kDoorSoundClassBase) + static_cast<int>(index);
    queue_sound(request);
}

void WorldState::queue_switch_sound(int switch_id) {
    if (switch_id < 0 || static_cast<std::size_t>(switch_id) >= switches_.size()) {
        return;
    }
    const Switch& sw = switches_[static_cast<std::size_t>(switch_id)];
    if (sw.sound < 0) {
        // 0x44f9ca: a negative sound means silence.
        return;
    }
    SoundRequest request;
    request.sample = sw.sound;
    request.volume = sw.aux;
    request.source = SoundSource::Switch;
    if (sw.kind == 0) {
        // kind 0 stores a world position in the operands.
        request.position = {static_cast<double>(sw.pos[0]), static_cast<double>(sw.pos[1]),
                            static_cast<double>(sw.pos[2])};
        request.position_known = true;
    } else if (sw.world_position.has_value()) {
        // kind >= 0x101: the centroid resolved at build time (0x43577c).
        request.position = *sw.world_position;
        request.position_known = true;
    } else {
        // No such polygon in the `.sbw` (or no geometry at all): the engine
        // would still queue the centroid, the model cannot.
        request.position_known = false;
    }
    queue_sound(request);
}

void WorldState::add_door(Door door) {
    finalise_door(door);
    doors_.push_back(std::move(door));
}

void WorldState::rebuild_indexes() {
    door_targets_.clear();
    lift_targets_.clear();
    for (std::size_t k = 0; k < doors_.size(); ++k) {
        const Door& door = doors_[k];
        for (int i = 0; i < 3; ++i) {
            const int alias = door.ids[static_cast<std::size_t>(i)];
            if (alias == 0) {
                continue;
            }
            auto it = std::find_if(door_targets_.begin(), door_targets_.end(),
                                   [alias](const auto& row) { return row.first == alias; });
            if (it == door_targets_.end()) {
                door_targets_.push_back({alias, {k}});
            } else if (std::find(it->second.begin(), it->second.end(), k) == it->second.end()) {
                it->second.push_back(k);
            }
        }
    }
    for (std::size_t k = 0; k < lifts_.size(); ++k) {
        const Lift& lift = lifts_[k];
        if (lift.id != 0) {
            lift_targets_.push_back({lift.id, k});
        }
        for (int call : lift.call_ids) {
            if (call != 0) {
                lift_targets_.push_back({call, k});
            }
        }
    }
    std::set<std::pair<int, int>> distinct;
    stats_.doors_bound = 0;
    for (const Door& door : doors_) {
        if (door.bound) {
            ++stats_.doors_bound;
            distinct.insert({door.sector, door.object});
        }
    }
    stats_.doors_distinct_objects = distinct.size();
    stats_.doors = doors_.size();
    stats_.doors_sliding = static_cast<std::size_t>(
        std::count_if(doors_.begin(), doors_.end(), [](const Door& d) { return d.sliding; }));
    stats_.doors_linked = static_cast<std::size_t>(
        std::count_if(doors_.begin(), doors_.end(), [](const Door& d) { return d.linked; }));
    stats_.switches = switches_.size();
    stats_.lifts = lifts_.size();
}

bool WorldState::build(const SbtLevel& level, const SbwLevel& world, std::string* error) {
    clear();
    if (error != nullptr) {
        error->clear();
    }

    const std::vector<std::uint8_t>* door_record = nullptr;
    const std::vector<std::uint8_t>* switch_record = nullptr;
    const std::vector<std::uint8_t>* lift_record = nullptr;
    for (const SbtRecord& record : level.records) {
        if (record.type == 2 && door_record == nullptr) {
            door_record = &record.payload;
        } else if (record.type == 1 && switch_record == nullptr) {
            switch_record = &record.payload;
        } else if (record.type == 18 && lift_record == nullptr) {
            lift_record = &record.payload;
        }
    }

    // Shipped levels keep the command stream in SCRIPT (record 11); save-like
    // files carry the runtime arrays directly.  Both are accepted.
    std::optional<Scene> scene;
    if (!level.script_code.empty()) {
        std::string scene_error;
        scene = extract_scene(level, scene_error, false);
        if (!scene && error != nullptr && door_record == nullptr && switch_record == nullptr) {
            *error = scene_error;
        }
    }

    if (door_record != nullptr) {
        std::uint32_t count = 0;
        if (!read_u32(*door_record, 0, count)) {
            ++stats_.bad_records;
        } else {
            const std::size_t available = (door_record->size() - 4) / 0xB8;
            if (count > available || count > 100) {
                ++stats_.bad_records;
            }
            const std::size_t n = std::min<std::size_t>(
                std::min<std::uint32_t>(count, 100), available);
            for (std::size_t k = 0; k < n; ++k) {
                Door door;
                if (parse_door_record(*door_record, 4 + k * 0xB8, door)) {
                    add_door(std::move(door));
                } else {
                    ++stats_.bad_records;
                }
            }
            stats_.record_doors = doors_.size();
        }
    } else if (scene) {
        for (const SceneDoor& src : scene->doors) {
            Door door = door_from_scene(src);
            add_door(std::move(door));
            ++stats_.script_doors;
        }
    }

    if (switch_record != nullptr) {
        std::uint32_t count = 0;
        if (!read_u32(*switch_record, 0, count)) {
            ++stats_.bad_records;
        } else {
            const std::size_t available = (switch_record->size() - 4) / 0x20;
            if (count > available || count > 512) {
                ++stats_.bad_records;
            }
            const std::size_t n = std::min<std::size_t>(
                std::min<std::uint32_t>(count, 512), available);
            for (std::size_t k = 0; k < n; ++k) {
                Switch sw;
                sw.id = static_cast<int>(k);
                const std::size_t offset = 4 + k * 0x20;
                if (!read_i32(*switch_record, offset, sw.kind) ||
                    !read_i32(*switch_record, offset + 4, sw.target) ||
                    !read_i32(*switch_record, offset + 8, sw.radius)) {
                    ++stats_.bad_records;
                    continue;
                }
                for (int axis = 0; axis < 3; ++axis) {
                    if (!read_i32(*switch_record, offset + 12 +
                                      static_cast<std::size_t>(axis) * 4,
                                  sw.pos[static_cast<std::size_t>(axis)])) {
                        ++stats_.bad_records;
                        break;
                    }
                }
                read_i32(*switch_record, offset + 0x18, sw.aux);
                read_i32(*switch_record, offset + 0x1C, sw.sound);
                switches_.push_back(std::move(sw));
            }
            stats_.record_switches = switches_.size();
        }
    } else if (scene) {
        for (const SceneSwitch& src : scene->switches) {
            Switch sw;
            sw.id = static_cast<int>(switches_.size());
            sw.target = src.id;
            sw.kind = src.kind;
            sw.radius = src.radius;
            sw.pos = src.pos;
            sw.aux = src.extra[0];
            sw.sound = src.extra[1];
            switches_.push_back(std::move(sw));
            ++stats_.script_switches;
        }
    }

    if (lift_record != nullptr) {
        std::uint32_t count = 0;
        if (!read_u32(*lift_record, 0, count)) {
            ++stats_.bad_records;
        } else {
            const std::size_t available = (lift_record->size() - 4) / 0xA8;
            if (count > available || count > 30) {
                ++stats_.bad_records;
            }
            const std::size_t n = std::min<std::size_t>(
                std::min<std::uint32_t>(count, 30), available);
            const std::size_t columns_offset = 4 + n * 0xA8;
            std::uint32_t column_count = 0;
            const bool has_columns =
                read_u32(*lift_record, columns_offset, column_count) &&
                column_count <= 200 &&
                lift_record->size() - columns_offset - 4 >=
                    static_cast<std::size_t>(column_count) * 12;
            if (!has_columns) {
                ++stats_.bad_records;
            }
            for (std::size_t k = 0; k < n; ++k) {
                Lift lift;
                // parse_lift_record needs the record offset; store it in the
                // otherwise-unused tail word so the helper stays simple.
                if (!parse_lift_record(*lift_record, 4 + k * 0xA8, k, lift)) {
                    ++stats_.bad_records;
                    continue;
                }
                if (has_columns) {
                    const std::size_t col = columns_offset + 4;
                    const std::size_t count_col = column_count;
                    int stop_count = 0;
                    int stop_start = 0;
                    if (read_i32(*lift_record, 4 + k * 0xA8 + 0x9C, stop_count) &&
                        read_i32(*lift_record, 4 + k * 0xA8 + 0xA0, stop_start) &&
                        stop_count >= 0 && stop_start >= 0 &&
                        static_cast<std::size_t>(stop_start + stop_count) <= count_col) {
                        for (int s = 0; s < stop_count; ++s) {
                            const std::size_t index = static_cast<std::size_t>(stop_start + s);
                            LiftStop stop;
                            read_i32(*lift_record, col + index * 4, stop.floor_id);
                            read_i32(*lift_record, col + count_col * 4 + index * 4,
                                     stop.height);
                            read_i32(*lift_record, col + count_col * 8 + index * 4,
                                     stop.sound);
                            lift.stops.push_back(stop);
                        }
                    } else {
                        ++stats_.bad_records;
                    }
                }
                if (!lift.stops.empty()) {
                    lift.stop_index = std::clamp(
                        lift.stop_index, 0, static_cast<int>(lift.stops.size()) - 1);
                } else {
                    lift.stop_index = 0;
                }
                lifts_.push_back(std::move(lift));
            }
            stats_.record_lifts = lifts_.size();
        }
    } else if (!level.script_code.empty()) {
        std::string decode_error;
        const std::optional<SbtProgram> program = decode_sbt_script(level.script_code,
                                                                    decode_error);
        if (program) {
            collect_lifts(program->insns, level.script_code, lifts_);
            stats_.script_lifts = lifts_.size();
        } else {
            ++stats_.bad_records;
            if (error != nullptr && error->empty()) {
                *error = decode_error;
            }
        }
    }

    // Bind doors to the .sbw object table.  This is the proven 0x434419
    // (sector, object) lookup, done once at build time.
    const auto& sectors = world.header.sectors;
    for (Door& door : doors_) {
        const bool sector_ok = door.sector >= 0 &&
                               static_cast<std::size_t>(door.sector) < sectors.size();
        const bool object_ok =
            sector_ok && door.object >= 0 &&
            static_cast<std::size_t>(door.object) <
                sectors[static_cast<std::size_t>(door.sector)].objects.size();
        door.bound = sector_ok && object_ok;
        if (!door.bound) {
            ++stats_.bad_bindings;
        }
        door.phase_prev = door.phase;
        door.phase_next = door.engine_phase();
        door.open_fraction = static_cast<double>(door.phase) * kDoorPhaseToFraction;
    }

    // `0x43577c` for kind >= 0x101: the switch sound plays at the polygon
    // centroid, so resolve the triple once from the `.sbw` geometry.
    for (Switch& sw : switches_) {
        if (!sw.is_poly()) {
            continue;
        }
        std::array<double, 3> centroid{};
        if (polygon_centroid(world, sw.pos[0], sw.pos[1], sw.pos[2], centroid)) {
            sw.world_position = centroid;
        }
    }

    rebuild_indexes();

    if (doors_.empty() && switches_.empty() && lifts_.empty() && !scene &&
        door_record == nullptr && switch_record == nullptr && lift_record == nullptr) {
        if (error != nullptr && error->empty()) {
            *error = "no DOOR/SWITCH/LIFT records and no SCRIPT bytecode";
        }
        return false;
    }
    return true;
}

void WorldState::start_lift(Lift& lift) {
    if (lift.stops.size() < 2 || lift.speed <= 0) {
        return;
    }
    std::size_t next = static_cast<std::size_t>(std::max(lift.stop_index, 0)) + 1;
    if (next >= lift.stops.size()) {
        next = 0;
    }
    lift.stop_index = static_cast<int>(next);
    lift.target = lift.stops[next].height;
    const std::int64_t distance = static_cast<std::int64_t>(lift.target) - lift.height;
    lift.direction = distance == 0 ? 0 : (distance > 0 ? 1 : -1);
    lift.moving = lift.direction != 0;
    lift.wait = 0;
    lift.height_tick = lift.height;
}

std::size_t WorldState::post_switch_events(EventQueue& queue,
                                           std::int16_t source) const {
    if (switch_event_cursor_ > events_.size()) {
        switch_event_cursor_ = events_.size();
    }
    if (door_event_cursor_ > events_.size()) {
        door_event_cursor_ = events_.size();
    }
    if (switch_event_sources_.size() != events_.size()) {
        switch_event_sources_.resize(events_.size());
    }
    if (switch_event_cursor_ >= events_.size()) {
        switch_post_blocked_ = false;
        return 0;
    }
    // Bind the source before walking: a full queue stops the walk at the first
    // pending record, but the records that arrived with *this* call still learn
    // their own source instead of inheriting an earlier caller's.
    for (std::size_t i = switch_event_cursor_; i < events_.size(); ++i) {
        if (!switch_event_sources_[i].has_value()) {
            switch_event_sources_[i] = source;
        }
    }
    std::size_t posted = 0;
    while (switch_event_cursor_ < events_.size()) {
        const WorldEvent& event = events_[switch_event_cursor_];
        if (event.kind != WorldEvent::Kind::SwitchPressed || event.switch_id < 0 ||
            event.switch_id >= 0x200) {
            // Invalid ids are never postable: advance instead of treating the
            // permanent rejection as a full queue and stalling the cursor.
            ++switch_event_cursor_;
            continue;
        }
        const std::int16_t event_source =
            switch_event_sources_[switch_event_cursor_].value_or(source);
        if (!queue.post_switch(static_cast<std::uint32_t>(event.switch_id),
                               event_source)) {
            switch_post_blocked_ = true;
            break; // full queue: keep the cursor on the pending record
        }
        ++posted;
        ++switch_event_cursor_;
    }
    if (switch_event_cursor_ >= events_.size()) {
        switch_post_blocked_ = false;
    }
    return posted;
}

bool WorldState::interact(int switch_id) {
    if (switch_id < 0 || static_cast<std::size_t>(switch_id) >= switches_.size()) {
        return false;
    }
    Switch& sw = switches_[static_cast<std::size_t>(switch_id)];
    sw.pressed = true;
    ++stats_.switch_interactions;
    log_event({WorldEvent::Kind::SwitchPressed, sw.id, sw.target, 0, 0});
    press_target(sw.id, sw.target);
    return true;
}

bool WorldState::interact(int switch_id, std::int16_t source) {
    if (switch_id < 0 || static_cast<std::size_t>(switch_id) >= switches_.size()) {
        return false;
    }
    Switch& sw = switches_[static_cast<std::size_t>(switch_id)];
    sw.pressed = true;
    ++stats_.switch_interactions;
    log_event({WorldEvent::Kind::SwitchPressed, sw.id, sw.target, 0, 0});
    // Bind the source that 0x479810 puts inside the type-2 record right away;
    // everything logged before this call already has its own answer.
    switch_event_sources_.back() = source;
    press_target(sw.id, sw.target);
    return true;
}

std::size_t WorldState::press_target(int switch_id, int target) {
    std::size_t acted = 0;
    // 0x44f960 plays the switch sound right after the generate event, no
    // matter whether the target matched a door, a lift or nothing at all.
    queue_switch_sound(switch_id);
    for (const auto& row : door_targets_) {
        if (row.first != target) {
            continue;
        }
        for (std::size_t index : row.second) {
            Door& door = doors_[index];
            if (door.locked) {
                ++stats_.locked_hits;
                log_event({WorldEvent::Kind::DoorLocked, switch_id, target, door.id, 0});
                ++acted;
                continue;
            }
            if (!door.active()) {
                continue; // 0x44f690 skips kind 0 doors entirely
            }
            ++acted;
            door.timer = 0;
            bool opening = true;
            if (door.state == DoorState::Opening) {
                opening = false;
            } else if (door.state == DoorState::Closing) {
                opening = true;
            } else {
                opening = door.phase < kDoorPhaseFull;
            }
            door.state = opening ? DoorState::Opening : DoorState::Closing;
            // 0x44f690 plays the *start* sound (v0/n0) when a GENERATE id
            // matches a door; 0x44f3f0 plays the loop separately.
            WorldEvent event{opening ? WorldEvent::Kind::DoorOpening
                                     : WorldEvent::Kind::DoorClosing,
                             switch_id, target, door.id, 0,
                             door.active() ? door.start_sound.sample : -1,
                             door.start_sound.volume};
            log_event(event);
            // 0x44f690 (state flip) plays the start slot at the leaf's
            // origin; the looped movement slot follows with the next tick
            // (0x44f3f0) and carries the class `1000000 + door index`.
            queue_door_sound(index, door, SoundSource::DoorStart, door.start_sound);
            queue_door_sound(index, door, SoundSource::DoorMove, door.move_sound);
        }
    }
    for (const auto& row : lift_targets_) {
        if (row.first != target) {
            continue;
        }
        Lift& lift = lifts_[row.second];
        if (!lift.moving) {
            start_lift(lift);
            ++stats_.lift_calls;
            log_event({WorldEvent::Kind::LiftCalled, switch_id, target, 0, lift.id,
                       lift.call_sound.sample, lift.call_sound.volume});
            ++acted;
        }
    }
    if (acted == 0) {
        ++stats_.unmatched_targets;
        log_event({WorldEvent::Kind::NoTarget, switch_id, target, 0, 0});
    }
    return acted;
}

namespace {

/// The engine's proximity metric (``0x4344df``): ``max + (mid + min) / 4``
/// over the absolute components.  Not Euclidean — an octagon.
double trigger_distance(double dx, double dy, double dz) {
    double a = std::fabs(dx);
    double b = std::fabs(dy);
    double c = std::fabs(dz);
    if (a < b) {
        std::swap(a, b);
    }
    if (a < c) {
        std::swap(a, c);
    }
    // a = max, b/c = the other two in some order.
    return a + (b + c) * 0.25;
}

} // namespace

std::size_t WorldState::poll_triggers(const std::array<double, 3>& pos,
                                      std::int16_t source) {
    std::size_t fired = 0;
    for (Switch& sw : switches_) {
        bool inside = false;
        if (sw.kind == 0) {
            inside = trigger_distance(pos[0] - sw.pos[0], pos[1] - sw.pos[1],
                                      pos[2] - sw.pos[2]) < static_cast<double>(sw.radius);
        } else if (sw.kind == 1) {
            // 0x44fc30 kind 1: distance to the *leaf origin* of the first door
            // matching ids[0..2]; no door, no trigger.
            const Door* target_door = nullptr;
            for (const auto& row : door_targets_) {
                if (row.first != sw.target || row.second.empty()) {
                    continue;
                }
                target_door = &doors_[row.second.front()];
                break;
            }
            if (target_door != nullptr) {
                const DoorTransform transform = door_transform(*target_door);
                inside = trigger_distance(pos[0] - transform.origin[0],
                                          pos[1] - transform.origin[1],
                                          pos[2] - transform.origin[2]) <
                         static_cast<double>(sw.radius);
            }
        }
        sw.inside = inside;
        if (!inside) {
            continue;
        }
        sw.pressed = true;
        ++stats_.switch_interactions;
        ++stats_.trigger_presses;
        log_event({WorldEvent::Kind::SwitchPressed, sw.id, sw.target, 0, 0});
        switch_event_sources_.back() = source;
        press_target(sw.id, sw.target); // counts the miss itself
        ++fired;
    }
    return fired;
}

std::size_t WorldState::press_polygon(const PolygonHit& hit, std::int16_t source,
                                      std::span<const int> kinds,
                                      std::size_t& counter, bool object_gate) {
    std::size_t fired = 0;
    for (Switch& sw : switches_) {
        if (std::find(kinds.begin(), kinds.end(), sw.kind) == kinds.end()) {
            continue;
        }
        // 0x428399(sector, object) returns `object+0x70 & 0x10000`; the
        // blast path requires zero (0x44fb63).  The aim ray has no such gate.
        if (object_gate && (hit.object_flags & 0x10000u) != 0) {
            continue;
        }
        if (sw.pos[0] != static_cast<int>(hit.sector) ||
            sw.pos[1] != static_cast<int>(hit.object) ||
            sw.pos[2] != static_cast<int>(hit.polygon)) {
            continue;
        }
        // 0x44fc30 (`local_58 <= radius`) and 0x44fb30 (`fVar4 <= radius`)
        // both use `<=`, unlike the strict `<` of the octagonal proximity.
        if (!(hit.distance <= static_cast<double>(sw.radius))) {
            continue;
        }
        sw.pressed = true;
        sw.inside = true;
        ++counter;
        log_event({WorldEvent::Kind::SwitchPressed, sw.id, sw.target, 0, 0});
        switch_event_sources_.back() = source;
        press_target(sw.id, sw.target);
        ++fired;
    }
    return fired;
}

std::size_t WorldState::poll_aim(const PolygonHit& hit, std::int16_t source) {
    static constexpr std::array<int, 1> kKinds{0x101};
    const std::size_t fired = press_polygon(hit, source, kKinds, stats_.aim_presses, false);
    stats_.switch_interactions += fired;
    stats_.trigger_presses += fired;
    return fired;
}

std::size_t WorldState::poll_blast(const PolygonHit& hit, std::int16_t source) {
    static constexpr std::array<int, 2> kKinds{0x102, 0x103};
    const std::size_t fired = press_polygon(hit, source, kKinds, stats_.blast_presses, true);
    stats_.switch_interactions += fired;
    stats_.trigger_presses += fired;
    return fired;
}

std::size_t WorldState::post_door_events(EventQueue& queue) const {
    if (door_event_cursor_ > events_.size()) {
        door_event_cursor_ = events_.size();
    }
    std::size_t posted = 0;
    while (door_event_cursor_ < events_.size()) {
        const WorldEvent& event = events_[door_event_cursor_];
        const bool door_event = event.kind == WorldEvent::Kind::DoorOpened ||
                                event.kind == WorldEvent::Kind::DoorClosed ||
                                event.kind == WorldEvent::Kind::DoorCrush;
        if (!door_event || event.target == 0) {
            // Not a door record, or the engine's `param_1 != 0` gate would
            // drop it anyway: advance instead of stalling the cursor.
            ++door_event_cursor_;
            continue;
        }
        const Door* door = find_door(event.door_id);
        if (door == nullptr) {
            ++door_event_cursor_;
            continue;
        }
        const DoorTransform transform = door_transform(*door);
        const bool ok = queue.post_generate_short(
            event.target, static_cast<std::uint16_t>(door->sector),
            static_cast<std::int16_t>(door->object), 0, transform.origin[0],
            transform.origin[1], transform.origin[2], 0xffffffffu);
        if (!ok) {
            door_post_blocked_ = true;
            break; // full queue: keep the cursor on the pending record
        }
        ++posted;
        ++door_event_cursor_;
    }
    if (door_event_cursor_ >= events_.size()) {
        door_post_blocked_ = false;
    }
    return posted;
}

std::size_t WorldState::trigger_poly(const PolygonHit& hit, std::int16_t source) {
    // 0x44fad0 compares (op4, op5, op6) with the caller's three ints and
    // requires kind == 0x102; the distance field is not consulted.
    std::size_t fired = 0;
    for (Switch& sw : switches_) {
        if (sw.kind != 0x102) {
            continue;
        }
        if (sw.pos[0] != static_cast<int>(hit.sector) ||
            sw.pos[1] != static_cast<int>(hit.object) ||
            sw.pos[2] != static_cast<int>(hit.polygon)) {
            continue;
        }
        sw.pressed = true;
        sw.inside = true;
        ++fired;
        log_event({WorldEvent::Kind::SwitchPressed, sw.id, sw.target, 0, 0});
        switch_event_sources_.back() = source;
        press_target(sw.id, sw.target);
    }
    stats_.switch_interactions += fired;
    stats_.trigger_presses += fired;
    return fired;
}

std::size_t WorldState::trigger_at(const std::array<double, 3>& pos, std::int16_t source) {
    std::size_t fired = 0;
    for (Switch& sw : switches_) {
        if (sw.kind != 0x104) {
            continue;
        }
        if (static_cast<int>(pos[0]) != sw.pos[0] || static_cast<int>(pos[1]) != sw.pos[1] ||
            static_cast<int>(pos[2]) != sw.pos[2]) {
            continue;
        }
        sw.pressed = true;
        ++stats_.switch_interactions;
        ++stats_.trigger_presses;
        log_event({WorldEvent::Kind::SwitchPressed, sw.id, sw.target, 0, 0});
        switch_event_sources_.back() = source;
        press_target(sw.id, sw.target);
        ++fired;
    }
    return fired;
}

std::size_t WorldState::drain_sounds(std::vector<SoundRequest>& out, std::size_t limit) {
    if (sound_cursor_ > sound_requests_.size()) {
        sound_cursor_ = sound_requests_.size();
    }
    const std::size_t pending = sound_requests_.size() - sound_cursor_;
    const std::size_t take = std::min(pending, limit);
    out.reserve(out.size() + take);
    for (std::size_t k = 0; k < take; ++k) {
        out.push_back(sound_requests_[sound_cursor_ + k]);
    }
    sound_cursor_ += take;
    if (sound_cursor_ >= sound_requests_.size()) {
        // Everything was handed out: the log is frame-local again.
        sound_requests_.clear();
        sound_cursor_ = 0;
    }
    return take;
}

std::span<const SoundRequest> WorldState::pending_sounds() const {
    const std::size_t begin = std::min(sound_cursor_, sound_requests_.size());
    return std::span<const SoundRequest>(sound_requests_).subspan(begin);
}

void WorldState::clear_sounds() {
    sound_requests_.clear();
    sound_cursor_ = 0;
}

bool WorldState::block_contact(int sector, int object) {
    for (Door& door : doors_) {
        if (door.sector != sector || door.object != object || !door.active()) {
            continue;
        }
        if (door.kind == 1) {
            return false; // ordinary door: no reaction, 0x450590 returns early
        }
        if (door.kind != 2 && door.kind != 3) {
            return false;
        }
        // Both branches drop the phase the tick just computed and go back to
        // opening; the golden `phase_prev` copy is at 0x4506b9/0x4506cf.
        door.phase = door.phase_prev;
        door.phase_next = door.phase;
        door.open_fraction = static_cast<double>(door.phase) * kDoorPhaseToFraction;
        door.timer = 0;
        door.state = DoorState::Opening;
        ++stats_.blocked_hits;
        if (door.kind == 2) {
            log_event({WorldEvent::Kind::DoorBlocked, -1, 0, door.id, 0});
        } else {
            // event_post(-22, sector, object, 0, lerp(closed, open), source)
            // 0x450590 posts the fixed id -22 (0xffffffea), not ids[4]: the
            // crush damage handler is an engine event, not a level one.
            log_event({WorldEvent::Kind::DoorCrush, -1, kDoorCrushEvent, door.id, 0});
            ++stats_.crush_hits;
        }
        return true;
    }
    return false;
}

void WorldState::advance_door(std::size_t index, Door& door, std::uint64_t ticks) {
    // 0x44f2e0 copies +0x8c into +0x90 before moving; 0x44f3f0 commits it back
    // (and 0x450590 can throw the whole tick away).  phase_prev is that copy.
    door.phase_prev = door.engine_phase();
    std::uint64_t left = ticks;
    std::uint32_t step = static_cast<std::uint32_t>(
        std::max<std::int32_t>(door.step, 0));
    const std::uint32_t max_step = 1u << 20; // defensive cap, still > one phase
    if (step > max_step) {
        step = max_step;
    }
    if (!door.active()) {
        // The engine only animates kinds 1..3 (0x44f2e0 low-nibble test).
        left = 0;
    }
    int guard = 0;
    while (left > 0 && guard++ < 4) {
        if (door.state == DoorState::Opening || door.state == DoorState::Closing) {
            if (step == 0) {
                break;
            }
            const bool opening = door.state == DoorState::Opening;
            const std::uint32_t target = opening ? kDoorPhaseFull : 0;
            const std::uint64_t need = ticks_to_phase(door.phase, target, step);
            if (left < need) {
                const std::uint64_t delta = left * step;
                door.phase = opening
                                 ? clamp_phase(static_cast<std::int64_t>(door.phase) +
                                               static_cast<std::int64_t>(delta))
                                 : clamp_phase(static_cast<std::int64_t>(door.phase) -
                                               static_cast<std::int64_t>(delta));
                left = 0;
            } else {
                door.phase = target;
                left -= need;
                door.state = DoorState::Closed;
                // 0x44f3f0 posts ids[4] when fully closed, ids[3] when fully
                // open, and plays the stop sound (v2/n2) in both cases.
                const int event_id = opening ? door.event_open : door.event_close;
                log_event({opening ? WorldEvent::Kind::DoorOpened
                                   : WorldEvent::Kind::DoorClosed,
                           -1, event_id, door.id, 0, door.stop_sound.sample,
                           door.stop_sound.volume});
                // 0x44f3f0 plays the stop slot (v2/n2) when the leaf reaches
                // either end and asks the mixer to stop the looped movement
                // sound of this door (0x46ba90, class `1000000 + index`).
                queue_door_sound(index, door, SoundSource::DoorStop, door.stop_sound);
                queue_door_move_stop(index, door);
                if (opening && door.hold_ticks > 0) {
                    door.timer = door.hold_ticks;
                }
            }
        } else if (door.timer > 0) {
            if (left < door.timer) {
                door.timer -= static_cast<std::uint32_t>(left);
                left = 0;
            } else {
                left -= door.timer;
                door.timer = 0;
                door.state = DoorState::Closing;
                // 0x44f3f0 plays v0/n0 when the dwell expires.
                log_event({WorldEvent::Kind::DoorClosing, -1, door.event_close, door.id, 0,
                           door.start_sound.sample, door.start_sound.volume});
                // The dwell expiry starts a new movement: the start slot
                // sounds again (0x44f66b) and the looped movement slot
                // follows with the next tick.
                queue_door_sound(index, door, SoundSource::DoorStart, door.start_sound);
                queue_door_sound(index, door, SoundSource::DoorMove, door.move_sound);
            }
        } else {
            break;
        }
    }
    door.phase_next = door.engine_phase();
    door.open_fraction = static_cast<double>(door.phase) * kDoorPhaseToFraction;
}

void WorldState::advance_lift(Lift& lift, std::uint64_t ticks) {
    if (!lift.moving || lift.speed <= 0 || lift.stops.empty()) {
        return;
    }
    std::uint64_t left = ticks;
    int guard = 0;
    while (left > 0 && lift.moving && guard++ < 2) {
        const std::int64_t distance =
            static_cast<std::int64_t>(lift.target) - lift.height;
        const std::int64_t speed = lift.speed;
        const std::uint64_t need = distance == 0
                                       ? 0
                                       : static_cast<std::uint64_t>(
                                             (std::llabs(distance) + speed - 1) / speed);
        if (left < need) {
            const std::int64_t delta = static_cast<std::int64_t>(left) * speed;
            lift.height = static_cast<int>(
                static_cast<std::int64_t>(lift.height) +
                (distance > 0 ? delta : -delta));
            left = 0;
        } else {
            lift.height = lift.target;
            left -= need;
            lift.moving = false;
            lift.direction = 0;
            // 0x4502a0: arrival sets +0x68 = 0, posts the stop's sound column
            // and reloads the dwell into +0x64.  The cabin transform is
            // refreshed by 0x44e6f0 in the same call.
            lift.height_tick = lift.height;
            lift.wait = lift.dwell;
            if (static_cast<std::size_t>(lift.stop_index) < lift.stops.size() &&
                lift.stops[static_cast<std::size_t>(lift.stop_index)].height ==
                    lift.height) {
                // already at the requested stop
            }
        }
        lift.height_tick = lift.height;
    }
}

void WorldState::tick(std::uint64_t ticks) {
    // A full queue can leave the tail unposted.  Keep that pending tail across
    // the tick instead of silently dropping the switch press; fully consumed
    // logs are cleared as before.
    if (switch_event_cursor_ > events_.size()) {
        switch_event_cursor_ = events_.size();
    }
    if (switch_event_sources_.size() != events_.size()) {
        // Defensive: keep the per-record source log index-aligned with the
        // event log even if a caller emptied one of them by hand.
        switch_event_sources_.resize(events_.size());
    }
    // A full queue can leave a tail unposted for either poster.  Keeping the
    // whole frame log (instead of erasing a prefix) keeps both cursors valid;
    // they are only meaningful while `events_` is alive, so they are reset
    // together with the array.  The log grows only while the queue stays full,
    // by the handful of events one tick produces.
    const bool blocked = switch_post_blocked_ || door_post_blocked_;
    if (!blocked) {
        events_.clear();
        switch_event_sources_.clear();
        switch_event_cursor_ = 0;
        door_event_cursor_ = 0;
    }
    switch_post_blocked_ = blocked;
    door_post_blocked_ = blocked;
    // A sound request that was not drained stays pending: the tail survives
    // the tick, exactly like the unposted door records.  A fully drained log
    // is dropped so the queue does not grow.
    if (sound_cursor_ >= sound_requests_.size()) {
        sound_requests_.clear();
        sound_cursor_ = 0;
    }
    if (ticks == 0) {
        return;
    }
    stats_.ticks += ticks;
    for (std::size_t k = 0; k < doors_.size(); ++k) {
        advance_door(k, doors_[k], ticks);
    }
    for (Lift& lift : lifts_) {
        advance_lift(lift, ticks);
    }
    for (Switch& sw : switches_) {
        sw.pressed = false;
    }
}

std::vector<ObjectDoorState> WorldState::apply_to_objects() const {
    std::vector<ObjectDoorState> out;
    out.reserve(doors_.size());
    for (const Door& door : doors_) {
        ObjectDoorState state;
        state.door_id = door.id;
        state.sector = door.sector;
        state.object = door.object;
        state.state = door.state;
        state.open_fraction = door.open_fraction;
        state.sliding = door.sliding;
        state.linked = door.linked;
        state.bound = door.bound;
        // The engine writes this transform into the .sbw object every tick
        // (0x44dfc0 -> 0x434887) and all consumers — render, collision, the
        // lighting rays — read the mutated object, so the doorway geometry
        // moves with the leaf.
        state.transform = door_transform(door);
        out.push_back(state);
    }
    return out;
}

Door* WorldState::find_door(int id) {
    for (Door& door : doors_) {
        if (door.id == id) {
            return &door;
        }
    }
    return nullptr;
}

const Door* WorldState::find_door(int id) const {
    for (const Door& door : doors_) {
        if (door.id == id) {
            return &door;
        }
    }
    return nullptr;
}

Lift* WorldState::find_lift(int id) {
    for (Lift& lift : lifts_) {
        if (lift.id == id) {
            return &lift;
        }
    }
    return nullptr;
}

const Lift* WorldState::find_lift(int id) const {
    for (const Lift& lift : lifts_) {
        if (lift.id == id) {
            return &lift;
        }
    }
    return nullptr;
}

void WorldState::set_locked(int door_id, bool locked) {
    for (Door& door : doors_) {
        if (door.id == door_id) {
            door.locked = locked;
        }
    }
}

void WorldState::set_hold_ticks(int door_id, std::uint32_t ticks) {
    for (Door& door : doors_) {
        if (door.id == door_id) {
            door.hold_ticks = ticks;
            if (door.state == DoorState::Closed && door.timer > 0) {
                door.timer = ticks;
            }
        }
    }
}

void WorldState::set_step(int door_id, std::int32_t phase_per_tick) {
    for (Door& door : doors_) {
        if (door.id == door_id) {
            door.step = std::max<std::int32_t>(phase_per_tick, 0);
        }
    }
}

} // namespace gm
