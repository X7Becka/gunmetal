// One player over `GameRuntime`: the driving-plan parser and the fixed-tick
// driver described in `game/sim_player.h` / `docs/SIM_PLAYER.md`.
//
// No new physics: the tick calls `gm::step` (`player/controller.cpp`) and
// `GameRuntime::update_player_weapon` (`game/runtime.cpp`, the `0x4453d0`
// body of `game/trigger.cpp`).  The only arithmetic of this file is the yaw
// integration and the aim direction; both are plain `double` and in a fixed
// order, so the module stays bit-deterministic.
#include "game/sim_player.h"

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <span>
#include <string>

namespace gm {
namespace {

bool fail(std::string* error, const std::string& text) {
    if (error != nullptr) {
        *error = text;
    }
    return false;
}

std::string_view trimmed(std::string_view text) {
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

/// Strict `double`: the whole field must be a finite number in `C` locale
/// (`std::strtod` never accepts a trailing space, so the field is trimmed
/// first).  `nan`, `inf`, `1e999` and `1abc` are rejected.
bool parse_value(std::string_view text, double* out) {
    text = trimmed(text);
    if (text.empty()) {
        return false;
    }
    const std::string buffer(text);
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(buffer.c_str(), &end);
    if (end != buffer.c_str() + buffer.size() || errno == ERANGE ||
        !std::isfinite(value)) {
        return false;
    }
    *out = value;
    return true;
}

/// Strict positive integer tick count in 1..kDriveMaxTicks.  Digits only: no
/// sign, no decimal point, no exponent, and no integer overflow on long input.
bool parse_ticks(std::string_view text, int* out) {
    text = trimmed(text);
    if (text.empty()) {
        return false;
    }
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
        if (value > kDriveMaxTicks) {
            return false; // out of range; also keeps the accumulator in `int`
        }
    }
    if (value < 1) {
        return false;
    }
    *out = value;
    return true;
}

/// Parse one `;`-separated field into `segment`.  `index` is the 1-based
/// segment number, only used in the error text.
///
/// Three accepted spellings (see the header):
///   `key=value:ticks` (numeric and boolean keys),
///   `key:ticks` (boolean keys; value = true),
///   `key=ticks` (boolean keys; the compact `jump=5` form of the docs).
bool parse_segment(std::string_view raw, int index, DriveSegment* segment,
                   std::string* error) {
    const std::string prefix = "drive segment " + std::to_string(index) + ": ";
    const std::string_view text = trimmed(raw);
    if (text.empty()) {
        return fail(error, prefix + "empty segment");
    }
    const std::size_t colon = text.find(':');
    std::string_view left;
    std::string_view ticks_text;
    bool ticks_after_equals = false;
    if (colon == std::string_view::npos) {
        // No ':' at all: only `key=ticks` (a boolean key) can be meant.
        const std::size_t equals = text.find('=');
        if (equals == std::string_view::npos || equals == 0) {
            return fail(error, prefix + "expected <key>[=<value>]:<ticks>, got \"" +
                                     std::string(text) + "\"");
        }
        left = trimmed(text.substr(0, equals));
        ticks_text = text.substr(equals + 1);
        ticks_after_equals = true;
    } else {
        if (text.find(':', colon + 1) != std::string_view::npos) {
            return fail(error, prefix + "more than one ':' in \"" + std::string(text) + "\"");
        }
        left = trimmed(text.substr(0, colon));
        ticks_text = text.substr(colon + 1);
    }
    if (left.empty()) {
        return fail(error, prefix + "missing key before ':'");
    }
    const std::size_t equals = left.find('=');
    const std::string_view key = trimmed(left.substr(0, equals));
    const bool has_value = equals != std::string_view::npos;
    const std::string_view value_text =
        has_value ? trimmed(left.substr(equals + 1)) : std::string_view();
    if (key.empty()) {
        return fail(error, prefix + "missing key before '='");
    }

    int ticks = 0;
    if (!parse_ticks(ticks_text, &ticks)) {
        return fail(error, prefix + "ticks must be an integer in 1.." +
                                 std::to_string(kDriveMaxTicks) + ", got \"" +
                                 std::string(ticks_text) + "\"");
    }

    // The three continuous keys need a number; the three boolean keys default
    // to true and accept a finite non-zero value (`fire=0:10` is a valid
    // "do not fire" stretch, useful when a plan is generated by a tool).
    double value = 0.0;
    const bool boolean_key = key == "fire" || key == "jump" || key == "crouch";
    const bool numeric_key = key == "forward" || key == "strafe" || key == "turn";
    if (!boolean_key && !numeric_key) {
        return fail(error, prefix + "unknown key \"" + std::string(key) +
                                 "\" (expected forward, strafe, turn, fire, jump, "
                                 "crouch)");
    }
    if (boolean_key) {
        value = 1.0;
        if (!ticks_after_equals && has_value && !parse_value(value_text, &value)) {
            return fail(error, prefix + "bad boolean for \"" + std::string(key) +
                                     "\": \"" + std::string(value_text) + "\"");
        }
    } else if (!has_value) {
        return fail(error, prefix + "key \"" + std::string(key) +
                                 "\" needs a value, e.g. " + std::string(key) + "=1:100");
    } else if (!parse_value(value_text, &value)) {
        return fail(error, prefix + "bad number for \"" + std::string(key) +
                                 "\": \"" + std::string(value_text) + "\"");
    }

    DriveSegment out;
    out.ticks = ticks;
    if (boolean_key) {
        const bool on = value != 0.0;
        if (key == "fire") {
            out.fire = on;
        } else if (key == "jump") {
            out.jump = on;
        } else {
            out.crouch = on;
        }
    } else if (key == "forward") {
        out.forward = value;
    } else if (key == "strafe") {
        out.strafe = value;
    } else {
        out.turn = value;
    }
    *segment = out;
    return true;
}

} // namespace

std::optional<std::vector<DriveSegment>> parse_drive(std::string_view script,
                                                     std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    std::vector<DriveSegment> segments;
    std::size_t begin = 0;
    int index = 0;
    while (true) {
        ++index;
        const std::size_t end = script.find(';', begin);
        const std::string_view raw =
            (end == std::string_view::npos) ? script.substr(begin)
                                            : script.substr(begin, end - begin);
        DriveSegment segment;
        if (!parse_segment(raw, index, &segment, error)) {
            return std::nullopt;
        }
        segments.push_back(segment);
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return segments;
}

bool SimPlayer::init(const Vec3d& feet, double yaw, const ChassisSpec& chassis,
                     std::string* error) {
    // A failed init leaves the object uninitialised, never half-configured.
    initialized_ = false;
    runtime_ = nullptr;
    ticks_done_ = 0;
    shots_fired_ = 0;
    last_weapon_ = PlayerWeaponTick{};
    last_step_ = StepResult{};

    if (!finite(feet)) {
        return fail(error, "init: feet must be finite");
    }
    if (!std::isfinite(yaw)) {
        return fail(error, "init: yaw must be finite");
    }
    if (!std::isfinite(chassis.eye_height) || !(chassis.eye_height > 0.0)) {
        return fail(error, "init: chassis eye height must be finite and > 0");
    }
    if (!std::isfinite(chassis.max_speed) || chassis.max_speed < 0.0) {
        return fail(error, "init: chassis max speed must be finite and >= 0");
    }

    // The defaults of `ControllerConfig` are the LIGHT capsule the controller
    // documents; only the three chassis-driven fields are replaced here.
    ControllerConfig config;
    config.eye_height = chassis.eye_height;
    config.max_speed = chassis.max_speed * kPlayerTickHz;
    config.retention = chassis_motion(chassis).retention;

    config_ = config;
    state_ = make_state(feet, yaw, config_);
    initialized_ = true;
    return true;
}

bool SimPlayer::tick(GameRuntime& runtime, const DriveSegment& segment,
                     int tick_index, std::string* error) {
    if (!initialized_) {
        return fail(error, "tick: SimPlayer::init() has not been called");
    }
    if (tick_index < 0) {
        return fail(error, "tick: tick_index must be >= 0");
    }
    if (!std::isfinite(segment.forward) || !std::isfinite(segment.strafe) ||
        !std::isfinite(segment.turn)) {
        return fail(error, "tick: segment forward/strafe/turn must be finite");
    }

    // Yaw first, so the same tick moves along the new heading.  `turn` is
    // radians per second; the tick is exactly `config.dt` seconds.
    const double next_yaw = state_.yaw + segment.turn * config_.dt;
    if (!std::isfinite(next_yaw)) {
        return fail(error, "tick: yaw became non-finite");
    }

    Input input;
    input.forward = segment.forward;
    input.strafe = segment.strafe;
    input.jump = segment.jump;
    input.crouch = segment.crouch;

    PlayerState from = state_;
    from.yaw = next_yaw;
    const StepResult result = step(from, input, runtime.collision(), config_);
    last_step_ = result;
    if (!result.ok) {
        // The controller leaves the state and the world untouched; so do we.
        return fail(error, "player step: " + result.error);
    }

    PlayerState next = result.state;
    next.yaw = next_yaw; // the controller never rewrites yaw; keep it exact
    const Vec3d eye = eye_position(next);
    const std::span<const Vec3d> eyes(&eye, 1);
    if (!runtime.set_player_eyes(eyes)) {
        return fail(error, "player eye point rejected by the runtime");
    }

    // Commit only after every check passed, so `state()` stays unchanged by a
    // failed tick.
    state_ = next;
    runtime_ = &runtime;
    ++ticks_done_;

    if (segment.fire) {
        PlayerWeaponInput weapon;
        weapon.trigger_held = true;
        // The first tick of the series is the edge (`+0x3344 & 0x20`); the
        // rest only hold it, which is what makes fire mode 0 fire once and
        // mode 1 use the weapon period.
        weapon.trigger_edge = tick_index == 0;
        weapon.slot = -1;
        weapon.aim_direction = {std::cos(state_.yaw), std::sin(state_.yaw), 0.0};
        last_weapon_ = runtime.update_player_weapon(weapon, eye);
        if (last_weapon_.action.fired()) {
            ++shots_fired_;
        }
    }
    return true;
}

std::uint32_t SimPlayer::ammo(int slot) const {
    if (runtime_ == nullptr || slot < 0 || slot >= kWeaponSlots) {
        return 0;
    }
    return runtime_->player_arsenal().ammo[static_cast<std::size_t>(slot)];
}

} // namespace gm
