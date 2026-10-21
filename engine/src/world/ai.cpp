#include "world/ai.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "world/ai_move.h"

namespace gm {

const char* ai_state_name(AiState state) {
    switch (state) {
    case AiState::idle:
        return "idle";
    case AiState::patrol:
        return "patrol";
    case AiState::alert:
        return "alert";
    case AiState::attack:
        return "attack";
    case AiState::dead:
        return "dead";
    }
    return "idle";
}

const char* ai_event_name(AiEventType type) {
    switch (type) {
    case AiEventType::attach:
        return "attach";
    case AiEventType::sight:
        return "sight";
    case AiEventType::lost_sight:
        return "lost_sight";
    case AiEventType::state:
        return "state";
    case AiEventType::attack:
        return "attack";
    case AiEventType::weapon:
        return "weapon";
    case AiEventType::damage:
        return "damage";
    case AiEventType::death:
        return "death";
    case AiEventType::leash:
        return "leash";
    case AiEventType::arrived:
        return "arrived";
    }
    return "state";
}

int ai_history_lag(int raw_offset) {
    if (raw_offset <= 0) {
        return 0; // no lag: the live target position is used
    }
    return std::min(raw_offset, kAiHistoryMaxLag);
}

bool ai_path_mode(int movement_type, AiPathMode& out) {
    switch (movement_type) {
    case 9:
        out = AiPathMode::path;
        return true;
    case 10:
        out = AiPathMode::loop;
        return true;
    case 12:
        out = AiPathMode::expire;
        return true;
    default:
        // `pathrandom` (11) needs `rand()` (0x480e30); every other movement
        // type never enters the 0x475f10 automaton.
        return false;
    }
}

int ai_weapon_count(const Thing& thing) {
    return std::min(kAiMaxWeapons, static_cast<int>(thing.proximity.size()));
}

bool ai_weapon_at(const Thing& thing, int index, AiWeapon& out) {
    if (index < 0 || index >= ai_weapon_count(thing)) {
        return false;
    }
    const ProximityEntry& entry = thing.proximity[static_cast<std::size_t>(index)];
    if (entry.event == 0 || entry.high < entry.low || entry.cooldown < 0 ||
        !finite(entry.offset)) {
        return false;
    }
    AiWeapon weapon;
    weapon.count = ai_weapon_count(thing);
    weapon.event = entry.event;
    weapon.low = entry.low;
    weapon.high = entry.high;
    weapon.cooldown = entry.cooldown;
    weapon.offset = entry.offset;
    out = weapon;
    return true;
}

bool ai_weapon_select(const Thing& thing, double distance, bool reverse,
                      AiWeapon& out) {
    if (!std::isfinite(distance)) {
        return false;
    }
    const int count = ai_weapon_count(thing);
    for (int step = 0; step < count; ++step) {
        const int index = reverse ? count - 1 - step : step;
        AiWeapon weapon;
        if (!ai_weapon_at(thing, index, weapon)) {
            continue;
        }
        if (distance < static_cast<double>(weapon.low) ||
            distance > static_cast<double>(weapon.high)) {
            continue;
        }
        out = weapon;
        return true;
    }
    return false;
}

void TargetHistory::reset() {
    slots.fill(Vec3d{});
    next = 0;
    count = 0;
}

void TargetHistory::record(const Vec3d& position) {
    slots[static_cast<std::size_t>(next)] = position;
    next = (next + 1) % kAiHistorySlots;
    if (count < kAiHistorySlots) {
        ++count;
    }
}

bool TargetHistory::at(int lag, Vec3d& out) const {
    const int clamped = std::clamp(lag, 0, kAiHistoryMaxLag);
    if (clamped >= count) {
        return false;
    }
    int index = next - 1 - clamped;
    while (index < 0) {
        index += kAiHistorySlots;
    }
    out = slots[static_cast<std::size_t>(index)];
    return true;
}

std::size_t Ai::attach(const Things& things) {
    agents_.clear();
    events_.clear();
    weapon_shots_.clear();
    reset_target_history();
    stats_ = AiStats{};
    things_ = &things;

    for (const Thing& thing : things.things()) {
        if (!thing.absolute()) {
            ++stats_.skipped_without_position;
            continue;
        }
        const auto& p = *thing.position;
        AiAgent agent;
        agent.thing_id = thing.id;
        agent.home = Vec3d{static_cast<double>(p[0]), static_cast<double>(p[1]),
                           static_cast<double>(p[2])};
        agent.position = agent.home;
        agent.leash = config_.leash;
        agent.has_home = true;
        agent.thing_type = thing.type;
        // A5: the weapon table is data only until `use_weapon_table` is set.
        agent.weapon.count = ai_weapon_count(thing);
        // A2 movement defaults; `use_thing_fields` overwrites them from the
        // THING_TYPES copy below.  Zero type values keep the fallbacks so a
        // synthetic record (all zeros) still moves.
        agent.leash_anchor = agent.home;
        agent.speed = config_.move_speed;
        agent.radius = config_.move_radius;
        agent.arrive_radius = config_.move_arrive_radius;
        agent.standoff = config_.move_standoff;
        agent.leash_distance = config_.move_leash;
        if (config_.use_thing_fields && thing.type >= 0 && !thing.type_name.empty()) {
            agent.movement_type =
                (thing.movement_type >= 0 && thing.movement_type <= 18)
                    ? thing.movement_type
                    : 0;
            agent.search_radius = thing.search_radius;
            agent.target_zone = thing.target_zone_threshold;
            agent.history_lag = ai_history_lag(thing.target_history_offset);
            agent.expire_ticks = thing.expire_ticks;
            agent.collide_event = thing.collide_event;
            agent.expire_event = thing.expire_event;
            agent.max_health = static_cast<double>(std::max(0, thing.max_health));
            agent.immune = thing.max_health == 0;
            agent.health = thing.alive ? (agent.immune ? 0.0 : agent.max_health) : 0.0;
            // A2: proven THING_TYPES movement fields (+0x10/+0x70/+0xd0/+0xf8)
            // and the target-zone threshold (+0x1cc) as the stop distance.
            if (thing.speed > 0.0) {
                agent.speed = thing.speed;
            }
            if (thing.radius > 0.0) {
                agent.radius = thing.radius;
            }
            if (thing.arrive_radius > 0.0) {
                agent.arrive_radius = thing.arrive_radius;
            }
            if (thing.target_zone_threshold > 0) {
                agent.standoff = static_cast<double>(thing.target_zone_threshold);
            }
            if (thing.leash_distance > 0.0) {
                agent.leash_distance = thing.leash_distance;
            }
            ++stats_.typed_agents;
        } else {
            agent.search_radius = config_.sight_range;
            agent.target_zone = static_cast<int>(config_.attack_range);
            agent.max_health = config_.health;
            agent.health = thing.alive ? config_.health : 0.0;
        }
        agent.state = thing.alive ? AiState::idle : AiState::dead;
        agent.timer = thing.alive ? config_.patrol_after_seconds : 0.0;
        agent.path_mode = config_.path_mode;
        agents_.push_back(agent);
        log(AiEventType::attach, agents_.back(), 0.0, 0);
        // A6 (opt-in): seed the route from the type's location-id list.  Only
        // movement types 9/10/12 reach the 0x475f10 automaton at all, so a
        // dormant +0x324[] on any other type is left alone.
        if (config_.load_path_from_types) {
            AiPathMode mode;
            if (ai_path_mode(thing.movement_type, mode)) {
                set_path_from_thing(thing.id);
            }
        }
    }
    stats_.attached = agents_.size();
    refresh_stats();
    return agents_.size();
}

bool Ai::set_players(std::span<const Vec3d> eyes) {
    if (eyes.empty() || eyes.size() > static_cast<std::size_t>(kAiMaxPlayers)) {
        return false;
    }
    for (const Vec3d& eye : eyes) {
        if (!finite(eye)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < eyes.size(); ++i) {
        player_eyes_[i] = eyes[i];
    }
    for (std::size_t i = eyes.size(); i < player_eyes_.size(); ++i) {
        player_eyes_[i] = Vec3d{};
    }
    player_count_ = static_cast<int>(eyes.size());
    return true;
}

bool Ai::set_player(const Vec3d& eye) {
    return set_players(std::span<const Vec3d>(&eye, 1));
}

void Ai::clear_players() {
    player_count_ = 0;
    player_eyes_.fill(Vec3d{});
    for (TargetHistory& history : histories_) {
        history.reset();
    }
}

bool Ai::player_eye(int index, Vec3d& out) const {
    if (index < 0 || index >= player_count_) {
        return false;
    }
    out = player_eyes_[static_cast<std::size_t>(index)];
    return true;
}

bool Ai::record_target_position(const Vec3d& position) {
    return record_target_position(0, position);
}

bool Ai::record_target_position(int player, const Vec3d& position) {
    if (player < 0 || player >= kAiMaxPlayers || !finite(position)) {
        return false;
    }
    histories_[static_cast<std::size_t>(player)].record(position);
    return true;
}

const TargetHistory& Ai::target_history(int player) const {
    const int index = std::clamp(player, 0, kAiMaxPlayers - 1);
    return histories_[static_cast<std::size_t>(index)];
}

void Ai::reset_target_history() {
    for (TargetHistory& history : histories_) {
        history.reset();
    }
}

bool Ai::target_position(int lag, Vec3d& out) const {
    return target_position(0, lag, out);
}

bool Ai::target_position(int player, int lag, Vec3d& out) const {
    if (player < 0 || player >= kAiMaxPlayers) {
        return false;
    }
    const int clamped = std::clamp(lag, 0, kAiHistoryMaxLag);
    if (clamped == 0 && player < player_count_) {
        out = player_eyes_[static_cast<std::size_t>(player)];
        return true;
    }
    return histories_[static_cast<std::size_t>(player)].at(clamped, out);
}

bool Ai::line_of_sight(const CollisionWorld& world, const Vec3d& from_eye,
                       const Vec3d& to_eye, double distance, double skin) {
    if (!(distance > skin)) {
        return true; // same point: nothing can stand between the eyes
    }
    const Vec3d delta = to_eye - from_eye;
    const auto hit = world.raycast(from_eye, delta, distance);
    return !hit || hit->t >= distance - skin;
}

void Ai::set_state(AiAgent& agent, AiState next, std::uint64_t tick) {
    if (agent.state == next) {
        return;
    }
    const AiState previous = agent.state;
    agent.state = next;
    ++stats_.transitions;
    AiEvent event;
    event.type = AiEventType::state;
    event.thing_id = agent.thing_id;
    event.from = previous;
    event.to = next;
    event.tick = tick;
    events_.push_back(event);
}

double octagonal_distance(const Vec3d& a, const Vec3d& b) {
    std::array<double, 3> d{std::abs(a.x - b.x), std::abs(a.y - b.y),
                            std::abs(a.z - b.z)};
    std::sort(d.begin(), d.end());
    return d[2] + 0.25 * (d[1] + d[0]);
}

void Ai::log(AiEventType type, const AiAgent& agent, double value, std::uint64_t tick,
             std::uint32_t event_id) {
    AiEvent event;
    event.type = type;
    event.thing_id = agent.thing_id;
    event.from = agent.state;
    event.to = agent.state;
    event.target = agent.target;
    event.target_thing = agent.target_is_thing ? agent.target_thing : -1;
    event.aim = agent.aim_point;
    event.value = value;
    event.event_id = event_id;
    event.tick = tick;
    events_.push_back(event);
}

void Ai::refresh_stats() {
    stats_.idle = 0;
    stats_.patrol = 0;
    stats_.alert = 0;
    stats_.attack = 0;
    stats_.dead = 0;
    for (const AiAgent& agent : agents_) {
        switch (agent.state) {
        case AiState::idle:
            ++stats_.idle;
            break;
        case AiState::patrol:
            ++stats_.patrol;
            break;
        case AiState::alert:
            ++stats_.alert;
            break;
        case AiState::attack:
            ++stats_.attack;
            break;
        case AiState::dead:
            ++stats_.dead;
            break;
        }
    }
}

void Ai::tick(const CollisionWorld& world, double dt) {
    if (!std::isfinite(dt) || !(dt > 0.0)) {
        ++stats_.ignored_ticks;
        return;
    }
    if (dt > config_.max_dt) {
        dt = config_.max_dt;
        ++stats_.clamped_ticks;
    }

    ++stats_.ticks;
    const std::uint64_t tick = stats_.ticks;

    // The vitals of the owning things come first: a thing that died
    // elsewhere (the weapons apply `apply_thing_damage`) turns its agent
    // dead before the state machine runs.
    sync_vitals();

    // One target position per player per accepted tick, mirroring the
    // original's per-player history[240][3] (docs/SAVE_FIELDS.md §9.2).
    for (int p = 0; p < player_count_; ++p) {
        histories_[static_cast<std::size_t>(p)].record(
            player_eyes_[static_cast<std::size_t>(p)]);
    }
    const double lose_seconds =
        config_.lose_sight_seconds > 1e-9 ? config_.lose_sight_seconds : kAiTickSeconds;
    const double attack_period =
        config_.attack_period > 1e-9 ? config_.attack_period : kAiTickSeconds;

    for (AiAgent& agent : agents_) {
        if (agent.state == AiState::dead) {
            continue; // dead agents do not tick
        }
        if (!agent.has_home) {
            continue;
        }

        // A5 (opt-in): the weapon cooldown counts down on every live tick,
        // independent of the state, like the original's +0x464 timer.
        if (config_.use_weapon_table && agent.weapon_cooldown > 0.0) {
            agent.weapon_cooldown = std::max(0.0, agent.weapon_cooldown - dt);
        }

        // One A2 seek step (`ai_seek_step`, docs/AI_CPP.md §7): octagonal
        // arrival, swept-sphere wall clamp and the hard leash clamp of
        // `0x470c70` (report §9).  Returns `true` when the target is inside
        // `arrive_radius`.  Only reached with `AiConfig::use_move_step`, so
        // the default scaffold stays byte-for-byte unchanged.
        auto move_params = [](const AiAgent& walker) {
            AiMoveParams params;
            params.speed_per_tick = walker.speed;
            params.radius = walker.radius;
            params.arrive_radius = walker.arrive_radius;
            return params;
        };

        // Shared tail of the seek and retreat steps: wall statistics, the hard
        // leash clamp (+0xd0) and the `move_delta`/`heading` writes.
        auto commit_step = [&](AiAgent& walker, const AiSeekResult& step) -> bool {
            if (step.blocked) {
                ++stats_.blocked_steps;
            }
            if (walker.leash_distance > 0.0) {
                const double before =
                    octagonal_distance(walker.leash_anchor, walker.position);
                const double after =
                    octagonal_distance(walker.leash_anchor, step.position);
                // The step may never *increase* the anchor distance once it
                // is outside the leash; walking back in is always allowed.
                if (after > walker.leash_distance && after > before) {
                    ++stats_.leash_clamps;
                    walker.move_delta = Vec3d{};
                    return false;
                }
            }
            walker.move_delta = step.position - walker.position;
            walker.position = step.position;
            if (length(walker.move_delta) > 1e-12) {
                walker.heading =
                    std::atan2(walker.move_delta.y, walker.move_delta.x);
                ++stats_.move_steps;
            }
            return step.arrived;
        };

        auto move_step = [&](AiAgent& walker, const Vec3d& target,
                             double standoff) -> bool {
            if (standoff > 0.0 &&
                octagonal_distance(walker.position, target) <= standoff) {
                walker.move_delta = Vec3d{};
                return false;
            }
            const AiSeekResult step = ai_seek_step(
                walker.position, target, world, move_params(walker), dt);
            return commit_step(walker, step);
        };

        // A4: one patrol waypoint step.  `path_advance` owns the cursor
        // (`0x475f10`), `move_step` owns the body (`0x476a50`); arrival
        // clears `+0x4f4` through `path_mark_arrived` so the next tick selects
        // the following point.  `pathexpire` turns the agent dead exactly
        // once, mirroring the original's `0x4703a0` post.
        auto path_step = [&](AiAgent& walker, AiPathMode mode) {
            const AiPathStep step = path_advance(
                walker.path, walker.path_waypoints, mode);
            walker.path = step.state;
            if (step.expired) {
                walker.target = -1;
                walker.target_thing = -1;
                walker.target_is_thing = false;
                walker.alertness = 0.0;
                walker.timer = 0.0;
                walker.returning = false;
                walker.move_delta = Vec3d{};
                set_state(walker, AiState::dead, tick);
                ++stats_.path_expiries;
                log(AiEventType::death, walker, 0.0, tick);
                return;
            }
            if (!step.waiting) {
                // `path` stands on its last point (and an empty `path`/`loop`
                // has nothing to walk); no body movement this tick.
                walker.move_delta = Vec3d{};
                return;
            }
            if (move_step(walker, step.point, 0.0)) {
                path_mark_arrived(walker.path);
            }
        };

        // A3: one step straight away from the target (`retreat` branch of
        // `0x473240`), clamped by the same sweep and leash rules.
        auto retreat_step = [&](AiAgent& walker, const Vec3d& threat) {
            const AiSeekResult step = ai_retreat_step(
                walker.position, threat, world, move_params(walker), dt);
            commit_step(walker, step);
            if (length(walker.move_delta) > 1e-12) {
                ++stats_.retreat_steps;
            }
        };

        // One return step (also used right after a leash break below).
        auto return_step = [&](AiAgent& walker) {
            if (config_.use_move_step) {
                if (move_step(walker, walker.home, 0.0)) {
                    walker.position = walker.home;
                    walker.move_delta = Vec3d{};
                    walker.returning = false;
                    walker.timer = 0.0;
                    set_state(walker, AiState::patrol, tick);
                    log(AiEventType::arrived, walker, 0.0, tick);
                }
                return;
            }
            const Vec3d delta = walker.home - walker.position;
            const double distance = length(delta);
            const double step = config_.return_speed * dt;
            if (distance <= step || distance <= 1e-12) {
                walker.position = walker.home;
                walker.returning = false;
                walker.timer = 0.0;
                set_state(walker, AiState::patrol, tick);
                log(AiEventType::arrived, walker, 0.0, tick);
            } else {
                walker.position = walker.position + delta * (step / distance);
            }
        };

        // Return movement: the only place where `position` leaves `home`.
        // Sight is ignored until the agent is back (a deliberate scaffold
        // simplification, documented in docs/AI_CPP.md).
        if (agent.state == AiState::idle && agent.returning) {
            return_step(agent);
            continue;
        }

        const Vec3d eye =
            agent.position + Vec3d{0.0, 0.0, config_.eye_height};
        double distance = 0.0;
        bool visible = false;
        int chosen = -1;
        int chosen_thing = -1;
        double best = std::numeric_limits<double>::infinity();
        double nearest = std::numeric_limits<double>::infinity();
        double nearest_thing = std::numeric_limits<double>::infinity();
        for (int p = 0; p < player_count_; ++p) {
            const Vec3d& target_eye =
                player_eyes_[static_cast<std::size_t>(p)];
            const double d = length(target_eye - eye);
            nearest = std::min(nearest, d);
            if (d >= agent.search_radius) {
                continue;
            }
            ++stats_.los_checks;
            const bool seen =
                line_of_sight(world, eye, target_eye, d, config_.los_skin);
            if (!seen) {
                ++stats_.los_blocked;
            } else if (d < best) {
                best = d;
                chosen = p;
            }
        }
        // `FUN_00473a00` scans the players first and only then the things
        // (8..0x10b).  The C++ target list is the same: absolute, alive
        // things other than the agent itself, nearest visible wins.
        if (chosen < 0 && things_ != nullptr) {
            for (const Thing& thing : things_->things()) {
                if (thing.id == agent.thing_id || !thing.alive ||
                    thing.expired || !thing.absolute()) {
                    continue;
                }
                const auto& p = *thing.position;
                const Vec3d target{
                    static_cast<double>(p[0]),
                    static_cast<double>(p[1]),
                    static_cast<double>(p[2])};
                const double d = length(target - eye);
                nearest_thing = std::min(nearest_thing, d);
                if (d >= agent.search_radius) {
                    continue;
                }
                ++stats_.los_checks;
                const bool seen =
                    line_of_sight(world, eye, target, d, config_.los_skin);
                if (!seen) {
                    ++stats_.los_blocked;
                } else if (d < best) {
                    best = d;
                    chosen_thing = thing.id;
                }
            }
        }

        if (chosen >= 0) {
            agent.target = chosen;
            agent.target_thing = -1;
            agent.target_is_thing = false;
            distance = best;
            visible = true;
        } else if (chosen_thing >= 0) {
            agent.target = -1;
            agent.target_thing = chosen_thing;
            agent.target_is_thing = true;
            distance = best;
            visible = true;
        } else if (agent.target >= 0 && agent.target < player_count_) {
            // Keep the last target for the loss-of-sight fade; the state
            // machine later clears it after `lose_sight_seconds`.
            distance = length(
                player_eyes_[static_cast<std::size_t>(agent.target)] - eye);
        } else if (agent.target_is_thing && things_ != nullptr &&
                   things_->by_id(agent.target_thing) != nullptr) {
            const Thing* thing = things_->by_id(agent.target_thing);
            const auto& p = *thing->position;
            distance = length(Vec3d{static_cast<double>(p[0]),
                                    static_cast<double>(p[1]),
                                    static_cast<double>(p[2])} -
                              eye);
        } else {
            agent.target = -1;
            agent.target_thing = -1;
            agent.target_is_thing = false;
            const double near =
                std::isfinite(nearest) ? nearest : nearest_thing;
            distance = std::isfinite(near) ? near : 0.0;
        }

        // Aim at the player position the thing's +0x1d0 asks for.  `0` is
        // the live eye; positive lags use that player's 240-slot ring and
        // fall back to the live eye until enough history exists.  Things
        // have no history ring, so their current position is the aim point.
        if (agent.target >= 0 && agent.target < player_count_) {
            const Vec3d& target_eye =
                player_eyes_[static_cast<std::size_t>(agent.target)];
            Vec3d aim = target_eye;
            if (agent.history_lag > 0) {
                target_position(agent.target, agent.history_lag, aim);
            }
            agent.aim_point = aim;
            agent.aim_distance = length(aim - eye);
            agent.has_aim = true;
        } else if (agent.target_is_thing && things_ != nullptr) {
            const Thing* thing = things_->by_id(agent.target_thing);
            if (thing == nullptr || !thing->alive || !thing->absolute()) {
                agent.target_thing = -1;
                agent.target_is_thing = false;
                agent.aim_point = Vec3d{};
                agent.aim_distance = 0.0;
                agent.has_aim = false;
            } else {
                const auto& p = *thing->position;
                const Vec3d aim{static_cast<double>(p[0]),
                                static_cast<double>(p[1]),
                                static_cast<double>(p[2])};
                agent.aim_point = aim;
                agent.aim_distance = length(aim - eye);
                agent.has_aim = true;
            }
        } else {
            agent.aim_point = Vec3d{};
            agent.aim_distance = 0.0;
            agent.has_aim = false;
        }
        agent.distance = distance;
        agent.visible = visible;

        // A3 (opt-in): `+0x1cc` is the original's target zone, not a weapon
        // radius.  With `use_move_step` the agent stands and fires inside it,
        // so the alert -> attack gate and the attack zone widen to the
        // standoff.  The original compares a *planar* metric to `+0x1cc`
        // (`0x436774` in `0x473a00`, report §6.2), while `distance` above is
        // 3D eye-to-eye; using the planar distance to the aim point keeps the
        // "stop on the standoff sphere" and "fire inside it" boundaries equal.
        // `use_move_step == false` leaves both gates at `target_zone`.
        const bool standoff_window = config_.use_move_step && agent.standoff > 0.0;
        const double engage_distance =
            standoff_window && agent.has_aim
                ? std::hypot(agent.aim_point.x - agent.position.x,
                             agent.aim_point.y - agent.position.y)
                : distance;
        const double engage_range =
            standoff_window
                ? std::max(static_cast<double>(agent.target_zone), agent.standoff)
                : static_cast<double>(agent.target_zone);

        // Leash: engaged agents too far from home drop the target and walk
        // back.  The radius is measured in the home frame; no navigation mesh
        // is involved.
        if ((agent.state == AiState::alert || agent.state == AiState::attack) &&
            length(agent.position - agent.home) > agent.leash) {
            agent.target = -1;
            agent.target_thing = -1;
            agent.target_is_thing = false;
            agent.alertness = 0.0;
            agent.timer = 0.0;
            agent.returning = true;
            set_state(agent, AiState::idle, tick);
            log(AiEventType::leash, agent, length(agent.position - agent.home), tick);
            return_step(agent); // start walking home on the same tick
            continue;
        }

        switch (agent.state) {
        case AiState::idle:
            if (visible) {
                agent.target = chosen;
                agent.target_thing = chosen_thing;
                agent.target_is_thing = chosen_thing >= 0;
                agent.alertness = 1.0;
                agent.timer = config_.alert_seconds;
                set_state(agent, AiState::alert, tick);
                log(AiEventType::sight, agent, distance, tick);
            } else if (agent.timer > 0.0) {
                agent.timer -= dt;
                if (agent.timer <= 0.0) {
                    agent.timer = 0.0;
                    set_state(agent, AiState::patrol, tick);
                }
            }
            break;
        case AiState::patrol:
            if (visible) {
                agent.target = chosen;
                agent.target_thing = chosen_thing;
                agent.target_is_thing = chosen_thing >= 0;
                agent.alertness = 1.0;
                agent.timer = config_.alert_seconds;
                set_state(agent, AiState::alert, tick);
                log(AiEventType::sight, agent, distance, tick);
            }
            break;
        case AiState::alert:
            if (visible) {
                agent.alertness = 1.0;
                if (agent.timer > 0.0) {
                    agent.timer -= dt;
                }
                if (agent.timer <= 0.0 && engage_distance <= engage_range) {
                    agent.timer = attack_period;
                    set_state(agent, AiState::attack, tick);
                }
            } else {
                agent.alertness -= dt / lose_seconds;
                if (agent.alertness <= 0.0) {
                    agent.alertness = 0.0;
                    agent.target = -1;
                    agent.target_thing = -1;
                    agent.target_is_thing = false;
                    agent.timer = 0.0;
                    set_state(agent, AiState::patrol, tick);
                    log(AiEventType::lost_sight, agent, 0.0, tick);
                }
            }
            break;
        case AiState::attack:
            if (visible) {
                agent.alertness = 1.0;
                // The 1.25x exit hysteresis applies to the A3 standoff path
                // as well; using the bare standoff boundary made a target
                // hovering on the sphere flip alert/attack every tick.
                const double leave_zone =
                    std::max(static_cast<double>(agent.target_zone) * 1.25,
                             engage_range * 1.25);
                if (engage_distance > leave_zone) {
                    agent.timer = 0.0;
                    set_state(agent, AiState::alert, tick);
                } else {
                    if (agent.timer > 0.0) {
                        agent.timer -= dt;
                    }
                    if (agent.timer <= 0.0 &&
                        (!standoff_window || engage_distance <= engage_range)) {
                        agent.timer = attack_period;
                        ++stats_.attacks;
                        log(AiEventType::attack, agent,
                            agent.has_aim ? agent.aim_distance : distance, tick);
                    }
                }
            } else {
                agent.alertness -= dt / lose_seconds;
                if (agent.alertness <= 0.0) {
                    agent.alertness = 0.0;
                    agent.target = -1;
                    agent.target_thing = -1;
                    agent.target_is_thing = false;
                    agent.timer = 0.0;
                    set_state(agent, AiState::patrol, tick);
                    log(AiEventType::lost_sight, agent, 0.0, tick);
                }
            }
            break;
        case AiState::dead:
            break;
        }

        // A5 (opt-in): a separate `weapon` event next to `attack`; the state
        // machine and the A3 window above are untouched.  The band uses the
        // original's octagonal metric (`local_60` of 0x473a00) over the agent
        // position and the aim point.  No projectile is spawned and the player
        // is not damaged here.
        if (config_.use_weapon_table && things_ != nullptr &&
            agent.state == AiState::attack && agent.visible && agent.has_aim &&
            agent.weapon_cooldown <= 0.0 &&
            (!standoff_window || engage_distance <= engage_range)) {
            const Thing* owner = things_->by_id(agent.thing_id);
            if (owner != nullptr) {
                const double weapon_distance =
                    octagonal_distance(agent.position, agent.aim_point);
                AiWeapon selected;
                if (ai_weapon_select(*owner, weapon_distance,
                                     agent.weapon_reverse, selected)) {
                    agent.weapon = selected;
                    agent.weapon_reverse = !agent.weapon_reverse;
                    agent.weapon_cooldown =
                        static_cast<double>(std::max(0, selected.cooldown)) /
                        static_cast<double>(kAiTickHz);
                    ++stats_.weapon_events;
                    log(AiEventType::weapon, agent, weapon_distance, tick,
                        selected.event);
                    if (config_.use_weapon_shots) {
                        AiWeaponShot shot;
                        shot.thing_id = agent.thing_id;
                        shot.event_id = selected.event;
                        shot.origin = agent.position + selected.offset;
                        const Vec3d to_aim = agent.aim_point - shot.origin;
                        const Vec3d direction = normalized(to_aim);
                        shot.direction = finite(direction) &&
                                                 length(direction) > 0.5
                                             ? direction
                                             : Vec3d{0.0, 0.0, 1.0};
                        const double speed = config_.weapon_shot_speed;
                        shot.speed = std::isfinite(speed) && speed > 0.0
                                         ? speed
                                         : 0.0;
                        shot.target = agent.target;
                        shot.target_thing = agent.target_thing;
                        shot.aim = agent.aim_point;
                        shot.distance = weapon_distance;
                        shot.tick = tick;
                        weapon_shots_.push_back(shot);
                        ++stats_.weapon_shots;
                    }
                }
            }
        }

        // A2/A3 movement runs after perception/state so the same tick that
        // acquires a target already takes a step.  Alert/attack pursue the
        // aim point (Euclidean target scan is untouched); patrol walks home,
        // or the A4 route when `use_path` is set.  Idle without `returning`
        // stays put.
        const AiPathMode path_mode =
            config_.load_path_from_types ? agent.path_mode : config_.path_mode;
        const bool route_patrol =
            config_.use_path && agent.state == AiState::patrol &&
            (path_mode == AiPathMode::expire ||
             !agent.path_waypoints.empty());
        if (route_patrol) {
            // A4 (opt-in): the patrol state walks the route; `alert`/`attack`
            // and `idle(returning)` above/below never select a waypoint.
            path_step(agent, path_mode);
        } else if (config_.use_move_step) {
            if (agent.state == AiState::alert || agent.state == AiState::attack) {
                if (agent.has_aim) {
                    // Standoff (`+0x1cc`, report §6.2 / §8):
                    //   d > standoff             -> seek (engage = 1)
                    //   d + 60 <= standoff       -> retreat (engage = -1)
                    //   standoff - 60 < d <= ... -> hold position and fire
                    //                               (engage = 0; the attack
                    //                               state emits the events)
                    const double d =
                        octagonal_distance(agent.position, agent.aim_point);
                    if (!(agent.standoff > 0.0) || d > agent.standoff) {
                        move_step(agent, agent.aim_point, agent.standoff);
                    } else if (d + config_.move_standoff_backoff <=
                               agent.standoff) {
                        retreat_step(agent, agent.aim_point);
                    } else {
                        agent.move_delta = Vec3d{};
                        ++stats_.standoff_holds;
                    }
                }
            } else if (agent.state == AiState::patrol) {
                move_step(agent, agent.home, 0.0);
            }
        }
    }
    refresh_stats();
}

std::size_t Ai::sync_vitals() {
    if (things_ == nullptr) {
        return 0;
    }
    std::size_t changed = 0;
    for (AiAgent& agent : agents_) {
        const Thing* thing = things_->by_id(agent.thing_id);
        if (thing == nullptr) {
            continue;
        }
        // The thing's own gauges are the truth (0x470500 writes them).
        agent.shields = thing->shields;
        agent.hits_left = thing->hits_left;
        agent.burn_timer = thing->burn_timer;
        agent.status_flags = thing->status_flags;
        if (config_.use_thing_fields) {
            agent.health = static_cast<double>(thing->health > 0 ? thing->health : 0);
        }
        const bool dead = !thing->alive ||
                          (config_.use_thing_fields && !agent.immune && agent.health <= 0.0);
        if (dead && agent.state != AiState::dead) {
            agent.target = -1;
            agent.target_thing = -1;
            agent.target_is_thing = false;
            agent.alertness = 0.0;
            agent.timer = 0.0;
            agent.returning = false;
            set_state(agent, AiState::dead, stats_.ticks);
            ++stats_.thing_deaths;
            log(AiEventType::death, agent, 0.0, stats_.ticks);
            ++changed;
        }
    }
    ++stats_.vitals_syncs;
    refresh_stats();
    return changed;
}

bool Ai::damage(int thing_id, double amount) {
    if (!std::isfinite(amount) || !(amount > 0.0)) {
        ++stats_.invalid_damage;
        return false;
    }
    AiAgent* agent = nullptr;
    for (AiAgent& candidate : agents_) {
        if (candidate.thing_id == thing_id) {
            agent = &candidate;
            break;
        }
    }
    if (agent == nullptr || agent->state == AiState::dead) {
        ++stats_.invalid_damage;
        return false;
    }
    if (agent->immune) {
        ++stats_.immune_hits;
        return false;
    }

    agent->health -= amount;
    ++stats_.damage_events;
    log(AiEventType::damage, *agent, amount, stats_.ticks);
    if (agent->health <= 0.0) {
        agent->health = 0.0;
        agent->target = -1;
        agent->target_thing = -1;
        agent->target_is_thing = false;
        agent->alertness = 0.0;
        agent->timer = 0.0;
        agent->returning = false;
        set_state(*agent, AiState::dead, stats_.ticks);
        ++stats_.deaths;
        log(AiEventType::death, *agent, 0.0, stats_.ticks);
    }
    refresh_stats();
    return true;
}

bool Ai::place(int thing_id, const Vec3d& position) {
    if (!finite(position)) {
        return false;
    }
    AiAgent* agent = nullptr;
    for (AiAgent& candidate : agents_) {
        if (candidate.thing_id == thing_id) {
            agent = &candidate;
            break;
        }
    }
    if (agent == nullptr || agent->state == AiState::dead) {
        return false;
    }
    agent->position = position;
    agent->move_delta = Vec3d{};
    return true;
}

bool Ai::set_path(int thing_id, std::span<const Vec3d> waypoints) {
    for (const Vec3d& point : waypoints) {
        if (!finite(point)) {
            return false;
        }
    }
    AiAgent* agent = nullptr;
    for (AiAgent& candidate : agents_) {
        if (candidate.thing_id == thing_id) {
            agent = &candidate;
            break;
        }
    }
    if (agent == nullptr || agent->state == AiState::dead) {
        return false;
    }
    const std::size_t count =
        std::min(waypoints.size(), static_cast<std::size_t>(kAiPathMaxPoints));
    agent->path_waypoints.assign(
        waypoints.begin(),
        waypoints.begin() + static_cast<std::ptrdiff_t>(count));
    agent->path = AiPathState{};
    return true;
}

bool Ai::set_path_from_thing(int thing_id) {
    if (things_ == nullptr) {
        return false;
    }
    const Thing* thing = things_->by_id(thing_id);
    if (thing == nullptr || thing->path_locations.empty()) {
        return false;
    }
    AiAgent* agent = nullptr;
    for (AiAgent& candidate : agents_) {
        if (candidate.thing_id == thing_id) {
            agent = &candidate;
            break;
        }
    }
    if (agent == nullptr || agent->state == AiState::dead) {
        return false;
    }
    std::vector<Vec3d> points;
    points.reserve(std::min(thing->path_locations.size(),
                            static_cast<std::size_t>(kAiPathMaxPoints)));
    for (const int id : thing->path_locations) {
        if (points.size() >= static_cast<std::size_t>(kAiPathMaxPoints)) {
            break;
        }
        const VmLocation* location = things_->location(id);
        if (location == nullptr) {
            continue; // 0x476023: the id is not in 0x80d8e0, point unusable
        }
        const Vec3d point{static_cast<double>(location->pos[0]),
                          static_cast<double>(location->pos[1]),
                          static_cast<double>(location->pos[2])};
        if (!finite(point)) {
            continue;
        }
        points.push_back(point);
    }
    if (points.empty()) {
        return false;
    }
    AiPathMode mode;
    if (ai_path_mode(thing->movement_type, mode)) {
        agent->path_mode = mode;
    }
    agent->path_waypoints = std::move(points);
    agent->path = AiPathState{};
    return true;
}

std::vector<AiWeaponShot> Ai::take_weapon_shots() {
    std::vector<AiWeaponShot> out;
    out.swap(weapon_shots_);
    return out;
}

const AiAgent* Ai::agent(int thing_id) const {
    for (const AiAgent& candidate : agents_) {
        if (candidate.thing_id == thing_id) {
            return &candidate;
        }
    }
    return nullptr;
}

} // namespace gm
