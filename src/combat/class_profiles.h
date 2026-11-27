#pragma once

// Data-driven class behaviour for the native engine.
//
// Before this table the native engine only had hand-written defaults for NEC,
// ROG, CLR and WAR, and every other class fell through to an empty caster
// profile with no rules, so an EZ group's MAG/ENC did nothing at all. Each
// supported class is now one entry here: role, movement, pet, engagement
// thresholds and an ordered spell/ability priority list. Adding a class is a
// table entry, not a new code path.
//
// The spell names are the RoF2-era names a player's own spells_us.txt carries;
// the client reads that file from the game folder and reports a name it cannot
// find instead of inventing one (docs/native-combat.md "Class profiles").
//
// The priorities below are Hailstone's own defaults, written from the class
// roles the game gives them. They are not a copy of any other add-on's tables:
// RGMercs is GPL-3.0-or-later and reference only (see THIRD_PARTY.md).

#include <string>
#include <string_view>
#include <vector>

#include "combat/combat_types.h"

namespace hailstone::combat {

struct ClassProfileDef {
    std::string_view class_short;
    std::string_view display_name;
    std::string_view role;

    MovementMode movement_mode = MovementMode::AssistRange;
    bool allow_melee_attack = false;
    bool allow_pet_attack = false;
    bool use_abilities = true;
    bool wants_rez = false;
    bool wants_buffs = false;
    bool wants_crowd_control = false;
    bool burn = false;

    float melee_range = 3.0f;
    float assist_range = 90.0f;
    float ranged_min_range = 20.0f;
    float ranged_max_range = 130.0f;
    float follow_distance = 15.0f;
    float disengage_distance = 175.0f;
    float retreat_distance = 45.0f;
    float max_engage_distance = 180.0f;

    int engage_at_hp_pct = 99;
    int heal_at_hp_pct = 75;
    int retreat_hp_pct = 20;
    int pet_recover_hp_pct = 35;

    std::vector<DesiredGemEntry> loadout;
    std::vector<CombatSpellRule> rules;
    // Spells the class cannot do its job without. A snapshot missing one gets a
    // single structured diagnostic, not a repeated failure every tick.
    std::vector<std::string> required_spells;
    std::vector<std::string> core_buffs;
};

// nullptr when the class is not one of the native profiles.
const ClassProfileDef* find_class_profile(std::string_view class_short);

// Short names of the classes that have a native profile, in table order.
std::vector<std::string_view> native_class_shorts();

// Builds the CombatProfile the native engine uses for a class. Returns false and
// sets error for an unsupported class, leaving `out` untouched.
bool build_native_combat_profile(std::string_view class_short, CombatProfile& out, std::string& error);

} // namespace hailstone::combat
