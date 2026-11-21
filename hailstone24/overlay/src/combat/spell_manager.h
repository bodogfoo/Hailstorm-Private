#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "combat/combat_types.h"

namespace hailstone::combat {

std::vector<std::string> split_pipe_alternatives(std::string_view spec);
bool spell_name_matches(std::string_view candidate_name, std::string_view spec);

const KnownSpellInfo* resolve_known_spell(const CombatSnapshot& snapshot, std::string_view spec);
const SpellGemInfo* resolve_memorized_gem(const CombatSnapshot& snapshot,
                                          std::string_view spec,
                                          int preferred_gem_1based = 0);

LoadoutAudit audit_loadout(const CombatSnapshot& snapshot, const CombatProfile& profile);

RememorizePlan plan_rememorize(const CombatSnapshot& snapshot,
                               const CombatProfile& profile,
                               const LoadoutAudit& audit,
                               bool user_requested = false);

struct RuleSelection {
    const CombatSpellRule* rule = nullptr;
    const SpellGemInfo* gem = nullptr;
    std::uint32_t target_spawn_id = 0;
    std::string target_name;
    std::string resolved_spell_name;
    std::string summary;
};

RuleSelection select_next_rule(const CombatSnapshot& snapshot,
                               const CombatProfile& profile,
                               const SpawnSnapshot* hostile_target,
                               bool in_combat_state,
                               bool has_engaged_target,
                               const std::unordered_map<std::string, std::uint64_t>& last_used_ms);

} // namespace hailstone::combat
