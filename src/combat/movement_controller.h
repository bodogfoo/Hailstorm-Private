#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "combat/combat_types.h"

namespace hailstone::combat {

enum class MovementCommandKind : std::uint8_t {
    None = 0,
    Stop,
    Follow,
    MoveToRange,
    MoveMelee,
    MoveDisengage,
};

struct MovementStepResult {
    MovementCommandKind command = MovementCommandKind::None;
    bool issued_to_actions = false;
    bool manual_override_active = false;
    std::uint64_t manual_override_remaining_ms = 0;
    std::string decision;
};

class MovementController {
public:
    void reset();

    void clear_manual_override();

    bool manual_override_active(std::uint64_t now_ms) const noexcept;
    std::uint64_t manual_override_remaining_ms(std::uint64_t now_ms) const noexcept;

    MovementStepResult force_stop(std::string_view reason,
                                  ICombatActions& actions,
                                  bool dry_run,
                                  const CombatSnapshot* snapshot = nullptr,
                                  bool unconditional = false);

    MovementStepResult update(CombatState state,
                              const CombatSnapshot& snapshot,
                              const CombatProfile& profile,
                              const SpawnSnapshot* hostile_target,
                              const SpawnSnapshot* assist_spawn,
                              const SpawnSnapshot* follow_spawn,
                              ICombatActions& actions,
                              bool dry_run);

    const std::string& last_decision() const noexcept { return last_decision_; }
    bool controller_moving() const noexcept { return active_command_ != MovementCommandKind::None && active_command_ != MovementCommandKind::Stop; }

private:
    MovementCommandKind active_command_ = MovementCommandKind::None;
    std::uint32_t active_target_id_ = 0;
    bool active_behind_ = false;
    float active_param_a_ = 0.0f;
    float active_param_b_ = 0.0f;
    std::uint64_t manual_override_until_ms_ = 0;
    bool manual_override_logged_ = false;
    std::string last_decision_ = "Stopped";
};

} // namespace hailstone::combat
