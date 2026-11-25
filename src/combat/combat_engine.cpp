#include "combat/combat_engine.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "combat/spell_manager.h"

namespace hailstone::combat {
namespace {

std::string to_lower_copy(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

void CombatEngine::set_profile(CombatProfile profile) {
    profile_ = std::move(profile);
    last_rule_used_ms_.clear();
}

void CombatEngine::start(std::string_view reason) {
    if (state_ == CombatState::EmergencyStop) {
        append_action(last_tick_ms_, profile_.dry_run, "safety",
                      "Cannot start while EmergencyStop is latched; call reset_emergency_stop first",
                      nullptr);
        return;
    }
    running_ = true;
    user_paused_ = false;
    auto_manual_paused_ = false;
    movement_.clear_manual_override();
    if (state_ == CombatState::Paused) {
        transition_to(CombatState::Idle, last_tick_ms_, reason, nullptr);
    } else {
        append_action(last_tick_ms_, profile_.dry_run, "control",
                      std::string(reason), nullptr);
    }
    idle_reason_ = "Waiting for next tick";
}

void CombatEngine::stop(ICombatActions* actions, std::string_view reason) {
    running_ = false;
    user_paused_ = false;
    auto_manual_paused_ = false;
    engaged_target_id_ = 0;
    engaged_target_name_.clear();
    engaged_target_hp_pct_ = 0;
    engaged_target_distance_ = 0.0f;
    pet_attack_target_id_ = 0;
    if (actions) {
        stop_offensive_actions(nullptr, *actions, reason);
        movement_.force_stop(reason, *actions, profile_.dry_run, nullptr);
    } else {
        auto_attack_commanded_ = false;
    }
    if (state_ != CombatState::Dead && state_ != CombatState::EmergencyStop) {
        transition_to(CombatState::Idle, last_tick_ms_, reason, actions);
    } else {
        append_action(last_tick_ms_, profile_.dry_run, "control", std::string(reason), actions);
    }
    idle_reason_ = std::string(reason);
}

void CombatEngine::pause(ICombatActions* actions, std::string_view reason) {
    if (state_ == CombatState::EmergencyStop || state_ == CombatState::Dead) {
        return;
    }
    user_paused_ = true;
    auto_manual_paused_ = false;
    if (actions) {
        stop_offensive_actions(nullptr, *actions, reason);
        movement_.force_stop(reason, *actions, profile_.dry_run, nullptr);
    } else {
        auto_attack_commanded_ = false;
    }
    transition_to(CombatState::Paused, last_tick_ms_, reason, actions);
}

void CombatEngine::resume(std::string_view reason) {
    if (state_ == CombatState::EmergencyStop) {
        append_action(last_tick_ms_, profile_.dry_run, "safety",
                      "Cannot resume while EmergencyStop is latched; reset E-Stop first",
                      nullptr);
        return;
    }
    user_paused_ = false;
    auto_manual_paused_ = false;
    movement_.clear_manual_override();
    if (state_ == CombatState::Paused) {
        transition_to(CombatState::Idle, last_tick_ms_, reason, nullptr);
    }
    idle_reason_ = running_ ? "Resumed" : "Controller stopped";
}

void CombatEngine::emergency_stop(ICombatActions& actions, std::string_view reason) {
    running_ = false;
    user_paused_ = false;
    auto_manual_paused_ = false;
    engaged_target_id_ = 0;
    engaged_target_name_.clear();
    engaged_target_hp_pct_ = 0;
    engaged_target_distance_ = 0.0f;
    pet_attack_target_id_ = 0;

    if (!profile_.dry_run) {
        actions.stop_cast();
        actions.set_auto_attack(false);
        actions.pet_back();
    } else {
        actions.log_message("safety", "[DRY-RUN] emergency_stop: stop_cast + auto_attack(false) + pet_back");
    }
    auto_attack_commanded_ = false;
    movement_.force_stop(reason, actions, profile_.dry_run, nullptr, true);
    transition_to(CombatState::EmergencyStop, last_tick_ms_, reason, &actions);
    idle_reason_ = "Emergency stop latched";
}

bool CombatEngine::reset_emergency_stop(std::string_view reason) {
    if (state_ != CombatState::EmergencyStop) {
        return false;
    }
    running_ = false;
    user_paused_ = false;
    auto_manual_paused_ = false;
    movement_.clear_manual_override();
    transition_to(CombatState::Idle, last_tick_ms_, reason, nullptr);
    idle_reason_ = "Emergency stop cleared (stopped)";
    return true;
}

void CombatEngine::set_dry_run(bool dry_run) {
    profile_.dry_run = dry_run;
    append_action(last_tick_ms_, dry_run, "config",
                  dry_run ? "Dry-run simulation enabled" : "Dry-run simulation disabled", nullptr);
}

void CombatEngine::set_auto_loadout(bool auto_loadout) {
    profile_.auto_loadout = auto_loadout;
    append_action(last_tick_ms_, profile_.dry_run, "config",
                  auto_loadout ? "Automatic loadout overwrite enabled"
                               : "Automatic loadout overwrite disabled (manual gems protected)",
                  nullptr);
}

void CombatEngine::set_movement_mode(MovementMode mode) {
    profile_.movement_mode = mode;
    append_action(last_tick_ms_, profile_.dry_run, "config",
                  "Movement mode set to " + std::string(movement_mode_name(mode)), nullptr);
}

void CombatEngine::set_assist_target(std::string assist_target) {
    profile_.assist_target = std::move(assist_target);
    append_action(last_tick_ms_, profile_.dry_run, "config",
                  "Assist target set to '" + profile_.assist_target + "'", nullptr);
}

void CombatEngine::set_follow_target(std::string follow_target) {
    profile_.follow_target = std::move(follow_target);
    if (profile_.follow_target.empty()) {
        if (profile_.movement_mode == MovementMode::Follow) profile_.movement_mode = MovementMode::None;
    } else {
        profile_.movement_mode = MovementMode::Follow;
    }
    append_action(last_tick_ms_, profile_.dry_run, "config",
                  "Follow target set to '" + profile_.follow_target + "'", nullptr);
}

void CombatEngine::request_rememorize() {
    rememorize_requested_ = true;
    append_action(last_tick_ms_, profile_.dry_run, "spells",
                  "Requested rememorize check", nullptr);
}

void CombatEngine::clear_logs() {
    transitions_.clear();
    actions_.clear();
    failed_memorizations_.clear();
    pending_memo_checks_.clear();
}

void CombatEngine::record_memo_failure(std::string msg) {
    if (msg.empty()) return;
    for (const auto& existing : failed_memorizations_) {
        if (existing == msg) return;
    }
    if (failed_memorizations_.size() >= 16) failed_memorizations_.pop_front();
    failed_memorizations_.push_back(std::move(msg));
}

void CombatEngine::check_pending_memorizations(const CombatSnapshot& snapshot, ICombatActions& actions) {
    // Clear stale failure entries for gems that now match their desired spell.
    if (!failed_memorizations_.empty()) {
        for (const auto& slot : last_audit_.slots) {
            if (slot.status == GemSyncKind::Match) {
                const std::string prefix = "Gem " + std::to_string(slot.gem_index + 1) + ":";
                std::erase_if(failed_memorizations_, [&](const std::string& msg) {
                    return msg.rfind(prefix, 0) == 0;
                });
            }
        }
    }
    if (pending_memo_checks_.empty()) return;
    std::vector<PendingMemoCheck> still_pending;
    for (const auto& chk : pending_memo_checks_) {
        bool memorized = false;
        for (const auto& g : snapshot.gems) {
            if (g.gem_index == chk.gem_index && g.spell_id == chk.spell_id && g.spell_id != 0) {
                memorized = true;
                break;
            }
        }
        if (memorized) continue;
        if (!snapshot.memorizing && snapshot.now_ms >= chk.started_ms &&
            (snapshot.now_ms - chk.started_ms) >= 1500ULL) {
            std::string msg = "Gem " + std::to_string(chk.gem_index + 1) + ": " + chk.spell_name +
                              " did not memorize (interrupted or rejected by server)";
            record_memo_failure(msg);
            append_action(snapshot.now_ms, false, "spells", "Memorization failed: " + msg, &actions);
            continue;
        }
        still_pending.push_back(chk);
    }
    pending_memo_checks_ = std::move(still_pending);
}

void CombatEngine::on_zone_change(ICombatActions* actions, std::string_view new_zone) {
    zone_short_ = std::string(new_zone);
    engaged_target_id_ = 0;
    engaged_target_name_.clear();
    engaged_target_hp_pct_ = 0;
    engaged_target_distance_ = 0.0f;
    pet_attack_target_id_ = 0;
    assist_target_id_ = 0;
    assist_target_name_.clear();
    if (actions) {
        stop_offensive_actions(nullptr, *actions, "Zone change");
        movement_.force_stop("Zone change", *actions, profile_.dry_run, nullptr);
    }
    if (state_ != CombatState::EmergencyStop && state_ != CombatState::Paused) {
        transition_to(CombatState::Idle, last_tick_ms_, "Zone changed to " + zone_short_, actions);
    }
}

void CombatEngine::transition_to(CombatState next,
                                 std::uint64_t now_ms,
                                 std::string_view reason,
                                 ICombatActions* actions) {
    const CombatState prev = state_;
    if (prev == next && reason == transition_reason_) {
        return;
    }
    state_ = next;
    transition_reason_ = std::string(reason);
    if (prev == next) {
        return;
    }

    TransitionLogEntry entry;
    entry.timestamp_ms = now_ms;
    entry.from_state = prev;
    entry.to_state = next;
    entry.reason = transition_reason_;
    transitions_.push_back(entry);
    while (transitions_.size() > kMaxLogEntries) {
        transitions_.pop_front();
    }

    const std::string msg = std::string(combat_state_name(prev)) + " -> " +
                            std::string(combat_state_name(next)) + ": " + transition_reason_;
    append_action(now_ms, profile_.dry_run, "state", msg, actions);
}

void CombatEngine::append_action(std::uint64_t now_ms,
                                 bool dry_run,
                                 std::string_view category,
                                 std::string detail,
                                 ICombatActions* actions) {
    last_action_ = detail;
    ActionLogEntry entry;
    entry.timestamp_ms = now_ms;
    entry.dry_run = dry_run;
    entry.category = std::string(category);
    entry.detail = detail;
    actions_.push_back(std::move(entry));
    while (actions_.size() > kMaxLogEntries) {
        actions_.pop_front();
    }
    if (actions) {
        actions->log_message(category, dry_run ? ("[DRY-RUN] " + detail) : detail);
    }
}

void CombatEngine::stop_offensive_actions(const CombatSnapshot* snapshot,
                                          ICombatActions& actions,
                                          std::string_view reason) {
    const bool attack_on = (snapshot && snapshot->auto_attack_on) || auto_attack_commanded_;
    if (attack_on) {
        if (!profile_.dry_run) {
            actions.set_auto_attack(false);
        } else {
            actions.log_message("combat", "[DRY-RUN] set_auto_attack(false): " + std::string(reason));
        }
    }
    auto_attack_commanded_ = false;
    if (pet_attack_target_id_ != 0) {
        if (!profile_.dry_run) {
            actions.pet_back();
        } else {
            actions.log_message("combat", "[DRY-RUN] pet_back: " + std::string(reason));
        }
        pet_attack_target_id_ = 0;
    }
}

const SpawnSnapshot* CombatEngine::find_spawn_by_id(const CombatSnapshot& snapshot,
                                                    std::uint32_t id) const {
    if (id == 0) {
        return nullptr;
    }
    if (snapshot.me.id == id) {
        return &snapshot.me;
    }
    if (snapshot.has_target && snapshot.target.id == id) {
        return &snapshot.target;
    }
    for (const auto& s : snapshot.spawns) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

const SpawnSnapshot* CombatEngine::resolve_assist_spawn(const CombatSnapshot& snapshot,
                                                        std::string& resolved_name) const {
    resolved_name.clear();
    const std::string key = to_lower_copy(profile_.assist_target);
    if (key.empty()) {
        return nullptr;
    }

    auto find_member_spawn = [&](const GroupMemberSnapshot& m) -> const SpawnSnapshot* {
        if (m.offline || !m.in_zone || m.spawn_id == 0) {
            return nullptr;
        }
        return find_spawn_by_id(snapshot, m.spawn_id);
    };

    if (key == "main_assist" || key == "assist") {
        for (const auto& m : snapshot.group_members) {
            if (m.main_assist) {
                resolved_name = m.name;
                return find_member_spawn(m);
            }
        }
    }
    if (key == "group_tank" || key == "main_tank" || key == "tank") {
        for (const auto& m : snapshot.group_members) {
            if (m.main_tank) {
                resolved_name = m.name;
                return find_member_spawn(m);
            }
        }
    }
    if (key == "group_leader" || key == "leader") {
        for (const auto& m : snapshot.group_members) {
            if (m.is_leader) {
                resolved_name = m.name;
                return find_member_spawn(m);
            }
        }
    }

    for (const auto& m : snapshot.group_members) {
        if (iequals(m.name, profile_.assist_target)) {
            resolved_name = m.name;
            return find_member_spawn(m);
        }
    }
    for (const auto& s : snapshot.spawns) {
        if (iequals(s.name, profile_.assist_target) && !s.is_corpse && !s.dead) {
            resolved_name = s.name;
            return &s;
        }
    }
    resolved_name = profile_.assist_target;
    return nullptr;
}

const SpawnSnapshot* CombatEngine::resolve_follow_spawn(const CombatSnapshot& snapshot) const {
    const std::string key = to_lower_copy(profile_.follow_target);
    if (key.empty()) {
        return nullptr;
    }
    if (key == "assist") {
        std::string tmp;
        return resolve_assist_spawn(snapshot, tmp);
    }
    if (key == "group_leader" || key == "leader") {
        for (const auto& m : snapshot.group_members) {
            if (m.is_leader && !m.offline && m.in_zone && m.spawn_id != 0) {
                return find_spawn_by_id(snapshot, m.spawn_id);
            }
        }
    }
    if (key == "main_tank" || key == "group_tank" || key == "tank") {
        for (const auto& m : snapshot.group_members) {
            if (m.main_tank && !m.offline && m.in_zone && m.spawn_id != 0) {
                return find_spawn_by_id(snapshot, m.spawn_id);
            }
        }
    }
    for (const auto& m : snapshot.group_members) {
        if (iequals(m.name, profile_.follow_target) && !m.offline && m.in_zone && m.spawn_id != 0) {
            return find_spawn_by_id(snapshot, m.spawn_id);
        }
    }
    for (const auto& s : snapshot.spawns) {
        if (iequals(s.name, profile_.follow_target) && !s.dead && !s.is_corpse) {
            return &s;
        }
    }
    return nullptr;
}

bool CombatEngine::is_valid_hostile(const CombatSnapshot& snapshot,
                                    const SpawnSnapshot& s,
                                    std::string& reason) const {
    if (s.id == 0) {
        reason = "Invalid spawn ID";
        return false;
    }
    if (s.id == snapshot.me.id) {
        reason = "Target is self";
        return false;
    }
    if (snapshot.has_pet && s.id == snapshot.pet_id) {
        reason = "Target is own pet";
        return false;
    }
    if (s.dead || s.is_corpse || s.hp_pct == 0) {
        reason = "Target is dead or corpse";
        return false;
    }
    if (!s.targetable) {
        reason = "Target is untargetable";
        return false;
    }
    if (!s.is_npc) {
        reason = "Target is not an NPC";
        return false;
    }
    if (s.is_pet || s.pet_owner_id != 0 || s.is_mercenary) {
        reason = "Target is a player pet/mercenary";
        return false;
    }
    const float dist = snapshot.me.position.distance_2d(s.position);
    if (dist > profile_.disengage_distance) {
        reason = "Target exceeded disengage distance";
        return false;
    }
    return true;
}

const SpawnSnapshot* CombatEngine::resolve_hostile_target(const CombatSnapshot& snapshot,
                                                          const SpawnSnapshot* assist_spawn,
                                                          std::string& reject_reason) const {
    reject_reason.clear();

    // If we already have an engaged target, validate it first.
    if (engaged_target_id_ != 0) {
        const SpawnSnapshot* current = find_spawn_by_id(snapshot, engaged_target_id_);
        if (!current) {
            reject_reason = "Engaged target disappeared";
            return nullptr;
        }
        std::string reason;
        if (!is_valid_hostile(snapshot, *current, reason)) {
            reject_reason = reason;
            return nullptr;
        }
        return current;
    }

    // Assist target takes priority if configured.
    if (!profile_.assist_target.empty()) {
        if (!assist_spawn || assist_spawn->id == 0) {
            reject_reason = "Assist target unavailable";
            return nullptr;
        }
        if (snapshot.has_assist_target && snapshot.assist_target_id != 0) {
            const SpawnSnapshot* assisted_mob = find_spawn_by_id(snapshot, snapshot.assist_target_id);
            if (!assisted_mob) {
                reject_reason = "Assisted target not in spawn table";
                return nullptr;
            }
            std::string reason;
            if (!is_valid_hostile(snapshot, *assisted_mob, reason)) {
                reject_reason = reason;
                return nullptr;
            }
            const float dist = snapshot.me.position.distance_2d(assisted_mob->position);
            if (dist > profile_.max_engage_distance) {
                reject_reason = "Assisted target out of engage distance";
                return nullptr;
            }
            if (assisted_mob->hp_pct > profile_.engage_at_hp_pct) {
                reject_reason = "Assisted target above engage HP threshold";
                return nullptr;
            }
            return assisted_mob;
        }
        reject_reason = "Waiting for assist target";
        return nullptr;
    }

    // Solo/explicit user target mode: require explicit allowed target or player's current valid NPC target.
    if (snapshot.has_target && snapshot.target.id != 0) {
        std::string reason;
        if (!is_valid_hostile(snapshot, snapshot.target, reason)) {
            reject_reason = reason;
            return nullptr;
        }
        if (!snapshot.allowed_to_attack_target && !snapshot.in_combat &&
            snapshot.pet_id == 0) {
            reject_reason = "Target not flagged hostile/engaged";
            return nullptr;
        }
        const float dist = snapshot.me.position.distance_2d(snapshot.target.position);
        if (dist > profile_.max_engage_distance) {
            reject_reason = "Target beyond max engage distance";
            return nullptr;
        }
        return &snapshot.target;
    }

    reject_reason = "No hostile target";
    return nullptr;
}

std::string CombatEngine::compute_idle_reason(const CombatSnapshot& snapshot,
                                              const SpawnSnapshot* assist_spawn,
                                              std::string_view target_reject_reason) const {
    if (!running_) {
        return "Controller stopped";
    }
    if (!profile_.enabled) {
        return "Profile disabled";
    }
    if (!snapshot.in_zone) {
        return "Waiting for character to enter zone";
    }
    if (!profile_.assist_target.empty() && !assist_spawn) {
        return "Assist target '" + profile_.assist_target + "' not in zone/range";
    }
    if (!target_reject_reason.empty()) {
        return std::string(target_reject_reason);
    }
    return "Waiting for target";
}

void CombatEngine::tick(const CombatSnapshot& snapshot, ICombatActions& actions) {
    last_tick_ms_ = snapshot.now_ms;
    if (!snapshot.character_name.empty()) {
        character_name_ = snapshot.character_name;
    }
    if (!snapshot.class_short.empty()) {
        class_short_ = snapshot.class_short;
    }
    if (!snapshot.server_name.empty()) {
        server_name_ = snapshot.server_name;
    }
    if (!snapshot.zone_short.empty()) {
        zone_short_ = snapshot.zone_short;
    }

    last_audit_ = audit_loadout(snapshot, profile_);
    check_pending_memorizations(snapshot, actions);
    last_audit_.failed_memorizations.assign(failed_memorizations_.begin(), failed_memorizations_.end());

    std::string assist_name;
    const SpawnSnapshot* assist_spawn = resolve_assist_spawn(snapshot, assist_name);
    assist_target_id_ = assist_spawn ? assist_spawn->id : 0;
    assist_target_name_ = assist_name;
    const SpawnSnapshot* follow_spawn = resolve_follow_spawn(snapshot);

    // 1. EmergencyStop stays latched until explicitly reset.
    if (state_ == CombatState::EmergencyStop) {
        stop_offensive_actions(&snapshot, actions, "Emergency stop active");
        movement_.force_stop("Emergency stop active", actions, profile_.dry_run, &snapshot);
        idle_reason_ = "Emergency stop latched";
        return;
    }

    // 2. Dead state overrides everything except EmergencyStop.
    if (snapshot.in_zone && snapshot.me.dead) {
        engaged_target_id_ = 0;
        engaged_target_name_.clear();
        engaged_target_hp_pct_ = 0;
        engaged_target_distance_ = 0.0f;
        stop_offensive_actions(&snapshot, actions, "Character died");
        movement_.force_stop("Character died", actions, profile_.dry_run, &snapshot);
        if (state_ != CombatState::Dead) {
            transition_to(CombatState::Dead, snapshot.now_ms, "Character died", &actions);
        }
        idle_reason_ = "Character is dead";
        return;
    }
    if (state_ == CombatState::Dead && (!snapshot.me.dead || !snapshot.in_zone)) {
        engaged_target_id_ = 0;
        transition_to(CombatState::Idle, snapshot.now_ms, "Character revived", &actions);
    }

    // 3. Complete a pending Disengage transition cleanly on the following tick.
    if (state_ == CombatState::Disengage) {
        stop_offensive_actions(&snapshot, actions, "Disengaging");
        movement_.force_stop("Disengaged", actions, profile_.dry_run, &snapshot);
        engaged_target_id_ = 0;
        engaged_target_name_.clear();
        engaged_target_hp_pct_ = 0;
        engaged_target_distance_ = 0.0f;
        transition_to(CombatState::Idle, snapshot.now_ms, "Disengage complete", &actions);
    }

    // 4. Not running or disabled: keep movement & attacks off!
    if (!running_ || !profile_.enabled || !snapshot.in_zone) {
        if (engaged_target_id_ != 0) {
            engaged_target_id_ = 0;
            engaged_target_name_.clear();
        }
        stop_offensive_actions(&snapshot, actions, running_ ? "Disabled/not zoned" : "Stopped");
        movement_.force_stop(running_ ? "Disabled/not zoned" : "Controller stopped",
                             actions, profile_.dry_run, &snapshot);
        if (state_ != CombatState::Idle && state_ != CombatState::Paused) {
            transition_to(CombatState::Idle, snapshot.now_ms,
                          !running_ ? "Controller stopped" : "Waiting for zone/enable",
                          &actions);
        }
        idle_reason_ = compute_idle_reason(snapshot, assist_spawn, "");
        return;
    }

    // 5. Explicit user pause holds in Paused until resume()/start().
    if (user_paused_) {
        stop_offensive_actions(&snapshot, actions, "User paused");
        movement_.force_stop("User paused", actions, profile_.dry_run, &snapshot);
        if (state_ != CombatState::Paused) {
            transition_to(CombatState::Paused, snapshot.now_ms, "Paused by user", &actions);
        }
        idle_reason_ = "Paused by user";
        return;
    }

    // 6. Manual movement / back-away override handling.
    if (profile_.pause_on_manual_move &&
        (snapshot.manual_move_backward || snapshot.player_moving_manually)) {
        movement_.update(CombatState::Paused, snapshot, profile_, nullptr, assist_spawn, follow_spawn,
                         actions, profile_.dry_run);
        if (snapshot.manual_move_backward || auto_attack_commanded_ || snapshot.auto_attack_on) {
            stop_offensive_actions(&snapshot, actions, "Manual player back-away/movement");
        }
        auto_manual_paused_ = true;
        if (state_ != CombatState::Paused) {
            transition_to(CombatState::Paused, snapshot.now_ms,
                          snapshot.manual_move_backward ? "Player backed away manually"
                                                        : "Manual player movement override",
                          &actions);
        }
        idle_reason_ = "Manual movement override";
        return;
    }

    if (auto_manual_paused_) {
        if (movement_.manual_override_active(snapshot.now_ms)) {
            idle_reason_ = "Manual movement override hold";
            return;
        }
        auto_manual_paused_ = false;
        transition_to(CombatState::Idle, snapshot.now_ms, "Manual movement override expired", &actions);
        idle_reason_ = "Manual movement override expired";
        return;
    }

    // 7. Resolve active hostile target & detect target loss / death / disengage.
    std::string target_reject_reason;
    const SpawnSnapshot* hostile = resolve_hostile_target(snapshot, assist_spawn, target_reject_reason);

    if (engaged_target_id_ != 0 && !hostile) {
        engaged_target_id_ = 0;
        engaged_target_name_.clear();
        engaged_target_hp_pct_ = 0;
        engaged_target_distance_ = 0.0f;
        stop_offensive_actions(&snapshot, actions, target_reject_reason);
        movement_.force_stop(target_reject_reason, actions, profile_.dry_run, &snapshot);
        transition_to(CombatState::Disengage, snapshot.now_ms,
                      target_reject_reason.empty() ? "Target lost" : target_reject_reason,
                      &actions);
        idle_reason_ = target_reject_reason;
        return;
    }

    if (hostile) {
        engaged_target_id_ = hostile->id;
        engaged_target_name_ = hostile->name;
        engaged_target_hp_pct_ = hostile->hp_pct;
        engaged_target_distance_ = snapshot.me.position.distance_2d(hostile->position);
    } else {
        engaged_target_id_ = 0;
        engaged_target_name_.clear();
        engaged_target_hp_pct_ = 0;
        engaged_target_distance_ = 0.0f;
    }

    // 8. Check Retreat condition (low HP while hostile/in combat).
    const bool under_threat = (hostile != nullptr) || snapshot.in_combat;
    if (profile_.retreat_hp_pct > 0 && under_threat &&
        snapshot.hp_pct <= profile_.retreat_hp_pct) {
        if (profile_.stop_cast_on_retreat && snapshot.casting) {
            if (!profile_.dry_run) {
                actions.stop_cast();
            } else {
                actions.log_message("safety", "[DRY-RUN] stop_cast on retreat");
            }
        }
        stop_offensive_actions(&snapshot, actions, "Retreating due to low HP");
        if (state_ != CombatState::Retreat) {
            transition_to(CombatState::Retreat, snapshot.now_ms,
                          "Low HP (" + std::to_string(snapshot.hp_pct) + "% <= " +
                              std::to_string(profile_.retreat_hp_pct) + "%)",
                          &actions);
        }
        movement_.update(CombatState::Retreat, snapshot, profile_, hostile, assist_spawn, follow_spawn,
                         actions, profile_.dry_run);
        idle_reason_.clear();
        return;
    }

    if (state_ == CombatState::Retreat) {
        const float dist = hostile ? snapshot.me.position.distance_2d(hostile->position) : profile_.disengage_distance;
        const bool recovered_hp = snapshot.hp_pct >= std::min(100, profile_.retreat_hp_pct + 12);
        const bool safe_dist = (!hostile) || (dist >= profile_.disengage_distance);
        if (recovered_hp || (safe_dist && !snapshot.in_combat)) {
            stop_offensive_actions(&snapshot, actions, "Retreat complete");
            movement_.force_stop("Retreat complete", actions, profile_.dry_run, &snapshot);
            transition_to(CombatState::Disengage, snapshot.now_ms, "Retreat threshold cleared", &actions);
            return;
        }
        movement_.update(CombatState::Retreat, snapshot, profile_, hostile, assist_spawn, follow_spawn,
                         actions, profile_.dry_run);
        return;
    }

    // 9. No active hostile -> Assist / OutOfCombat buffs + rememorize + optional Follow.
    if (!hostile) {
        stop_offensive_actions(&snapshot, actions, "No hostile target");

        const bool waiting_on_assist = (!profile_.assist_target.empty() && assist_spawn != nullptr);
        const CombatState target_ooc_state = waiting_on_assist ? CombatState::Assist : CombatState::Idle;
        if (state_ != target_ooc_state) {
            transition_to(target_ooc_state, snapshot.now_ms,
                          waiting_on_assist ? ("Assisting " + assist_name) : compute_idle_reason(snapshot, assist_spawn, target_reject_reason),
                          &actions);
        }

        // Handle rememorization only safely out of combat.
        const bool should_check_memo = rememorize_requested_ ||
                                       (profile_.rememorize_missing &&
                                        (last_audit_.any_missing || (profile_.auto_loadout && last_audit_.any_manual_conflict)) &&
                                        (last_rememorize_attempt_ms_ == 0 ||
                                         snapshot.now_ms - last_rememorize_attempt_ms_ >= 2000ULL));
        if (should_check_memo) {
            const RememorizePlan plan = plan_rememorize(snapshot, profile_, last_audit_, rememorize_requested_);
            const bool was_manual_req = rememorize_requested_;
            rememorize_requested_ = false;
            last_rememorize_attempt_ms_ = snapshot.now_ms;

            if (plan.allowed && !plan.slots_to_memorize.empty()) {
                if (snapshot.sitting && !profile_.dry_run) {
                    // ClientApp memorizes normally even while sitting or GameConnection memorize handles it; do not block.
                }
                if (plan.use_batch_set) {
                    std::vector<std::pair<int, std::uint32_t>> batch;
                    batch.reserve(plan.slots_to_memorize.size());
                    for (const auto& s : plan.slots_to_memorize) {
                        batch.emplace_back(s.gem_index, s.resolved_spell_id);
                    }
                    bool ok = true;
                    if (!profile_.dry_run) {
                        ok = actions.memorize_loadout(batch);
                    }
                    if (ok) {
                        if (!profile_.dry_run) {
                            for (const auto& s : plan.slots_to_memorize) {
                                pending_memo_checks_.push_back(
                                    {s.gem_index, s.resolved_spell_id, s.resolved_spell_name, snapshot.now_ms});
                            }
                        }
                        std::string detail = "Rememorizing loadout (" + std::to_string(batch.size()) + " gems)";
                        append_action(snapshot.now_ms, profile_.dry_run, "spells", detail, &actions);
                    } else {
                        std::string detail = "Batch loadout memorization failed (" +
                                             std::to_string(batch.size()) + " gems rejected)";
                        record_memo_failure(detail);
                        last_audit_.failed_memorizations.assign(failed_memorizations_.begin(),
                                                                failed_memorizations_.end());
                        append_action(snapshot.now_ms, profile_.dry_run, "spells", detail, &actions);
                    }
                } else {
                    const auto& s = plan.slots_to_memorize.front();
                    bool ok = true;
                    if (!profile_.dry_run) {
                        ok = actions.memorize_gem(s.book_slot, s.gem_index);
                    }
                    if (ok) {
                        if (!profile_.dry_run) {
                            pending_memo_checks_.push_back(
                                {s.gem_index, s.resolved_spell_id, s.resolved_spell_name, snapshot.now_ms});
                        }
                        std::string detail = "Rememorizing Gem " + std::to_string(s.gem_index + 1) +
                                             ": " + s.resolved_spell_name;
                        append_action(snapshot.now_ms, profile_.dry_run, "spells", detail, &actions);
                    } else {
                        std::string detail = "Gem " + std::to_string(s.gem_index + 1) + ": " +
                                             s.resolved_spell_name + " memorization rejected";
                        record_memo_failure(detail);
                        last_audit_.failed_memorizations.assign(failed_memorizations_.begin(),
                                                                failed_memorizations_.end());
                        append_action(snapshot.now_ms, profile_.dry_run, "spells",
                                      "Memorization failed: " + detail, &actions);
                    }
                }
            } else if (was_manual_req && !plan.blocked_reason.empty()) {
                append_action(snapshot.now_ms, profile_.dry_run, "spells",
                              "Rememorize skipped: " + plan.blocked_reason, &actions);
            }
        }

        // Out-of-combat movement (follow / assist range).
        movement_.update(target_ooc_state, snapshot, profile_, nullptr, assist_spawn, follow_spawn,
                         actions, profile_.dry_run);

        // Evaluate out-of-combat / always rules (self buffs, out-of-combat heals, etc.)
        if (!snapshot.in_combat && !snapshot.memorizing) {
            RuleSelection ooc = select_next_rule(snapshot, profile_, nullptr, false, false, last_rule_used_ms_);
            if (ooc.rule) {
                if (snapshot.sitting && !profile_.dry_run) {
                    actions.set_stand_state(true);
                }
                if (ooc.rule->action_kind == SpellActionKind::CastSpell && ooc.gem) {
                    if (ooc.target_spawn_id != 0 &&
                        (!snapshot.has_target || snapshot.target.id != ooc.target_spawn_id)) {
                        if (!profile_.dry_run) {
                            actions.set_target(ooc.target_spawn_id);
                        }
                    }
                    bool ok = true;
                    if (!profile_.dry_run) {
                        ok = actions.cast_gem(ooc.gem->gem_index);
                    }
                    if (ok) {
                        last_rule_used_ms_[ooc.rule->name] = snapshot.now_ms;
                        last_cast_ = ooc.resolved_spell_name;
                        append_action(snapshot.now_ms, profile_.dry_run, "cast", ooc.summary, &actions);
                    }
                }
            }
        }

        idle_reason_ = (target_ooc_state == CombatState::Idle)
                           ? compute_idle_reason(snapshot, assist_spawn, target_reject_reason)
                           : ("Assisting " + assist_name);
        return;
    }

    // 10. Hostile target is active -> Engage -> Combat.
    const float target_dist = snapshot.me.position.distance_2d(hostile->position);
    float desired_combat_range = 200.0f;
    switch (profile_.movement_mode) {
    case MovementMode::MeleeRange:
        desired_combat_range = std::max(3.0f, profile_.melee_range) + 4.0f;
        break;
    case MovementMode::RangedRange:
        desired_combat_range = std::max(20.0f, profile_.ranged_max_range);
        break;
    case MovementMode::Follow:
    case MovementMode::AssistRange:
    case MovementMode::DisengageDistance:
    case MovementMode::None:
        desired_combat_range = profile_.max_engage_distance;
        break;
    }

    const bool in_engage_range = (target_dist <= desired_combat_range);
    if (state_ != CombatState::Combat && state_ != CombatState::Engage) {
        transition_to(in_engage_range ? CombatState::Combat : CombatState::Engage,
                      snapshot.now_ms,
                      "Engaging " + hostile->name + " (dist=" + std::to_string(static_cast<int>(target_dist)) + ")",
                      &actions);
    } else if (state_ == CombatState::Engage && in_engage_range) {
        transition_to(CombatState::Combat, snapshot.now_ms,
                      "In combat range of " + hostile->name, &actions);
    } else if (state_ == CombatState::Combat && !in_engage_range && profile_.movement_mode != MovementMode::None) {
        transition_to(CombatState::Engage, snapshot.now_ms,
                      hostile->name + " moved out of immediate range; closing", &actions);
    }

    idle_reason_.clear();

    // Update movement toward/around target.
    movement_.update(state_, snapshot, profile_, hostile, assist_spawn, follow_spawn,
                     actions, profile_.dry_run);

    // Ensure target is targeted before offensive actions.
    if (!snapshot.has_target || snapshot.target.id != hostile->id) {
        if (!profile_.dry_run) {
            actions.set_target(hostile->id);
        } else {
            actions.log_message("combat", "[DRY-RUN] set_target(" + hostile->name + ")");
        }
    }

    // Stand up for combat if sitting.
    if (snapshot.sitting && !profile_.dry_run) {
        actions.set_stand_state(true);
    }

    // Melee auto-attack control.
    const bool want_melee = profile_.allow_melee_attack &&
                            (target_dist <= std::max(3.0f, profile_.melee_range) + 2.0f);
    if (want_melee && !auto_attack_commanded_) {
        if (!profile_.dry_run) {
            actions.set_auto_attack(true);
        }
        auto_attack_commanded_ = true;
        append_action(snapshot.now_ms, profile_.dry_run, "melee",
                      "Auto-attack ON -> " + hostile->name, &actions);
    } else if (!want_melee && (auto_attack_commanded_ || snapshot.auto_attack_on) &&
               // Leave auto-attack enabled while a melee character is
               // actively closing on its target.  RGMercs may enable it
               // before the target is inside the final melee distance; the
               // previous logic immediately disabled it, creating an
               // on/off loop that prevented reliable engagement.
               !(profile_.movement_mode == MovementMode::MeleeRange &&
                 snapshot.nav_stick_spawn == hostile->id)) {
        if (!profile_.dry_run) {
            actions.set_auto_attack(false);
        }
        auto_attack_commanded_ = false;
        append_action(snapshot.now_ms, profile_.dry_run, "melee",
                      "Auto-attack OFF (out of melee range)", &actions);
    }

    // Pet attack control (once per engaged target).
    if (profile_.allow_pet_attack && snapshot.has_pet && snapshot.pet_id != 0 &&
        pet_attack_target_id_ != hostile->id) {
        if (!profile_.dry_run) {
            actions.pet_attack(hostile->id);
        }
        pet_attack_target_id_ = hostile->id;
        append_action(snapshot.now_ms, profile_.dry_run, "pet",
                      "Pet attack -> " + hostile->name, &actions);
    }

    // Evaluate priority rules (healing/lifetaps/dots/nukes/abilities).
    RuleSelection sel = select_next_rule(snapshot, profile_, hostile, true, true, last_rule_used_ms_);
    if (!sel.rule) {
        return;
    }

    if (sel.rule->action_kind == SpellActionKind::CastSpell && sel.gem) {
        if (sel.target_spawn_id != 0 &&
            (!snapshot.has_target || snapshot.target.id != sel.target_spawn_id)) {
            if (!profile_.dry_run) {
                actions.set_target(sel.target_spawn_id);
            }
        }
        bool ok = true;
        if (!profile_.dry_run) {
            ok = actions.cast_gem(sel.gem->gem_index);
        }
        if (ok) {
            last_rule_used_ms_[sel.rule->name] = snapshot.now_ms;
            last_cast_ = sel.resolved_spell_name;
            append_action(snapshot.now_ms, profile_.dry_run, "cast", sel.summary, &actions);
        }
    } else if (sel.rule->action_kind == SpellActionKind::CombatAbility) {
        bool ok = true;
        if (!profile_.dry_run) {
            ok = actions.combat_ability(sel.resolved_spell_name);
        }
        if (ok) {
            last_rule_used_ms_[sel.rule->name] = snapshot.now_ms;
            last_cast_ = sel.resolved_spell_name;
            append_action(snapshot.now_ms, profile_.dry_run, "ability", sel.summary, &actions);
        }
    } else if (sel.rule->action_kind == SpellActionKind::PetAttack) {
        if (!profile_.dry_run) {
            actions.pet_attack(sel.target_spawn_id);
        }
        last_rule_used_ms_[sel.rule->name] = snapshot.now_ms;
        pet_attack_target_id_ = sel.target_spawn_id;
        append_action(snapshot.now_ms, profile_.dry_run, "pet", sel.summary, &actions);
    } else if (sel.rule->action_kind == SpellActionKind::MeleeAttack) {
        if (!auto_attack_commanded_) {
            if (!profile_.dry_run) {
                actions.set_auto_attack(true);
            }
            auto_attack_commanded_ = true;
            last_rule_used_ms_[sel.rule->name] = snapshot.now_ms;
            append_action(snapshot.now_ms, profile_.dry_run, "melee", sel.summary, &actions);
        }
    }
}

CombatStatusSnapshot CombatEngine::status_snapshot(const CombatSnapshot* latest_snapshot) const {
    CombatStatusSnapshot out;
    out.state = state_;
    out.movement_mode = profile_.movement_mode;
    out.running = running_;
    out.dry_run = profile_.dry_run;
    out.auto_loadout = profile_.auto_loadout;
    out.manual_override_active = movement_.manual_override_active(last_tick_ms_);
    out.manual_override_remaining_ms = movement_.manual_override_remaining_ms(last_tick_ms_);
    out.auto_attack_active = latest_snapshot ? latest_snapshot->auto_attack_on : auto_attack_commanded_;
    out.character_name = latest_snapshot && !latest_snapshot->character_name.empty()
                             ? latest_snapshot->character_name
                             : character_name_;
    out.class_short = latest_snapshot && !latest_snapshot->class_short.empty()
                          ? latest_snapshot->class_short
                          : (class_short_.empty() ? profile_.class_short : class_short_);
    out.server_name = latest_snapshot && !latest_snapshot->server_name.empty()
                          ? latest_snapshot->server_name
                          : server_name_;
    out.zone_short = latest_snapshot && !latest_snapshot->zone_short.empty()
                         ? latest_snapshot->zone_short
                         : zone_short_;
    out.profile_path = profile_.source_path;
    out.target_id = engaged_target_id_;
    out.target_name = engaged_target_name_;
    out.target_hp_pct = engaged_target_hp_pct_;
    out.target_distance = engaged_target_distance_;
    out.assist_target_id = assist_target_id_;
    out.assist_target_name = assist_target_name_;
    out.transition_reason = transition_reason_;
    out.idle_reason = idle_reason_;
    out.last_action = last_action_;
    out.last_cast = last_cast_;
    out.last_movement_decision = movement_.last_decision();
    out.loadout_audit = last_audit_;
    out.loadout_audit.failed_memorizations.assign(failed_memorizations_.begin(), failed_memorizations_.end());
    out.transitions.assign(transitions_.begin(), transitions_.end());
    out.actions.assign(actions_.begin(), actions_.end());
    return out;
}

} // namespace hailstone::combat
