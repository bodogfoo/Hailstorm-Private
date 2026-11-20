#include "combat/movement_controller.h"

#include <cmath>
#include <sstream>

namespace hailstone::combat {
namespace {

bool approx_eq(float a, float b) noexcept {
    return std::fabs(a - b) < 0.25f;
}

} // namespace

void MovementController::reset() {
    active_command_ = MovementCommandKind::None;
    active_target_id_ = 0;
    active_behind_ = false;
    active_param_a_ = 0.0f;
    active_param_b_ = 0.0f;
    manual_override_until_ms_ = 0;
    manual_override_logged_ = false;
    last_decision_ = "Reset";
}

void MovementController::clear_manual_override() {
    manual_override_until_ms_ = 0;
    manual_override_logged_ = false;
}

bool MovementController::manual_override_active(std::uint64_t now_ms) const noexcept {
    return manual_override_until_ms_ > now_ms;
}

std::uint64_t MovementController::manual_override_remaining_ms(std::uint64_t now_ms) const noexcept {
    if (manual_override_until_ms_ <= now_ms) {
        return 0;
    }
    return manual_override_until_ms_ - now_ms;
}

MovementStepResult MovementController::force_stop(std::string_view reason,
                                                  ICombatActions& actions,
                                                  bool dry_run,
                                                  const CombatSnapshot* snapshot,
                                                  bool unconditional) {
    MovementStepResult res;
    res.command = MovementCommandKind::Stop;
    const bool had_nav = (snapshot && (snapshot->nav_active || snapshot->nav_stick_spawn != 0));
    const bool was_moving = controller_moving() || had_nav;

    active_command_ = MovementCommandKind::None;
    active_target_id_ = 0;
    active_behind_ = false;
    active_param_a_ = 0.0f;
    active_param_b_ = 0.0f;
    last_decision_ = "Stop: " + std::string(reason);
    res.decision = last_decision_;

    if (was_moving || unconditional) {
        if (!dry_run) {
            actions.stop_movement(reason);
        } else {
            actions.log_message("movement", "[DRY-RUN] stop_movement: " + std::string(reason));
        }
        res.issued_to_actions = true;
    }
    return res;
}

MovementStepResult MovementController::update(CombatState state,
                                              const CombatSnapshot& snapshot,
                                              const CombatProfile& profile,
                                              const SpawnSnapshot* hostile_target,
                                              const SpawnSnapshot* assist_spawn,
                                              const SpawnSnapshot* follow_spawn,
                                              ICombatActions& actions,
                                              bool dry_run) {
    MovementStepResult res;

    // Hard halts (disabled/Dead/EmergencyStop) stop immediately without latching override.
    if (!profile.enabled ||
        state == CombatState::Dead ||
        state == CombatState::EmergencyStop) {
        return force_stop(combat_state_name(state), actions, dry_run, &snapshot);
    }

    // Detect manual player movement / explicit back-away BEFORE Paused early return so cooldown latches.
    if (profile.pause_on_manual_move &&
        (snapshot.manual_move_backward || snapshot.player_moving_manually)) {
        const std::uint64_t hold_ms =
            profile.manual_override_cooldown_ms > 0 ? profile.manual_override_cooldown_ms : 3500ULL;
        manual_override_until_ms_ = snapshot.now_ms + hold_ms;

        const bool first_detect = !manual_override_logged_;
        manual_override_logged_ = true;

        res = force_stop(snapshot.manual_move_backward ? "manual back-away" : "manual movement",
                         actions, dry_run, &snapshot);
        // If user backed away while controller had not flagged active_command_ yet, still ensure nav stops once.
        if (first_detect && !res.issued_to_actions) {
            if (!dry_run) {
                actions.stop_movement("manual override");
            } else {
                actions.log_message("movement", "[DRY-RUN] stop_movement: manual override");
            }
            res.issued_to_actions = true;
        }
        res.manual_override_active = true;
        res.manual_override_remaining_ms = hold_ms;
        last_decision_ = snapshot.manual_move_backward
                             ? "Manual override (player backed away)"
                             : "Manual override (player moving)";
        res.decision = last_decision_;
        return res;
    }

    if (state == CombatState::Paused) {
        return force_stop("Paused", actions, dry_run, &snapshot);
    }
    if (manual_override_active(snapshot.now_ms)) {
        res.manual_override_active = true;
        res.manual_override_remaining_ms = manual_override_remaining_ms(snapshot.now_ms);
        last_decision_ = "Manual override hold (" +
                         std::to_string((res.manual_override_remaining_ms + 999) / 1000) + "s)";
        res.decision = last_decision_;
        return res;
    }
    manual_override_logged_ = false;

    // Disengage / Retreat movement away from hostile target.
    if (state == CombatState::Retreat || state == CombatState::Disengage) {
        if (state == CombatState::Retreat && hostile_target && hostile_target->id != 0) {
            const float dist = snapshot.me.position.distance_2d(hostile_target->position);
            const float desired_dist = std::max(profile.disengage_distance,
                                                profile.ranged_max_range + profile.retreat_distance);
            if (dist < desired_dist) {
                const bool same = (active_command_ == MovementCommandKind::MoveDisengage &&
                                   active_target_id_ == hostile_target->id &&
                                   approx_eq(active_param_a_, desired_dist) &&
                                   snapshot.nav_active);
                std::ostringstream oss;
                oss << "Retreat away from " << hostile_target->name
                    << " (" << static_cast<int>(dist) << " -> " << static_cast<int>(desired_dist) << ")";
                last_decision_ = oss.str();
                res.command = MovementCommandKind::MoveDisengage;
                res.decision = last_decision_;
                if (!same) {
                    active_command_ = MovementCommandKind::MoveDisengage;
                    active_target_id_ = hostile_target->id;
                    active_param_a_ = desired_dist;
                    active_param_b_ = 0.0f;
                    active_behind_ = false;
                    if (!dry_run) {
                        actions.move_disengage(hostile_target->position, desired_dist);
                    } else {
                        actions.log_message("movement", "[DRY-RUN] " + last_decision_);
                    }
                    res.issued_to_actions = true;
                }
                return res;
            }
        }
        return force_stop(state == CombatState::Retreat ? "Retreat distance reached" : "Disengaged",
                          actions, dry_run, &snapshot);
    }

    // Idle state: either optional Follow, or stopped.
    if (state == CombatState::Idle || state == CombatState::Assist) {
        if (profile.movement_mode == MovementMode::Follow && follow_spawn && follow_spawn->id != 0 &&
            !follow_spawn->dead) {
            const float dist = snapshot.me.position.distance_2d(follow_spawn->position);
            if (dist > profile.follow_distance) {
                const bool same = (active_command_ == MovementCommandKind::Follow &&
                                   active_target_id_ == follow_spawn->id &&
                                   approx_eq(active_param_a_, profile.follow_distance) &&
                                   snapshot.nav_active);
                last_decision_ = "Follow " + follow_spawn->name + " (dist " +
                                 std::to_string(static_cast<int>(dist)) + ")";
                res.command = MovementCommandKind::Follow;
                res.decision = last_decision_;
                if (!same) {
                    active_command_ = MovementCommandKind::Follow;
                    active_target_id_ = follow_spawn->id;
                    active_param_a_ = profile.follow_distance;
                    active_param_b_ = 0.0f;
                    active_behind_ = false;
                    if (!dry_run) {
                        actions.move_follow(follow_spawn->id, profile.follow_distance);
                    } else {
                        actions.log_message("movement", "[DRY-RUN] " + last_decision_);
                    }
                    res.issued_to_actions = true;
                }
                return res;
            }
            return force_stop("Within follow range", actions, dry_run, &snapshot);
        }

        if (state == CombatState::Assist && profile.movement_mode == MovementMode::AssistRange &&
            assist_spawn && assist_spawn->id != 0 && !assist_spawn->dead) {
            const float dist = snapshot.me.position.distance_2d(assist_spawn->position);
            if (dist > profile.assist_range) {
                const bool same = (active_command_ == MovementCommandKind::MoveToRange &&
                                   active_target_id_ == assist_spawn->id &&
                                   approx_eq(active_param_b_, profile.assist_range) &&
                                   snapshot.nav_active);
                last_decision_ = "Move within assist range of " + assist_spawn->name;
                res.command = MovementCommandKind::MoveToRange;
                res.decision = last_decision_;
                if (!same) {
                    active_command_ = MovementCommandKind::MoveToRange;
                    active_target_id_ = assist_spawn->id;
                    active_param_a_ = 0.0f;
                    active_param_b_ = profile.assist_range;
                    active_behind_ = false;
                    if (!dry_run) {
                        actions.move_to_range(assist_spawn->id, 0.0f, profile.assist_range);
                    } else {
                        actions.log_message("movement", "[DRY-RUN] " + last_decision_);
                    }
                    res.issued_to_actions = true;
                }
                return res;
            }
        }

        return force_stop("Idle hold", actions, dry_run, &snapshot);
    }

    // Engage or Combat states.
    if (!hostile_target || hostile_target->id == 0 || hostile_target->dead || hostile_target->is_corpse) {
        return force_stop("No active target", actions, dry_run, &snapshot);
    }

    const float dist = snapshot.me.position.distance_2d(hostile_target->position);

    switch (profile.movement_mode) {
    case MovementMode::None:
        return force_stop("Movement mode None", actions, dry_run, &snapshot);

    case MovementMode::DisengageDistance: {
        if (dist < profile.disengage_distance) {
            const bool same = (active_command_ == MovementCommandKind::MoveDisengage &&
                               active_target_id_ == hostile_target->id &&
                               approx_eq(active_param_a_, profile.disengage_distance));
            last_decision_ = "Keep disengage distance (" + std::to_string(static_cast<int>(dist)) +
                             " < " + std::to_string(static_cast<int>(profile.disengage_distance)) + ")";
            res.command = MovementCommandKind::MoveDisengage;
            res.decision = last_decision_;
            if (!same) {
                active_command_ = MovementCommandKind::MoveDisengage;
                active_target_id_ = hostile_target->id;
                active_param_a_ = profile.disengage_distance;
                if (!dry_run) {
                    actions.move_disengage(hostile_target->position, profile.disengage_distance);
                } else {
                    actions.log_message("movement", "[DRY-RUN] " + last_decision_);
                }
                res.issued_to_actions = true;
            }
            return res;
        }
        return force_stop("Outside disengage distance", actions, dry_run, &snapshot);
    }

    case MovementMode::Follow: {
        if (follow_spawn && follow_spawn->id != 0 && !follow_spawn->dead) {
            const float fdist = snapshot.me.position.distance_2d(follow_spawn->position);
            if (fdist > profile.follow_distance) {
                const bool same = (active_command_ == MovementCommandKind::Follow &&
                                   active_target_id_ == follow_spawn->id &&
                                   approx_eq(active_param_a_, profile.follow_distance) &&
                                   snapshot.nav_active);
                last_decision_ = "Follow " + follow_spawn->name;
                res.command = MovementCommandKind::Follow;
                res.decision = last_decision_;
                if (!same) {
                    active_command_ = MovementCommandKind::Follow;
                    active_target_id_ = follow_spawn->id;
                    active_param_a_ = profile.follow_distance;
                    if (!dry_run) {
                        actions.move_follow(follow_spawn->id, profile.follow_distance);
                    } else {
                        actions.log_message("movement", "[DRY-RUN] " + last_decision_);
                    }
                    res.issued_to_actions = true;
                }
                return res;
            }
        }
        return force_stop("Within follow distance", actions, dry_run, &snapshot);
    }

    case MovementMode::AssistRange: {
        if (assist_spawn && assist_spawn->id != 0 && !assist_spawn->dead) {
            const float adist = snapshot.me.position.distance_2d(assist_spawn->position);
            if (adist > profile.assist_range) {
                const bool same = (active_command_ == MovementCommandKind::MoveToRange &&
                                   active_target_id_ == assist_spawn->id &&
                                   approx_eq(active_param_b_, profile.assist_range) &&
                                   snapshot.nav_active);
                last_decision_ = "Approach assist range of " + assist_spawn->name;
                res.command = MovementCommandKind::MoveToRange;
                res.decision = last_decision_;
                if (!same) {
                    active_command_ = MovementCommandKind::MoveToRange;
                    active_target_id_ = assist_spawn->id;
                    active_param_a_ = 0.0f;
                    active_param_b_ = profile.assist_range;
                    if (!dry_run) {
                        actions.move_to_range(assist_spawn->id, 0.0f, profile.assist_range);
                    } else {
                        actions.log_message("movement", "[DRY-RUN] " + last_decision_);
                    }
                    res.issued_to_actions = true;
                }
                return res;
            }
        }
        return force_stop("Within assist range", actions, dry_run, &snapshot);
    }

    case MovementMode::MeleeRange: {
        const float melee_dist = std::max(4.0f, profile.melee_range);
        const bool already_sticking_same = (active_command_ == MovementCommandKind::MoveMelee &&
                                            active_target_id_ == hostile_target->id &&
                                            active_behind_ == profile.melee_behind &&
                                            approx_eq(active_param_a_, melee_dist) &&
                                            snapshot.nav_stick_spawn == hostile_target->id);
        if (dist > melee_dist || profile.melee_behind || !already_sticking_same) {
            std::ostringstream oss;
            oss << "Melee " << (profile.melee_behind ? "(behind) " : "") << hostile_target->name
                << " dist=" << static_cast<int>(dist) << "/" << static_cast<int>(melee_dist);
            last_decision_ = oss.str();
            res.command = MovementCommandKind::MoveMelee;
            res.decision = last_decision_;
            if (!already_sticking_same) {
                active_command_ = MovementCommandKind::MoveMelee;
                active_target_id_ = hostile_target->id;
                active_param_a_ = melee_dist;
                active_param_b_ = 0.0f;
                active_behind_ = profile.melee_behind;
                if (!dry_run) {
                    actions.move_melee(hostile_target->id, melee_dist, profile.melee_behind);
                } else {
                    actions.log_message("movement", "[DRY-RUN] " + last_decision_);
                }
                res.issued_to_actions = true;
            }
            return res;
        }
        return force_stop("In melee range", actions, dry_run, &snapshot);
    }

    case MovementMode::RangedRange: {
        const float min_r = std::max(0.0f, profile.ranged_min_range);
        const float max_r = std::max(min_r + 10.0f, profile.ranged_max_range);
        if (dist > max_r) {
            const bool same = (active_command_ == MovementCommandKind::MoveToRange &&
                               active_target_id_ == hostile_target->id &&
                               approx_eq(active_param_a_, min_r) &&
                               approx_eq(active_param_b_, max_r) &&
                               snapshot.nav_active);
            std::ostringstream oss;
            oss << "Close to ranged band [" << static_cast<int>(min_r) << ", "
                << static_cast<int>(max_r) << "] dist=" << static_cast<int>(dist);
            last_decision_ = oss.str();
            res.command = MovementCommandKind::MoveToRange;
            res.decision = last_decision_;
            if (!same) {
                active_command_ = MovementCommandKind::MoveToRange;
                active_target_id_ = hostile_target->id;
                active_param_a_ = min_r;
                active_param_b_ = max_r;
                active_behind_ = false;
                if (!dry_run) {
                    actions.move_to_range(hostile_target->id, min_r, max_r);
                } else {
                    actions.log_message("movement", "[DRY-RUN] " + last_decision_);
                }
                res.issued_to_actions = true;
            }
            return res;
        }
        if (min_r > 0.0f && dist < min_r && !snapshot.casting) {
            const bool same = (active_command_ == MovementCommandKind::MoveToRange &&
                               active_target_id_ == hostile_target->id &&
                               approx_eq(active_param_a_, min_r) &&
                               approx_eq(active_param_b_, max_r) &&
                               snapshot.nav_active);
            std::ostringstream oss;
            oss << "Hold minimum ranged distance [" << static_cast<int>(min_r)
                << "] dist=" << static_cast<int>(dist);
            last_decision_ = oss.str();
            res.command = MovementCommandKind::MoveToRange;
            res.decision = last_decision_;
            if (!same) {
                active_command_ = MovementCommandKind::MoveToRange;
                active_target_id_ = hostile_target->id;
                active_param_a_ = min_r;
                active_param_b_ = max_r;
                active_behind_ = false;
                if (!dry_run) {
                    actions.move_to_range(hostile_target->id, min_r, max_r);
                } else {
                    actions.log_message("movement", "[DRY-RUN] " + last_decision_);
                }
                res.issued_to_actions = true;
            }
            return res;
        }
        return force_stop("Inside ranged band", actions, dry_run, &snapshot);
    }
    }

    return res;
}

} // namespace hailstone::combat
