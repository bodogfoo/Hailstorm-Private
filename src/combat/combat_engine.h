#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

#include "combat/combat_types.h"
#include "combat/diagnostics.h"
#include "combat/movement_controller.h"

namespace hailstone::combat {

class CombatEngine {
public:
    static constexpr std::size_t kMaxLogEntries = 100;

    CombatEngine() = default;

    // Optional structured log. When set, every action the engine issues and every
    // tick it spends idle is recorded once (repeats fold into the record's count),
    // which is what the legacy add-on's per-tick spam was standing in for.
    void set_diagnostics(DiagnosticsSink* sink) noexcept { diagnostics_ = sink; }
    DiagnosticsSink* diagnostics() const noexcept { return diagnostics_; }

    void set_profile(CombatProfile profile);
    const CombatProfile& profile() const noexcept { return profile_; }
    CombatProfile& mutable_profile() noexcept { return profile_; }

    bool running() const noexcept { return running_; }
    CombatState state() const noexcept { return state_; }
    bool emergency_stopped() const noexcept { return state_ == CombatState::EmergencyStop; }

    void start(std::string_view reason = "Started by user");
    void stop(ICombatActions* actions, std::string_view reason = "Stopped by user");
    void pause(ICombatActions* actions, std::string_view reason = "Paused by user");
    void resume(std::string_view reason = "Resumed by user");
    void emergency_stop(ICombatActions& actions, std::string_view reason = "Emergency stop");
    bool reset_emergency_stop(std::string_view reason = "Emergency stop cleared");

    void set_dry_run(bool dry_run);
    void set_auto_loadout(bool auto_loadout);
    void set_movement_mode(MovementMode mode);
    void set_assist_target(std::string assist_target);
    void set_follow_target(std::string follow_target);

    void request_rememorize();
    void clear_logs();

    void on_zone_change(ICombatActions* actions, std::string_view new_zone);

    void tick(const CombatSnapshot& snapshot, ICombatActions& actions);

    CombatStatusSnapshot status_snapshot(const CombatSnapshot* latest_snapshot = nullptr) const;

private:
    void transition_to(CombatState next, std::uint64_t now_ms, std::string_view reason, ICombatActions* actions);
    void append_action(std::uint64_t now_ms, bool dry_run, std::string_view category, std::string detail, ICombatActions* actions);

    void stop_offensive_actions(const CombatSnapshot* snapshot, ICombatActions& actions, std::string_view reason);

    const SpawnSnapshot* find_spawn_by_id(const CombatSnapshot& snapshot, std::uint32_t id) const;
    const SpawnSnapshot* resolve_assist_spawn(const CombatSnapshot& snapshot, std::string& resolved_name) const;
    const SpawnSnapshot* resolve_follow_spawn(const CombatSnapshot& snapshot) const;
    const SpawnSnapshot* resolve_hostile_target(const CombatSnapshot& snapshot,
                                                const SpawnSnapshot* assist_spawn,
                                                std::string& reject_reason) const;

    bool is_valid_hostile(const CombatSnapshot& snapshot, const SpawnSnapshot& s, std::string& reason) const;
    std::string compute_idle_reason(const CombatSnapshot& snapshot,
                                    const SpawnSnapshot* assist_spawn,
                                    std::string_view target_reject_reason) const;

    CombatProfile profile_{};
    CombatState state_ = CombatState::Idle;
    bool running_ = false;
    bool user_paused_ = false;
    bool auto_manual_paused_ = false;
    bool rememorize_requested_ = false;
    bool auto_attack_commanded_ = false;
    std::uint32_t engaged_target_id_ = 0;
    std::uint32_t pet_attack_target_id_ = 0;
    std::uint32_t assist_target_id_ = 0;
    std::string assist_target_name_;
    std::string engaged_target_name_;
    std::uint8_t engaged_target_hp_pct_ = 0;
    float engaged_target_distance_ = 0.0f;
    std::uint64_t last_rememorize_attempt_ms_ = 0;
    std::uint64_t last_tick_ms_ = 0;

    std::string character_name_;
    std::string class_short_;
    std::string server_name_;
    std::string zone_short_;
    std::string transition_reason_ = "Initialized";
    std::string idle_reason_ = "Controller stopped";
    std::string last_action_;
    std::string last_cast_;
    struct PendingMemoCheck {
        int gem_index = 0;
        std::uint32_t spell_id = 0;
        std::string spell_name;
        std::uint64_t started_ms = 0;
    };

    void record_memo_failure(std::string msg);
    void check_pending_memorizations(const CombatSnapshot& snapshot, ICombatActions& actions);
    // Called at the end of every tick path: when the tick issued nothing, the
    // idle reason becomes a diagnostic so "why did nothing happen" is answerable.
    void report_idle_diagnostic(std::uint64_t action_serial_before, std::uint64_t now_ms);

    // Healing, resurrection, cures and pet healing, chosen by support_policy and
    // issued through the same action interface as a rule. Runs before the damage
    // rotation in both the out-of-combat and the combat path; returns true when
    // it cast something, so the tick stops there. A blocked support action
    // records why and lets the rotation have the tick: a healer with an empty
    // heal gem still fights.
    bool try_support_action(const CombatSnapshot& snapshot, ICombatActions& actions);

    LoadoutAudit last_audit_{};
    MovementController movement_{};
    std::unordered_map<std::string, std::uint64_t> last_rule_used_ms_;
    std::deque<TransitionLogEntry> transitions_;
    std::deque<ActionLogEntry> actions_;
    std::deque<std::string> failed_memorizations_;
    std::vector<PendingMemoCheck> pending_memo_checks_;
    std::uint64_t action_serial_ = 0;
    DiagnosticsSink* diagnostics_ = nullptr;
};

} // namespace hailstone::combat
