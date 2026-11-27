#include "combat/class_profiles.h"

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace hailstone::combat {

namespace {

std::string upper_copy(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char c : sv) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return out;
}

std::string trim_copy(std::string_view sv) {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!sv.empty() && is_space(sv.front())) sv.remove_prefix(1);
    while (!sv.empty() && is_space(sv.back())) sv.remove_suffix(1);
    return std::string(sv);
}

CombatSpellRule make_rule(std::string name,
                          std::string spell,
                          int priority,
                          TargetKind target,
                          SpellActionKind kind,
                          CombatCondition condition,
                          int min_mana_pct = 0,
                          int target_max_hp_pct = 100,
                          std::uint64_t cooldown_ms = 0,
                          bool require_buff_missing = false,
                          float max_range = 200.0f,
                          int self_max_hp_pct = 100,
                          int self_min_hp_pct = 0,
                          float min_range = 0.0f) {
    CombatSpellRule r;
    r.name = std::move(name);
    r.spell_name = std::move(spell);
    r.priority = priority;
    r.target = target;
    r.action_kind = kind;
    r.combat_condition = condition;
    r.min_mana_pct = min_mana_pct;
    r.target_max_hp_pct = target_max_hp_pct;
    r.cooldown_ms = cooldown_ms;
    r.require_buff_missing = require_buff_missing;
    r.max_range = max_range;
    r.self_max_hp_pct = self_max_hp_pct;
    r.self_min_hp_pct = self_min_hp_pct;
    r.min_range = min_range;
    r.detrimental = (target == TargetKind::HostileTarget);
    return r;
}

DesiredGemEntry gem(int zero_based_index, std::string spell, bool required = true) {
    DesiredGemEntry e;
    e.gem_index = zero_based_index;
    e.spell_name = std::move(spell);
    e.required = required;
    return e;
}

CombatSpellRule melee_ability(std::string name, std::string spell, int priority) {
    return make_rule(std::move(name), std::move(spell), priority, TargetKind::HostileTarget,
                     SpellActionKind::CombatAbility, CombatCondition::EngagedTargetOnly,
                     0, 100, 6000, false, 14.0f);
}

ClassProfileDef make_warrior() {
    ClassProfileDef d;
    d.class_short = "WAR";
    d.display_name = "Warrior";
    d.role = "tank";
    d.movement_mode = MovementMode::MeleeRange;
    d.allow_melee_attack = true;
    d.use_abilities = true;
    d.melee_range = 3.0f;
    d.engage_at_hp_pct = 99;
    d.retreat_hp_pct = 15;
    d.rules = {
        melee_ability("Taunt", "Taunt", 100),
        melee_ability("Bash", "Bash", 90),
        melee_ability("Kick", "Kick", 80),
    };
    return d;
}

ClassProfileDef make_rogue() {
    ClassProfileDef d;
    d.class_short = "ROG";
    d.display_name = "Rogue";
    d.role = "melee_dps";
    d.movement_mode = MovementMode::MeleeRange;
    d.allow_melee_attack = true;
    d.use_abilities = true;
    d.melee_range = 3.0f;
    d.engage_at_hp_pct = 95;
    d.retreat_hp_pct = 20;
    d.rules = {
        melee_ability("Backstab", "Backstab", 100),
        melee_ability("Kick", "Kick", 60),
        melee_ability("Evade", "Evade", 40),
    };
    return d;
}

ClassProfileDef make_cleric() {
    ClassProfileDef d;
    d.class_short = "CLR";
    d.display_name = "Cleric";
    d.role = "healer";
    d.movement_mode = MovementMode::AssistRange;
    d.allow_melee_attack = false;
    d.use_abilities = true;
    d.wants_rez = true;
    d.wants_buffs = true;
    d.assist_range = 95.0f;
    d.ranged_min_range = 15.0f;
    d.ranged_max_range = 150.0f;
    d.heal_at_hp_pct = 78;
    d.retreat_hp_pct = 25;
    d.loadout = {
        gem(0, "Superior Healing|Greater Healing|Healing|Light Healing"),
        gem(1, "Complete Heal"),
        gem(2, "Word of Restoration|Word of Healing"),
        gem(3, "Resuscitate|Revive|Renewal|Reanimation"),
        gem(4, "Symbol of Pinzarn|Symbol of Naltron|Symbol of Transal"),
        gem(5, "Cure Poison|Abolish Poison|Remedy"),
        gem(6, "Yaulp|Yaulp II|Yaulp III", false),
    };
    d.rules = {
        // Rez out of combat first: a dead group member is worth more than a heal.
        make_rule("Resurrect", "Resuscitate|Revive|Renewal|Reanimation", 120,
                  TargetKind::GroupMember, SpellActionKind::CastSpell, CombatCondition::OutOfCombatOnly,
                  10, 100, 3000, false, 100.0f),
        make_rule("HealCritical", "Complete Heal", 110, TargetKind::GroupLowestHp,
                  SpellActionKind::CastSpell, CombatCondition::InCombatOnly, 15, 55, 0, false, 100.0f),
        make_rule("Heal", "Superior Healing|Greater Healing|Healing|Light Healing", 100,
                  TargetKind::GroupLowestHp, SpellActionKind::CastSpell,
                  CombatCondition::InCombatOnly, 5, 78, 0, false, 100.0f),
        make_rule("GroupHeal", "Word of Restoration|Word of Healing", 95, TargetKind::GroupLowestHp,
                  SpellActionKind::CastSpell, CombatCondition::InCombatOnly, 20, 60, 4000, false, 100.0f),
        make_rule("Cure", "Cure Poison|Abolish Poison|Cure Disease|Remedy", 105,
                  TargetKind::GroupMember, SpellActionKind::CastSpell, CombatCondition::Always,
                  5, 100, 0, false, 100.0f),
        make_rule("Symbol", "Symbol of Pinzarn|Symbol of Naltron|Symbol of Transal", 30,
                  TargetKind::Self, SpellActionKind::CastSpell, CombatCondition::OutOfCombatOnly,
                  5, 100, 0, true, 100.0f),
        make_rule("Yaulp", "Yaulp|Yaulp II|Yaulp III", 25, TargetKind::Self,
                  SpellActionKind::CastSpell, CombatCondition::InCombatOnly, 0, 100, 24000, true, 100.0f),
    };
    d.core_buffs = {"Symbol of Pinzarn", "Yaulp"};
    return d;
}

ClassProfileDef make_magician() {
    ClassProfileDef d;
    d.class_short = "MAG";
    d.display_name = "Magician";
    d.role = "ranged_dps";
    d.movement_mode = MovementMode::RangedRange;
    d.allow_melee_attack = false;
    d.allow_pet_attack = true;
    d.use_abilities = true;
    d.wants_buffs = true;
    d.pet_recover_hp_pct = 40;
    d.ranged_min_range = 20.0f;
    d.ranged_max_range = 135.0f;
    d.loadout = {
        gem(0, "Child of Fire|Burst of Flame|Bolt of Flame|Shock of Flame"),
        gem(1, "Bolt of Flame|Shock of Flame|Flame Bolt"),
        gem(2, "Burnout|Burnout II|Burnout III"),
        gem(3, "Shield of Fire|Flame Shield|Shield of Lava"),
        gem(4, "Renew Elements|Renew Summoning|Summon Heatstone", false),
        gem(5, "Summon: Fireblade|Summon: Blazing Stalker", false),
    };
    d.rules = {
        make_rule("PetAttack", "Pet Attack", 96, TargetKind::HostileTarget,
                  SpellActionKind::PetAttack, CombatCondition::EngagedTargetOnly, 0, 100, 0, false, 200.0f),
        make_rule("PetHeal", "Renew Elements|Renew Summoning", 88, TargetKind::Pet,
                  SpellActionKind::CastSpell, CombatCondition::Always, 10, 40, 2000, false, 100.0f),
        // Child of Fire is this class's signature nuke; the alternatives keep the
        // magician useful at levels where it is not yet scribed.
        make_rule("Nuke", "Child of Fire|Burst of Flame|Bolt of Flame|Shock of Flame", 90,
                  TargetKind::HostileTarget, SpellActionKind::CastSpell,
                  CombatCondition::EngagedTargetOnly, 12, 100, 0, false, 200.0f),
        make_rule("PetHaste", "Burnout|Burnout II|Burnout III", 70, TargetKind::Pet,
                  SpellActionKind::CastSpell, CombatCondition::Always, 10, 100, 0, true, 100.0f),
        make_rule("DamageShield", "Shield of Fire|Flame Shield|Shield of Lava", 40, TargetKind::Self,
                  SpellActionKind::CastSpell, CombatCondition::Always, 10, 100, 0, true, 100.0f),
    };
    d.required_spells = {"Child of Fire"};
    d.core_buffs = {"Burnout", "Shield of Fire"};
    return d;
}

ClassProfileDef make_necromancer() {
    ClassProfileDef d;
    d.class_short = "NEC";
    d.display_name = "Necromancer";
    d.role = "ranged_dps";
    d.movement_mode = MovementMode::RangedRange;
    d.allow_melee_attack = false;
    d.allow_pet_attack = true;
    d.use_abilities = true;
    d.wants_buffs = true;
    d.pet_recover_hp_pct = 35;
    d.ranged_min_range = 20.0f;
    d.ranged_max_range = 130.0f;
    d.loadout = {
        gem(0, "Venom of the Snake|Envenomed Bolt|Poison Bolt"),
        gem(1, "Lifetap|Lifedraw|Siphon Life|Spirit Tap"),
        gem(2, "Lich|Dark Pact|Arch Lich"),
        gem(3, "Renew Bones|Shadow Compact", false),
        gem(4, "Dark Salve|Dark Empathy", false),
    };
    d.rules = {
        make_rule("PetAttack", "Pet Attack", 96, TargetKind::HostileTarget,
                  SpellActionKind::PetAttack, CombatCondition::EngagedTargetOnly, 0, 100, 0, false, 200.0f),
        make_rule("PetHeal", "Renew Bones|Shadow Compact", 86, TargetKind::Pet,
                  SpellActionKind::CastSpell, CombatCondition::Always, 10, 45, 2000, false, 100.0f),
        make_rule("Lifetap", "Lifetap|Lifedraw|Siphon Life|Spirit Tap", 84, TargetKind::HostileTarget,
                  SpellActionKind::CastSpell, CombatCondition::EngagedTargetOnly, 8, 100, 3000, false, 200.0f),
        make_rule("Dot", "Venom of the Snake|Envenomed Bolt|Poison Bolt", 82, TargetKind::HostileTarget,
                  SpellActionKind::CastSpell, CombatCondition::EngagedTargetOnly, 12, 100, 0, false, 200.0f),
        make_rule("Lich", "Lich|Dark Pact|Arch Lich", 60, TargetKind::Self,
                  SpellActionKind::CastSpell, CombatCondition::Always, 10, 100, 0, true, 100.0f),
    };
    d.core_buffs = {"Lich"};
    return d;
}

ClassProfileDef make_enchanter() {
    ClassProfileDef d;
    d.class_short = "ENC";
    d.display_name = "Enchanter";
    d.role = "crowd_control";
    d.movement_mode = MovementMode::RangedRange;
    d.allow_melee_attack = false;
    d.allow_pet_attack = false;
    d.use_abilities = true;
    d.wants_buffs = true;
    d.wants_crowd_control = true;
    d.ranged_min_range = 20.0f;
    d.ranged_max_range = 130.0f;
    d.loadout = {
        gem(0, "Chaotic Feedback|Sanity Warp"),
        gem(1, "Tashani|Tashanian|Tashina"),
        gem(2, "Alacrity|Celerity|Swift Like the Wind"),
        gem(3, "Clarity|Gift of Magic|Gift of Brilliance"),
        gem(4, "Mesmerize|Enthrall|Entrance", false),
    };
    d.rules = {
        make_rule("Haste", "Alacrity|Celerity|Swift Like the Wind", 92, TargetKind::GroupTank,
                  SpellActionKind::CastSpell, CombatCondition::Always, 10, 100, 0, true, 100.0f),
        make_rule("Clarity", "Clarity|Gift of Magic|Gift of Brilliance", 88, TargetKind::GroupTank,
                  SpellActionKind::CastSpell, CombatCondition::Always, 10, 100, 0, true, 100.0f),
        make_rule("Tash", "Tashani|Tashanian|Tashina", 86, TargetKind::HostileTarget,
                  SpellActionKind::CastSpell, CombatCondition::EngagedTargetOnly, 8, 100, 0, false, 200.0f),
        make_rule("Nuke", "Chaotic Feedback|Sanity Warp", 70, TargetKind::HostileTarget,
                  SpellActionKind::CastSpell, CombatCondition::EngagedTargetOnly, 12, 100, 0, false, 200.0f),
        // Mez needs an add to aim at; the combat snapshot only carries the
        // engaged target, so this rule ships disabled until a secondary-hostile
        // input exists (docs/unknowns.md).
        make_rule("Mez", "Mesmerize|Enthrall|Entrance", 50, TargetKind::HostileTarget,
                  SpellActionKind::CastSpell, CombatCondition::EngagedTargetOnly, 10, 100, 0, false, 200.0f),
    };
    d.rules.back().enabled = false;
    d.core_buffs = {"Clarity", "Alacrity"};
    return d;
}

const std::vector<ClassProfileDef>& class_table() {
    static const std::vector<ClassProfileDef> kTable = {
        make_warrior(), make_rogue(), make_cleric(), make_magician(), make_necromancer(), make_enchanter(),
    };
    return kTable;
}

} // namespace

const ClassProfileDef* find_class_profile(std::string_view class_short) {
    // Players and settings files spell the class with padding: " Mag " is the
    // magician as much as "MAG" is.
    const std::string cls = upper_copy(trim_copy(class_short));
    if (cls.empty()) return nullptr;
    for (const auto& def : class_table()) {
        if (def.class_short == cls) return &def;
    }
    return nullptr;
}

std::vector<std::string_view> native_class_shorts() {
    std::vector<std::string_view> out;
    for (const auto& def : class_table()) out.push_back(def.class_short);
    return out;
}

bool build_native_combat_profile(std::string_view class_short, CombatProfile& out, std::string& error) {
    const ClassProfileDef* def = find_class_profile(class_short);
    if (def == nullptr) {
        error = "No native combat profile for class '" + std::string(class_short) + "'";
        return false;
    }
    error.clear();

    CombatProfile p;
    p.profile_name = std::string(def->display_name) + " Native Default";
    p.class_short = std::string(def->class_short);
    p.role = std::string(def->role);
    p.mode = "assist";
    p.enabled = true;
    p.dry_run = false;
    p.auto_loadout = false;
    p.rememorize_missing = true;
    p.allow_melee_attack = def->allow_melee_attack;
    p.allow_pet_attack = def->allow_pet_attack;
    p.use_abilities = def->use_abilities;

    p.assist_target = "main_assist";
    p.engage_at_hp_pct = def->engage_at_hp_pct;
    p.max_engage_distance = def->max_engage_distance;
    p.follow_target = "group_leader";
    p.follow_distance = def->follow_distance;

    p.movement_mode = def->movement_mode;
    p.assist_range = def->assist_range;
    p.melee_range = def->melee_range;
    p.ranged_min_range = def->ranged_min_range;
    p.ranged_max_range = def->ranged_max_range;
    p.disengage_distance = def->disengage_distance;
    p.melee_behind = (def->class_short == "ROG");
    p.pause_on_manual_move = true;
    p.manual_override_cooldown_ms = 3500;

    p.retreat_hp_pct = def->retreat_hp_pct;
    p.retreat_distance = def->retreat_distance;
    p.stop_cast_on_retreat = true;
    p.stop_attack_on_target_loss = true;

    p.loadout = def->loadout;
    p.rules = def->rules;
    out = std::move(p);
    return true;
}

} // namespace hailstone::combat
