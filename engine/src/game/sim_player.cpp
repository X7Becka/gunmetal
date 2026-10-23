// One player over `GameRuntime`: the driving-plan parser and the fixed-tick
// driver described in `game/sim_player.h` / `docs/SIM_PLAYER.md`.
//
// No new physics: the tick calls `gm::step` (`player/controller.cpp`) and
// `GameRuntime::update_player_weapon` (`game/runtime.cpp`, the `0x4453d0`
// body of `game/trigger.cpp`).  The only arithmetic of this file is the yaw
// integration and the aim direction; both are plain `double` and in a fixed
// order, so the module stays bit-deterministic.
//
// Slice 2 (`docs/SIM_PROJECTILE.md`) adds projectile mode: with a template
// installed the same tick runs one `GameRuntime::tick(1)` and feeds the last
// projectile generation to `GameRuntime::apply_projectile_blasts`, storing the
// caller-owned thing vitals back.  The round itself is stepped by the runtime;
// nothing here re-implements `0x4752d0`.
//
// Slice 3 (`docs/SIM_DAMAGE.md`) gives the player real gauges: `set_vitals()`
// stores the starting `Vitals`, the tick installs them on the runtime
// (`set_player_vitals`) and blasts through
// `apply_projectile_blasts_to_players`, reads `player_vitals()` back and
// accumulates `damage_taken()`.  Death is `armor <= 0` -- the original rule of
// `0x453b50`/`0x4536e0` -- and `respawn()` restores the starting pose/gauges.
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

/// One line of `set_vitals()`: the save invariants of
/// `docs/PLAYER_FIELDS.md` §11 (`0 < armor_max <= 10^6`, `0 <= armor <=
/// armor_max`, `0 <= shields[i] <= shield_max`).  A gauge outside them would
/// make `apply_damage_gauge()` meaningless (and `mult_*` non-finite would
/// poison every later hit), so it is rejected instead of clamped.
bool valid_vitals(const Vitals& vitals, std::string* error) {
    if (vitals.armor_max <= 0) {
        return fail(error, "set_vitals: armor_max must be > 0");
    }
    if (vitals.armor < 0 || vitals.armor > vitals.armor_max) {
        return fail(error, "set_vitals: armor must be in 0..armor_max");
    }
    if (vitals.shield_max < 0) {
        return fail(error, "set_vitals: shield_max must be >= 0");
    }
    for (std::size_t i = 0; i < vitals.shields.size(); ++i) {
        if (vitals.shields[i] < 0 || vitals.shields[i] > vitals.shield_max) {
            return fail(error, "set_vitals: shields[" + std::to_string(i) +
                                   "] must be in 0..shield_max");
        }
    }
    if (!std::isfinite(vitals.mult_plate) || vitals.mult_plate < 0.0) {
        return fail(error, "set_vitals: mult_plate must be finite and >= 0");
    }
    if (!std::isfinite(vitals.mult_hull) || vitals.mult_hull < 0.0) {
        return fail(error, "set_vitals: mult_hull must be finite and >= 0");
    }
    return true;
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
    vitals_runtime_ = nullptr;
    vitals_dirty_ = true;

    // Projectile mode survives re-init (the template is configuration), but
    // its per-life counters belong to the previous player: start clean.
    projectile_runtime_ = nullptr;
    projectiles_spawned_ = 0;
    blast_hits_ = 0;
    things_damaged_ = 0;
    damaged_things_.clear();

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
    start_feet_ = feet;
    start_yaw_ = yaw;

    // `start_vitals_` is configuration and survives re-init (set_vitals() may
    // be called before init()); every new life starts from it.
    vitals_ = start_vitals_;
    alive_ = vitals_.armor > 0;
    death_tick_ = -1;
    deaths_ = 0;
    damage_taken_ = 0;
    reset_regen(regen_state_);
    initialized_ = true;
    return true;
}

bool SimPlayer::set_regen(const RegenRule& rule, std::string* error) {
    if (const char* why = regen_rule_error(rule); why != nullptr) {
        return fail(error, why);
    }
    regen_rule_ = rule;
    reset_regen(regen_state_);
    return true;
}

void SimPlayer::clear_regen() {
    regen_rule_.reset();
    reset_regen(regen_state_);
}

bool SimPlayer::set_vitals(const Vitals& vitals, std::string* error) {
    if (!valid_vitals(vitals, error)) {
        return false;
    }
    start_vitals_ = vitals;
    vitals_ = vitals;
    alive_ = vitals.armor > 0;
    vitals_dirty_ = true; // installed on the runtime by the next tick()
    reset_regen(regen_state_);
    return true;
}

bool SimPlayer::respawn(std::string* error) {
    if (!initialized_) {
        return fail(error, "respawn: SimPlayer::init() has not been called");
    }
    state_ = make_state(start_feet_, start_yaw_, config_);
    vitals_ = start_vitals_;
    alive_ = vitals_.armor > 0;
    vitals_dirty_ = true;
    reset_regen(regen_state_);
    ++deaths_;
    last_step_ = StepResult{};
    last_weapon_ = PlayerWeaponTick{};
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
    if (!alive_) {
        // Dead player: a successful no-op.  Nothing moves, nothing fires and
        // the runtime is not ticked, so the state, the tick counter and the
        // gauges stop changing until `respawn()` (docs/SIM_DAMAGE.md §4).
        return true;
    }

    // Projectile mode (slice 2): the runtime must know the template before the
    // weapon seam can spawn a round, so install it first.  Installing is the
    // only thing the template affects until `segment.fire` spawns a round.
    install_projectile(runtime);

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
    // The gauges live on the runtime (`set_player_vitals`): one per eye point,
    // so they can only be installed after `set_player_eyes()` and must be
    // re-installed per runtime / after set_vitals()/respawn().
    if (!install_vitals(runtime, error)) {
        return false;
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
        if (last_weapon_.spawned >= 0) {
            // The round itself is stepped below; only a real spawn counts.
            ++projectiles_spawned_;
        }
    }

    // Projectile mode: one runtime tick, so the round just spawned (and any
    // round of an earlier shot) is stepped by `Projectiles::tick_results` and
    // its impact/expiry is blasted in this same tick.  The slice-1 tick does
    // not do this; without `set_projectile()` nothing below runs.
    bool damaged_this_tick = false;
    if (has_projectile()) {
        runtime.tick(1);
        const Vitals before = vitals_;
        apply_blasts(runtime);
        // The runtime owns the gauges it just wrote (`docs/RUNTIME_CPP.md` §7);
        // copy them back so `vitals()`/the next tick see the damage.
        const std::span<const Vitals> owned = runtime.player_vitals();
        if (!owned.empty()) {
            vitals_ = owned[0];
        } else {
            vitals_dirty_ = true; // eyes/vitals list was dropped behind our back
        }
        const auto lost = [](std::int32_t was, std::int32_t now) {
            return was > now ? was - now : 0;
        };
        damaged_this_tick = lost(before.armor, vitals_.armor) > 0;
        damage_taken_ += lost(before.armor, vitals_.armor);
        for (std::size_t i = 0; i < vitals_.shields.size(); ++i) {
            if (lost(before.shields[i], vitals_.shields[i]) > 0) {
                damaged_this_tick = true;
            }
            damage_taken_ += lost(before.shields[i], vitals_.shields[i]);
        }
        // Death rule of the original `0x453b50` -> `0x4536e0` (docs/DAMAGE.md
        // §2.7/§3): the gauges are written first, then `armor <= 0` kills.  The
        // `+0x3200 & 0x20` bit of the original is the shield-hit flash, not
        // death (docs/PLAYER_FIELDS.md §7, docs/SIM_DAMAGE.md §3).
        if (alive_ && vitals_.armor <= 0) {
            alive_ = false;
            death_tick_ = ticks_done_;
        }
    }

    // Regeneration (slice 4, `M6.1.3`): after the gauges are read back and the
    // damage has been accounted, one pure `apply_regen()` step may restore
    // shields/armor.  It runs after `damage_taken_` was updated, so
    // regeneration can never lower that counter; a death (or a dead player)
    // restores nothing.  The rule is off unless `set_regen()` installed one.
    if (regen_rule_ && alive_) {
        const Vitals before = vitals_;
        apply_regen(vitals_, regen_state_, *regen_rule_, damaged_this_tick);
        if (vitals_.armor != before.armor || vitals_.shields != before.shields) {
            // The runtime still holds the pre-regeneration copy: re-install it
            // on the next tick so the next blast sees the restored gauges.
            vitals_dirty_ = true;
        }
    }
    return true;
}

void SimPlayer::set_projectile(const Thing& projectile, int owner_player) {
    projectile_ = projectile;
    projectile_owner_ = owner_player;
    // The old runtime keeps a copy of the previous template; the next tick
    // re-installs the new one before it spawns anything.
    projectile_runtime_ = nullptr;
}

void SimPlayer::clear_projectile() {
    // The runtime keeps its own copy (`set_player_projectile`), so drop that
    // too: otherwise a later `fire` would still spawn a round while the player
    // is in slice-1 mode.
    if (projectile_runtime_ != nullptr) {
        projectile_runtime_->clear_player_projectile();
    }
    projectile_.reset();
    projectile_owner_ = 0;
    projectile_runtime_ = nullptr;
}

void SimPlayer::install_projectile(GameRuntime& runtime) {
    if (!projectile_ || projectile_runtime_ == &runtime) {
        return;
    }
    // Thin wrapper over the documented runtime seam (`0x46ea40`): no fields
    // are rewritten and no validation is added -- the template is passed
    // through exactly as the caller built it.
    runtime.set_player_projectile(*projectile_, projectile_owner_, 0.0);
    projectile_runtime_ = &runtime;
}

bool SimPlayer::install_vitals(GameRuntime& runtime, std::string* error) {
    if (!vitals_dirty_ && vitals_runtime_ == &runtime) {
        return true;
    }
    const std::span<const Vitals> span(&vitals_, 1);
    if (!runtime.set_player_vitals(span)) {
        // The only documented rejection is a size mismatch with the eye list,
        // and `set_player_eyes({eye})` above always made it one.
        return fail(error, "player vitals rejected by the runtime");
    }
    vitals_runtime_ = &runtime;
    vitals_dirty_ = false;
    return true;
}

void SimPlayer::apply_blasts(GameRuntime& runtime) {
    const std::span<const ProjectileTickRecord> results =
        runtime.last_projectile_results();
    if (results.empty()) {
        return; // no round in the world: nothing to blast, no generation mark
    }
    // Cheap pre-check: only a real contact (`bounced`) or a first expiry can
    // blast in the weapon-keyed bridge (`proximity` needs an explicit opt-in).
    // A spent record is replayed every tick while it stays in `Projectiles`, so
    // without this the whole vitals copy below would run for the rest of the
    // simulation after the first hit.
    bool triggers = false;
    for (const ProjectileTickRecord& record : results) {
        if (record.already_spent) {
            continue;
        }
        if (record.status == ProjectileStatus::bounced ||
            record.status == ProjectileStatus::expired) {
            triggers = true;
            break;
        }
    }
    if (!triggers) {
        return;
    }
    const Weapons* table = runtime.player_weapons();
    if (table == nullptr) {
        return; // no weapon table: the runtime has no blasts to select
    }
    const Weapon* weapon = table->at(runtime.player_arsenal().current);
    if (weapon == nullptr) {
        return;
    }

    // Thing vitals are caller-owned (`docs/RUNTIME_CPP.md` §7): copy them out,
    // let the bridge write damage into the copy, then store it back.  The
    // player gauges are runtime-owned (`install_vitals`), so the bridge takes
    // them from `player_vitals_` -- this is the slice-3 damage path.
    const std::vector<Thing>& things = runtime.things().things();
    std::vector<ThingVitals> vitals;
    vitals.reserve(things.size());
    for (const Thing& thing : things) {
        vitals.push_back(thing_vitals(thing));
    }

    ProjectileBlastContext context;
    context.source_player = projectile_owner_;
    context.source_thing = -1;
    const ProjectileBlastBatch batch =
        runtime.apply_projectile_blasts_to_players(*weapon, context, vitals);
    if (batch.blasts == 0) {
        return;
    }

    blast_hits_ += static_cast<int>(batch.damage.hits.size());
    if (damaged_things_.size() < vitals.size()) {
        damaged_things_.resize(vitals.size(), 0);
    }
    for (const AppliedBlastHit& hit : batch.damage.hits) {
        if (hit.hit.is_player) {
            continue; // counted in blast_hits(); its damage is in player_vitals()
        }
        const auto index = static_cast<std::size_t>(hit.hit.index);
        if (index >= vitals.size() || vitals[index].max_hp <= 0) {
            continue; // immune thing (max_hp == 0): `apply_thing_damage` no-op
        }
        if (damaged_things_[index] == 0) {
            damaged_things_[index] = 1;
            ++things_damaged_;
        }
    }

    // The caller-owned vitals are what actually changes the world thing.
    for (std::size_t index = 0; index < vitals.size(); ++index) {
        Thing* thing = runtime.things().by_id(things[index].id);
        if (thing != nullptr) {
            store_thing_vitals(*thing, vitals[index]);
        }
    }
}

std::uint32_t SimPlayer::ammo(int slot) const {
    if (runtime_ == nullptr || slot < 0 || slot >= kWeaponSlots) {
        return 0;
    }
    return runtime_->player_arsenal().ammo[static_cast<std::size_t>(slot)];
}

} // namespace gm
