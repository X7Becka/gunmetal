#include "world/ai.h"

#include <algorithm>
#include <cmath>
#include <utility>

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

std::size_t Ai::attach(const Things& things) {
    agents_.clear();
    events_.clear();
    stats_ = AiStats{};

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
        if (config_.use_thing_fields && thing.type >= 0 && !thing.type_name.empty()) {
            agent.movement_type =
                (thing.movement_type >= 0 && thing.movement_type <= 18)
                    ? thing.movement_type
                    : 0;
            agent.search_radius = thing.search_radius;
            agent.target_zone = thing.target_zone_threshold;
            agent.history_lag = std::clamp(thing.target_history_offset, 0, 239);
            agent.max_health = static_cast<double>(std::max(0, thing.max_health));
            agent.immune = thing.max_health == 0;
            agent.health = thing.alive ? (agent.immune ? 0.0 : agent.max_health) : 0.0;
            ++stats_.typed_agents;
        } else {
            agent.search_radius = config_.sight_range;
            agent.target_zone = static_cast<int>(config_.attack_range);
            agent.max_health = config_.health;
            agent.health = thing.alive ? config_.health : 0.0;
        }
        agent.state = thing.alive ? AiState::idle : AiState::dead;
        agent.timer = thing.alive ? config_.patrol_after_seconds : 0.0;
        agents_.push_back(agent);
        log(AiEventType::attach, agents_.back(), 0.0, 0);
    }
    stats_.attached = agents_.size();
    refresh_stats();
    return agents_.size();
}

bool Ai::set_player(const Vec3d& eye) {
    if (!finite(eye)) {
        return false;
    }
    player_eye_ = eye;
    has_player_ = true;
    return true;
}

void Ai::clear_player() {
    has_player_ = false;
    player_eye_ = Vec3d{};
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

void Ai::log(AiEventType type, const AiAgent& agent, double value, std::uint64_t tick) {
    AiEvent event;
    event.type = type;
    event.thing_id = agent.thing_id;
    event.from = agent.state;
    event.to = agent.state;
    event.value = value;
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

        // One return step (also used right after a leash break below).
        auto return_step = [&](AiAgent& walker) {
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

        double distance = 0.0;
        bool visible = false;
        if (has_player_) {
            const Vec3d eye = agent.position + Vec3d{0.0, 0.0, config_.eye_height};
            const Vec3d delta = player_eye_ - eye;
            distance = length(delta);
            if (distance < agent.search_radius) {
                ++stats_.los_checks;
                visible = line_of_sight(world, eye, player_eye_, distance, config_.los_skin);
                if (!visible) {
                    ++stats_.los_blocked;
                }
            }
        }
        agent.distance = distance;
        agent.visible = visible;

        // Leash: engaged agents too far from home drop the target and walk
        // back.  The radius is measured in the home frame; no navigation mesh
        // is involved.
        if ((agent.state == AiState::alert || agent.state == AiState::attack) &&
            length(agent.position - agent.home) > agent.leash) {
            agent.target = -1;
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
                agent.target = 0;
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
                agent.target = 0;
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
                if (agent.timer <= 0.0 && distance <= static_cast<double>(agent.target_zone)) {
                    agent.timer = attack_period;
                    set_state(agent, AiState::attack, tick);
                }
            } else {
                agent.alertness -= dt / lose_seconds;
                if (agent.alertness <= 0.0) {
                    agent.alertness = 0.0;
                    agent.target = -1;
                    agent.timer = 0.0;
                    set_state(agent, AiState::patrol, tick);
                    log(AiEventType::lost_sight, agent, 0.0, tick);
                }
            }
            break;
        case AiState::attack:
            if (visible) {
                agent.alertness = 1.0;
                const double leave_zone = config_.use_thing_fields
                                             ? static_cast<double>(agent.target_zone)
                                             : static_cast<double>(agent.target_zone) * 1.25;
                if (distance > leave_zone) {
                    agent.timer = 0.0;
                    set_state(agent, AiState::alert, tick);
                } else {
                    if (agent.timer > 0.0) {
                        agent.timer -= dt;
                    }
                    if (agent.timer <= 0.0) {
                        agent.timer = attack_period;
                        ++stats_.attacks;
                        log(AiEventType::attack, agent, distance, tick);
                    }
                }
            } else {
                agent.alertness -= dt / lose_seconds;
                if (agent.alertness <= 0.0) {
                    agent.alertness = 0.0;
                    agent.target = -1;
                    agent.timer = 0.0;
                    set_state(agent, AiState::patrol, tick);
                    log(AiEventType::lost_sight, agent, 0.0, tick);
                }
            }
            break;
        case AiState::dead:
            break;
        }
    }
    refresh_stats();
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
    return true;
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
