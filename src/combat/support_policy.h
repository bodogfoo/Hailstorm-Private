#pragma once

// Decisions the support classes make outside their damage rotation: heal, rez,
// pet recovery, buff maintenance and cures. Pure functions over a snapshot, so a
// six-box group's behaviour can be tested without a server.
//
// The caller (CombatEngine or the Lua bridge) owns casting and cooldowns; this
// module only answers "what is the most useful support action right now, and at
// whom". docs/native-combat.md "Healing, pets and buffs".

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "combat/combat_types.h"

namespace hailstone::combat {

enum class SupportActionKind : std::uint8_t {
    None = 0,
    Resurrect,
    Heal,
    CriticalHeal,
    GroupHeal,
    Cure,
    PetHeal,
    PetAttack,
    Buff,
};

inline constexpr std::string_view support_action_kind_name(SupportActionKind k) noexcept {
    switch (k) {
    case SupportActionKind::None: return "None";
    case SupportActionKind::Resurrect: return "Resurrect";
    case SupportActionKind::Heal: return "Heal";
    case SupportActionKind::CriticalHeal: return "CriticalHeal";
    case SupportActionKind::GroupHeal: return "GroupHeal";
    case SupportActionKind::Cure: return "Cure";
    case SupportActionKind::PetHeal: return "PetHeal";
    case SupportActionKind::PetAttack: return "PetAttack";
    case SupportActionKind::Buff: return "Buff";
    }
    return "None";
}

struct SupportSettings {
    int heal_at_hp_pct = 78;
    int critical_hp_pct = 55;
    bool allow_self_heal = true;
    bool allow_group_heal = true;
    bool allow_rez = true;
    bool allow_pet_heal = true;
    int pet_heal_at_hp_pct = 40;
    int rez_min_mana_pct = 5;
    // Number of hurt members at which a group heal beats a single-target one.
    int group_heal_min_members = 2;

    std::string heal_spell;
    std::string critical_heal_spell;
    std::string group_heal_spell;
    std::string rez_spell;
    std::string pet_heal_spell;
    std::string cure_spell;
};

struct SupportDecision {
    SupportActionKind kind = SupportActionKind::None;
    std::string spell_spec;
    TargetKind target = TargetKind::GroupLowestHp;
    std::string target_name;
    std::uint32_t target_spawn_id = 0;
    int target_hp_pct = 100;
    // Higher wins when the caller can only do one thing this tick.
    int priority = 0;
    std::string detail;
};

// Builds settings from a class profile's spells and thresholds so the class
// table stays the single source of truth for what a class heals with.
SupportSettings support_settings_from_profile(const CombatProfile& profile,
                                              int heal_at_hp_pct,
                                              int pet_heal_at_hp_pct);

// Thresholds come from the profile's own support rules (the "Heal" rule's
// target_max_hp_pct is what a class table entry sets as heal_at_hp_pct), so a
// saved profile and a class table both drive healing from one place.
SupportSettings support_settings_for_profile(const CombatProfile& profile);

// Lowest-HP living, in-zone, non-offline group member below the heal threshold.
std::optional<SupportDecision> choose_heal(const CombatSnapshot& snapshot, const SupportSettings& settings);

// A dead, in-zone group member that a resurrection can be aimed at.
std::optional<SupportDecision> choose_rez(const CombatSnapshot& snapshot, const SupportSettings& settings);

std::optional<SupportDecision> choose_pet_heal(const CombatSnapshot& snapshot, const SupportSettings& settings);

// First buff spec not already on the target (self or the group's tank).
std::optional<SupportDecision> choose_buff(const CombatSnapshot& snapshot,
                                           const std::vector<std::string>& buff_specs);

// Priority order: rez, critical heal, group heal, heal, cure, pet heal.
std::optional<SupportDecision> choose_support_action(const CombatSnapshot& snapshot,
                                                     const SupportSettings& settings,
                                                     const std::vector<std::string>& buff_specs);

} // namespace hailstone::combat
