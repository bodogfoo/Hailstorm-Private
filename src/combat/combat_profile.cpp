#include "combat/combat_profile.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <system_error>

#include "core/ascii_case.h"
#include "core/ini_file.h"

namespace fs = std::filesystem;

namespace hailstone::combat {

namespace {

std::string trim_copy(std::string_view sv) {
    std::size_t b = 0;
    while (b < sv.size() && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
    std::size_t e = sv.size();
    while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
    return std::string(sv.substr(b, e - b));
}

std::string upper_copy(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char c : sv) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

bool parse_bool_val(std::string_view sv, bool fallback) {
    std::string t = trim_copy(sv);
    if (t.empty()) return fallback;
    if (iequals(t, "1") || iequals(t, "true") || iequals(t, "yes") || iequals(t, "on")) return true;
    if (iequals(t, "0") || iequals(t, "false") || iequals(t, "no") || iequals(t, "off")) return false;
    return fallback;
}

int parse_int_val(std::string_view sv, int fallback) {
    std::string t = trim_copy(sv);
    if (t.empty()) return fallback;
    char* end = nullptr;
    long v = std::strtol(t.c_str(), &end, 10);
    if (end == t.c_str() || *end != '\0') return fallback;
    return static_cast<int>(v);
}

std::uint64_t parse_u64_val(std::string_view sv, std::uint64_t fallback) {
    std::string t = trim_copy(sv);
    if (t.empty()) return fallback;
    char* end = nullptr;
    unsigned long long v = std::strtoull(t.c_str(), &end, 10);
    if (end == t.c_str() || *end != '\0') return fallback;
    return static_cast<std::uint64_t>(v);
}

float parse_float_val(std::string_view sv, float fallback) {
    std::string t = trim_copy(sv);
    if (t.empty()) return fallback;
    char* end = nullptr;
    float v = std::strtof(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0') return fallback;
    return v;
}

std::string format_float(float v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

std::string safe_filename_stem(std::string_view name) {
    std::string out;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-') {
            out.push_back(c);
        } else if (c == ' ') {
            out.push_back('_');
        }
    }
    return out;
}

void set_gem(CombatProfile& p, int gem_index, std::string spell_name, bool required = true) {
    if (gem_index < 0 || gem_index >= 12) return;
    for (auto& g : p.loadout) {
        if (g.gem_index == gem_index) {
            g.spell_name = std::move(spell_name);
            g.required = required;
            return;
        }
    }
    p.loadout.push_back(DesiredGemEntry{gem_index, std::move(spell_name), required});
}

void remove_gem(CombatProfile& p, int gem_index) {
    p.loadout.erase(
        std::remove_if(p.loadout.begin(), p.loadout.end(),
                       [gem_index](const DesiredGemEntry& e) { return e.gem_index == gem_index; }),
        p.loadout.end());
}

void apply_ini_to_profile(const IniFile& ini, CombatProfile& p, bool override_mode = false) {
    auto get_str = [&](std::string_view sec, std::string_view key) -> std::optional<std::string> {
        if (is_combat_secret_key(key)) return std::nullopt;
        return ini.get(sec, key);
    };

    if (auto v = get_str("General", "profile_name"); v && !trim_copy(*v).empty()) p.profile_name = trim_copy(*v);
    if (auto v = get_str("General", "class"); v && !trim_copy(*v).empty()) p.class_short = upper_copy(trim_copy(*v));
    if (auto v = get_str("General", "role"); v) p.role = trim_copy(*v);
    if (auto v = get_str("General", "mode"); v && !trim_copy(*v).empty()) p.mode = trim_copy(*v);
    if (auto v = get_str("General", "enabled")) p.enabled = parse_bool_val(*v, p.enabled);
    if (auto v = get_str("General", "dry_run")) p.dry_run = parse_bool_val(*v, p.dry_run);
    if (auto v = get_str("General", "auto_loadout")) p.auto_loadout = parse_bool_val(*v, p.auto_loadout);
    if (auto v = get_str("General", "rememorize_missing")) p.rememorize_missing = parse_bool_val(*v, p.rememorize_missing);
    if (auto v = get_str("General", "allow_melee_attack")) p.allow_melee_attack = parse_bool_val(*v, p.allow_melee_attack);
    if (auto v = get_str("General", "allow_pet_attack")) p.allow_pet_attack = parse_bool_val(*v, p.allow_pet_attack);
    if (auto v = get_str("General", "use_abilities")) p.use_abilities = parse_bool_val(*v, p.use_abilities);

    if (auto v = get_str("Targeting", "assist_target")) {
        const std::string t = trim_copy(*v);
        if (iequals(t, "none")) {
            p.assist_target.clear();
        } else if (!override_mode || !t.empty()) {
            p.assist_target = t;
        }
    }
    if (auto v = get_str("Targeting", "engage_at_hp_pct")) p.engage_at_hp_pct = std::clamp(parse_int_val(*v, p.engage_at_hp_pct), 1, 100);
    if (auto v = get_str("Targeting", "max_engage_distance")) p.max_engage_distance = std::max(10.0f, parse_float_val(*v, p.max_engage_distance));
    if (auto v = get_str("Targeting", "follow_target")) {
        const std::string t = trim_copy(*v);
        if (iequals(t, "none")) {
            p.follow_target.clear();
        } else if (!override_mode || !t.empty()) {
            p.follow_target = t;
        }
    }
    if (auto v = get_str("Targeting", "follow_distance")) p.follow_distance = std::max(2.0f, parse_float_val(*v, p.follow_distance));

    if (auto v = get_str("Movement", "movement_mode")) p.movement_mode = parse_movement_mode(*v, p.movement_mode);
    if (auto v = get_str("Movement", "assist_range")) p.assist_range = std::max(5.0f, parse_float_val(*v, p.assist_range));
    if (auto v = get_str("Movement", "melee_range")) p.melee_range = std::max(3.0f, parse_float_val(*v, p.melee_range));
    if (auto v = get_str("Movement", "ranged_min_range")) p.ranged_min_range = std::max(0.0f, parse_float_val(*v, p.ranged_min_range));
    if (auto v = get_str("Movement", "ranged_max_range")) p.ranged_max_range = std::max(10.0f, parse_float_val(*v, p.ranged_max_range));
    if (auto v = get_str("Movement", "disengage_distance")) p.disengage_distance = std::max(10.0f, parse_float_val(*v, p.disengage_distance));
    if (auto v = get_str("Movement", "melee_behind")) p.melee_behind = parse_bool_val(*v, p.melee_behind);
    if (auto v = get_str("Movement", "pause_on_manual_move")) p.pause_on_manual_move = parse_bool_val(*v, p.pause_on_manual_move);
    if (auto v = get_str("Movement", "manual_override_cooldown_ms")) p.manual_override_cooldown_ms = parse_u64_val(*v, p.manual_override_cooldown_ms);

    if (auto v = get_str("Safety", "retreat_hp_pct")) p.retreat_hp_pct = std::clamp(parse_int_val(*v, p.retreat_hp_pct), 0, 95);
    if (auto v = get_str("Safety", "retreat_distance")) p.retreat_distance = std::max(5.0f, parse_float_val(*v, p.retreat_distance));
    if (auto v = get_str("Safety", "stop_cast_on_retreat")) p.stop_cast_on_retreat = parse_bool_val(*v, p.stop_cast_on_retreat);
    if (auto v = get_str("Safety", "stop_attack_on_target_loss")) p.stop_attack_on_target_loss = parse_bool_val(*v, p.stop_attack_on_target_loss);

    const auto gem_keys = ini.keys("Loadout");
    if (!gem_keys.empty()) {
        for (int g = 1; g <= 12; ++g) {
            const std::string key = "gem" + std::to_string(g);
            if (auto v = ini.get("Loadout", key)) {
                const std::string spell = trim_copy(*v);
                if (spell.empty()) {
                    remove_gem(p, g - 1);
                } else {
                    const bool req = parse_bool_val(ini.get_string("Loadout", key + "_required", "true"), true);
                    set_gem(p, g - 1, spell, req);
                }
            }
        }
    }

    // Parse [Rule.<name>] or [Rule1..N] sections if present.
    std::vector<CombatSpellRule> parsed_rules;
    const bool replace_rules = parse_bool_val(ini.get_string("General", "replace_rules", "false"), false);
    for (const std::string& sec : ini.sections()) {
        if (!istarts_with(sec, "Rule.") && !istarts_with(sec, "Rule")) continue;
        if (iequals(sec, "Rules")) continue;
        CombatSpellRule r;
        if (istarts_with(sec, "Rule.")) {
            r.name = sec.substr(5);
        } else {
            r.name = ini.get_string(sec, "name", sec);
        }
        r.spell_name = trim_copy(ini.get_string(sec, "spell", ""));
        if (r.spell_name.empty()) continue;
        r.preferred_gem = parse_int_val(ini.get_string(sec, "gem", "0"), 0);
        r.action_kind = parse_spell_action_kind(ini.get_string(sec, "action", "cast"), SpellActionKind::CastSpell);
        r.target = parse_target_kind(ini.get_string(sec, "target", "HostileTarget"), TargetKind::HostileTarget);
        r.combat_condition = parse_combat_condition(ini.get_string(sec, "condition", "InCombatOnly"),
                                                    CombatCondition::InCombatOnly);
        r.priority = parse_int_val(ini.get_string(sec, "priority", "50"), 50);
        r.min_mana_pct = std::clamp(parse_int_val(ini.get_string(sec, "min_mana_pct", "0"), 0), 0, 100);
        r.max_mana_pct = std::clamp(parse_int_val(ini.get_string(sec, "max_mana_pct", "100"), 100), 0, 100);
        r.target_min_hp_pct = std::clamp(parse_int_val(ini.get_string(sec, "target_min_hp_pct", "0"), 0), 0, 100);
        r.target_max_hp_pct = std::clamp(parse_int_val(ini.get_string(sec, "target_max_hp_pct", "100"), 100), 0, 100);
        r.self_min_hp_pct = std::clamp(parse_int_val(ini.get_string(sec, "self_min_hp_pct", "0"), 0), 0, 100);
        r.self_max_hp_pct = std::clamp(parse_int_val(ini.get_string(sec, "self_max_hp_pct", "100"), 100), 0, 100);
        r.min_range = parse_float_val(ini.get_string(sec, "min_range", "0"), 0.0f);
        r.max_range = parse_float_val(ini.get_string(sec, "max_range", "200"), 200.0f);
        r.cooldown_ms = parse_u64_val(ini.get_string(sec, "cooldown_ms", "0"), 0);
        r.require_buff_missing = parse_bool_val(ini.get_string(sec, "require_buff_missing", "false"), false);
        r.detrimental = parse_bool_val(ini.get_string(sec, "detrimental",
                                                      r.target == TargetKind::HostileTarget ? "true" : "false"),
                                       r.target == TargetKind::HostileTarget);
        r.enabled = parse_bool_val(ini.get_string(sec, "enabled", "true"), true);
        parsed_rules.push_back(std::move(r));
    }

    if (!parsed_rules.empty()) {
        if (replace_rules) {
            p.rules = std::move(parsed_rules);
        } else {
            for (const auto& nr : parsed_rules) {
                auto it = std::find_if(p.rules.begin(), p.rules.end(),
                                       [&](const CombatSpellRule& existing) { return iequals(existing.name, nr.name); });
                if (it != p.rules.end()) *it = nr;
                else p.rules.push_back(nr);
            }
        }
    }
}

CombatProfile make_nec_profile() {
    CombatProfile p;
    p.profile_name = "Necromancer Native Default";
    p.class_short = "NEC";
    p.role = "ranged_dps";
    p.mode = "solo";
    p.auto_loadout = false;
    p.rememorize_missing = true;
    p.allow_melee_attack = false;
    p.allow_pet_attack = true;
    p.use_abilities = false;

    p.movement_mode = MovementMode::RangedRange;
    p.assist_range = 90.0f;
    p.melee_range = 12.0f;
    p.ranged_min_range = 25.0f;
    p.ranged_max_range = 135.0f;
    p.disengage_distance = 185.0f;
    p.retreat_hp_pct = 20;
    p.retreat_distance = 55.0f;

    // Supports exact names or pipe-separated fallback spell lines for RoF2/EZ levels.
    set_gem(p, 0, "Splurt|Pyrocruor|Blood of Thule|Cessation of Cor|Boiling Blood|Heat Blood", true);
    set_gem(p, 1, "Scent of Terris|Scent of Darkness|Scent of Shadow|Scent of Dusk", true);
    set_gem(p, 2, "Dooming Darkness|Cascading Darkness|Engulfing Darkness|Clinging Darkness", true);
    set_gem(p, 3, "Siphon Life|Drain Soul|Drain Spirit|Lifetap|Lifespike|Lifedraw", true);
    set_gem(p, 4, "Horror|Funeral Pyre of Kelador|Venom of the Snake|Envenomed Bolt|Poison Bolt", true);
    set_gem(p, 5, "Renew Bones|Mend Bones|Convoke Shadow|Animate Dead", false);
    set_gem(p, 6, "Seduction of Saryrn|Arch Lich|Demi Lich|Lich|Call of Bones|Dark Pact", false);
    set_gem(p, 7, "Feign Death|Harmshield", false);

    {
        CombatSpellRule r;
        r.name = "EmergencyLifetap";
        r.spell_name = "Siphon Life|Drain Soul|Drain Spirit|Lifetap|Lifespike|Lifedraw";
        r.preferred_gem = 4;
        r.target = TargetKind::HostileTarget;
        r.combat_condition = CombatCondition::EngagedTargetOnly;
        r.priority = 98;
        r.self_max_hp_pct = 62;
        r.min_mana_pct = 5;
        r.max_range = 140.0f;
        r.cooldown_ms = 2500;
        r.detrimental = true;
        p.rules.push_back(std::move(r));
    }
    {
        CombatSpellRule r;
        r.name = "PetHeal";
        r.spell_name = "Renew Bones|Mend Bones";
        r.preferred_gem = 6;
        r.target = TargetKind::Pet;
        r.combat_condition = CombatCondition::Always;
        r.priority = 90;
        r.target_max_hp_pct = 65;
        r.min_mana_pct = 15;
        r.max_range = 100.0f;
        r.cooldown_ms = 6000;
        r.detrimental = false;
        p.rules.push_back(std::move(r));
    }
    {
        CombatSpellRule r;
        r.name = "SnareDarkness";
        r.spell_name = "Dooming Darkness|Cascading Darkness|Engulfing Darkness|Clinging Darkness";
        r.preferred_gem = 3;
        r.target = TargetKind::HostileTarget;
        r.combat_condition = CombatCondition::EngagedTargetOnly;
        r.priority = 85;
        r.target_min_hp_pct = 15;
        r.min_mana_pct = 12;
        r.max_range = 150.0f;
        r.cooldown_ms = 18000;
        r.require_buff_missing = true;
        r.detrimental = true;
        p.rules.push_back(std::move(r));
    }
    {
        CombatSpellRule r;
        r.name = "ScentDebuff";
        r.spell_name = "Scent of Terris|Scent of Darkness|Scent of Shadow|Scent of Dusk";
        r.preferred_gem = 2;
        r.target = TargetKind::HostileTarget;
        r.combat_condition = CombatCondition::EngagedTargetOnly;
        r.priority = 82;
        r.target_min_hp_pct = 35;
        r.min_mana_pct = 15;
        r.max_range = 150.0f;
        r.cooldown_ms = 24000;
        r.require_buff_missing = true;
        r.detrimental = true;
        p.rules.push_back(std::move(r));
    }
    {
        CombatSpellRule r;
        r.name = "PrimaryDot";
        r.spell_name = "Splurt|Pyrocruor|Blood of Thule|Cessation of Cor|Boiling Blood|Heat Blood";
        r.preferred_gem = 1;
        r.target = TargetKind::HostileTarget;
        r.combat_condition = CombatCondition::EngagedTargetOnly;
        r.priority = 76;
        r.target_min_hp_pct = 20;
        r.min_mana_pct = 15;
        r.max_range = 150.0f;
        r.cooldown_ms = 18000;
        r.require_buff_missing = true;
        r.detrimental = true;
        p.rules.push_back(std::move(r));
    }
    {
        CombatSpellRule r;
        r.name = "PoisonDot";
        r.spell_name = "Horror|Funeral Pyre of Kelador|Venom of the Snake|Envenomed Bolt|Poison Bolt";
        r.preferred_gem = 5;
        r.target = TargetKind::HostileTarget;
        r.combat_condition = CombatCondition::EngagedTargetOnly;
        r.priority = 70;
        r.target_min_hp_pct = 18;
        r.min_mana_pct = 20;
        r.max_range = 150.0f;
        r.cooldown_ms = 12000;
        r.require_buff_missing = true;
        r.detrimental = true;
        p.rules.push_back(std::move(r));
    }
    {
        CombatSpellRule r;
        r.name = "LifetapSustain";
        r.spell_name = "Siphon Life|Drain Soul|Drain Spirit|Lifetap|Lifespike|Lifedraw";
        r.preferred_gem = 4;
        r.target = TargetKind::HostileTarget;
        r.combat_condition = CombatCondition::EngagedTargetOnly;
        r.priority = 55;
        r.self_max_hp_pct = 88;
        r.min_mana_pct = 25;
        r.max_range = 140.0f;
        r.cooldown_ms = 4000;
        r.detrimental = true;
        p.rules.push_back(std::move(r));
    }
    {
        CombatSpellRule r;
        r.name = "LichBuff";
        r.spell_name = "Seduction of Saryrn|Arch Lich|Demi Lich|Lich|Call of Bones|Dark Pact";
        r.preferred_gem = 7;
        r.target = TargetKind::Self;
        r.combat_condition = CombatCondition::OutOfCombatOnly;
        r.priority = 40;
        r.self_min_hp_pct = 55;
        r.max_mana_pct = 95;
        r.cooldown_ms = 15000;
        r.require_buff_missing = true;
        r.detrimental = false;
        p.rules.push_back(std::move(r));
    }
    return p;
}

CombatProfile make_rog_profile() {
    CombatProfile p;
    p.profile_name = "Rogue Native Default";
    p.class_short = "ROG";
    p.role = "melee_dps";
    p.mode = "assist";
    p.auto_loadout = false;
    p.rememorize_missing = false;
    p.allow_melee_attack = true;
    p.allow_pet_attack = false;
    p.use_abilities = true;

    p.assist_target = "main_assist";
    p.movement_mode = MovementMode::MeleeRange;
    p.assist_range = 85.0f;
    p.melee_range = 12.0f;
    p.melee_behind = true;
    p.disengage_distance = 150.0f;
    p.retreat_hp_pct = 18;
    p.retreat_distance = 40.0f;

    {
        CombatSpellRule r;
        r.name = "Backstab";
        r.spell_name = "Backstab";
        r.action_kind = SpellActionKind::CombatAbility;
        r.target = TargetKind::HostileTarget;
        r.combat_condition = CombatCondition::EngagedTargetOnly;
        r.priority = 100;
        r.max_range = 14.0f;
        r.cooldown_ms = 4000;
        r.detrimental = true;
        p.rules.push_back(std::move(r));
    }
    {
        CombatSpellRule r;
        r.name = "AssassinStrike";
        r.spell_name = "Assassin's Strike|Assault|Pinpoint Vulnerability|Duelist";
        r.action_kind = SpellActionKind::CombatAbility;
        r.target = TargetKind::HostileTarget;
        r.combat_condition = CombatCondition::EngagedTargetOnly;
        r.priority = 80;
        r.target_min_hp_pct = 20;
        r.max_range = 15.0f;
        r.cooldown_ms = 12000;
        r.detrimental = true;
        p.rules.push_back(std::move(r));
    }
    {
        CombatSpellRule r;
        r.name = "DisarmTrap";
        r.spell_name = "Intimidation|Kick";
        r.action_kind = SpellActionKind::CombatAbility;
        r.target = TargetKind::HostileTarget;
        r.combat_condition = CombatCondition::EngagedTargetOnly;
        r.priority = 60;
        r.max_range = 14.0f;
        r.cooldown_ms = 8000;
        r.detrimental = true;
        p.rules.push_back(std::move(r));
    }
    return p;
}

CombatProfile make_clr_profile() {
    CombatProfile p;
    p.profile_name = "Cleric Native Default";
    p.class_short = "CLR";
    p.role = "healer";
    p.mode = "group";
    p.auto_loadout = false;
    p.rememorize_missing = true;
    p.allow_melee_attack = false;
    p.allow_pet_attack = false;
    p.use_abilities = false;

    p.assist_target = "main_assist";
    p.follow_target = "main_tank";
    p.follow_distance = 18.0f;
    p.movement_mode = MovementMode::RangedRange;
    p.assist_range = 85.0f;
    p.ranged_min_range = 15.0f;
    p.ranged_max_range = 100.0f;
    p.disengage_distance = 160.0f;
    p.retreat_hp_pct = 15;
    p.retreat_distance = 45.0f;

    set_gem(p, 0, "Complete Heal|Supernal Remedy|Holy Light|Greater Healing|Healing|Light Healing", true);
    set_gem(p, 1, "Remedy|Fast Healing|Superior Healing", true);
    set_gem(p, 2, "Word of Replenishment|Word of Vigor|Word of Healing", false);
    set_gem(p, 3, "Sound of Might|Force|Holy Might|Stun", false);
    set_gem(p, 4, "Virtue|Hand of Virtue|Aegolism|Heroism|Armor of Faith|Courage", false);
    set_gem(p, 5, "Yaulp V|Yaulp IV|Yaulp", false);

    {
        CombatSpellRule r;
        r.name = "EmergencyTankHeal";
        r.spell_name = "Complete Heal|Supernal Remedy|Holy Light|Greater Healing|Healing|Light Healing";
        r.preferred_gem = 1;
        r.target = TargetKind::GroupTank;
        r.combat_condition = CombatCondition::Always;
        r.priority = 100;
        r.target_max_hp_pct = 50;
        r.min_mana_pct = 5;
        r.max_range = 120.0f;
        r.cooldown_ms = 2000;
        r.detrimental = false;
        p.rules.push_back(std::move(r));
    }
    {
        CombatSpellRule r;
        r.name = "SelfHeal";
        r.spell_name = "Remedy|Fast Healing|Holy Light|Superior Healing|Greater Healing|Healing";
        r.preferred_gem = 2;
        r.target = TargetKind::Self;
        r.combat_condition = CombatCondition::Always;
        r.priority = 96;
        r.self_max_hp_pct = 65;
        r.min_mana_pct = 5;
        r.max_range = 100.0f;
        r.cooldown_ms = 2500;
        r.detrimental = false;
        p.rules.push_back(std::move(r));
    }
    {
        CombatSpellRule r;
        r.name = "GroupLowestHeal";
        r.spell_name = "Remedy|Fast Healing|Holy Light|Superior Healing|Greater Healing|Healing";
        r.preferred_gem = 2;
        r.target = TargetKind::GroupLowestHp;
        r.combat_condition = CombatCondition::Always;
        r.priority = 90;
        r.target_max_hp_pct = 75;
        r.min_mana_pct = 8;
        r.max_range = 120.0f;
        r.cooldown_ms = 2500;
        r.detrimental = false;
        p.rules.push_back(std::move(r));
    }
    {
        CombatSpellRule r;
        r.name = "StunNuke";
        r.spell_name = "Sound of Might|Force|Holy Might|Stun";
        r.preferred_gem = 4;
        r.target = TargetKind::HostileTarget;
        r.combat_condition = CombatCondition::EngagedTargetOnly;
        r.priority = 50;
        r.target_min_hp_pct = 15;
        r.min_mana_pct = 55;
        r.max_range = 100.0f;
        r.cooldown_ms = 12000;
        r.detrimental = true;
        p.rules.push_back(std::move(r));
    }
    {
        CombatSpellRule r;
        r.name = "SelfArmorBuff";
        r.spell_name = "Virtue|Hand of Virtue|Aegolism|Heroism|Armor of Faith|Courage";
        r.preferred_gem = 5;
        r.target = TargetKind::Self;
        r.combat_condition = CombatCondition::OutOfCombatOnly;
        r.priority = 35;
        r.min_mana_pct = 30;
        r.cooldown_ms = 15000;
        r.require_buff_missing = true;
        r.detrimental = false;
        p.rules.push_back(std::move(r));
    }
    return p;
}

CombatProfile make_war_profile() {
    CombatProfile p;
    p.profile_name = "Warrior Native Default";
    p.class_short = "WAR";
    p.role = "tank";
    p.mode = "tank";
    p.auto_loadout = false;
    p.rememorize_missing = false;
    p.allow_melee_attack = true;
    p.allow_pet_attack = false;
    p.use_abilities = true;

    p.movement_mode = MovementMode::MeleeRange;
    p.assist_range = 85.0f;
    p.melee_range = 12.0f;
    p.melee_behind = false;
    p.disengage_distance = 140.0f;
    p.retreat_hp_pct = 12;
    p.retreat_distance = 35.0f;

    {
        CombatSpellRule r;
        r.name = "Taunt";
        r.spell_name = "Taunt";
        r.action_kind = SpellActionKind::CombatAbility;
        r.target = TargetKind::HostileTarget;
        r.combat_condition = CombatCondition::EngagedTargetOnly;
        r.priority = 100;
        r.max_range = 14.0f;
        r.cooldown_ms = 6000;
        r.detrimental = true;
        p.rules.push_back(std::move(r));
    }
    {
        CombatSpellRule r;
        r.name = "KickOrBash";
        r.spell_name = "Bash|Kick";
        r.action_kind = SpellActionKind::CombatAbility;
        r.target = TargetKind::HostileTarget;
        r.combat_condition = CombatCondition::EngagedTargetOnly;
        r.priority = 80;
        r.max_range = 14.0f;
        r.cooldown_ms = 6000;
        r.detrimental = true;
        p.rules.push_back(std::move(r));
    }
    {
        CombatSpellRule r;
        r.name = "DefensiveDisc";
        r.spell_name = "Defensive|Evasive|Furious";
        r.action_kind = SpellActionKind::CombatAbility;
        r.target = TargetKind::Self;
        r.combat_condition = CombatCondition::InCombatOnly;
        r.priority = 95;
        r.self_max_hp_pct = 45;
        r.cooldown_ms = 30000;
        r.detrimental = false;
        p.rules.push_back(std::move(r));
    }
    return p;
}

} // namespace

bool is_combat_secret_key(std::string_view key) {
    return iequals(key, "password") || iequals(key, "pass") || iequals(key, "secret") ||
           iequals(key, "token") || iequals(key, "ticket") || iequals(key, "master_password") ||
           iequals(key, "session_key") || iequals(key, "auth_token") ||
           istarts_with(key, "password") || iends_with(key, "_password") ||
           iends_with(key, "_secret") || iends_with(key, "_token");
}

bool is_path_within_root(const fs::path& root, const fs::path& candidate) {
    if (root.empty() || candidate.empty()) return false;
    std::error_code ec;
    fs::path nr = fs::weakly_canonical(fs::absolute(root, ec), ec);
    if (ec) nr = root.lexically_normal();
    fs::path nc = candidate.is_absolute() ? candidate : (nr / candidate);
    nc = fs::weakly_canonical(nc, ec);
    if (ec) nc = (candidate.is_absolute() ? candidate : (root / candidate)).lexically_normal();
    if (nr == nc) return true;
    auto it_r = nr.begin();
    auto it_c = nc.begin();
    while (it_r != nr.end() && it_c != nc.end()) {
        if (*it_r != *it_c) return false;
        ++it_r;
        ++it_c;
    }
    return it_r == nr.end();
}

MovementMode parse_movement_mode(std::string_view text, MovementMode fallback) {
    const std::string t = trim_copy(text);
    if (iequals(t, "none") || iequals(t, "off")) return MovementMode::None;
    if (iequals(t, "follow")) return MovementMode::Follow;
    if (iequals(t, "assistrange") || iequals(t, "assist")) return MovementMode::AssistRange;
    if (iequals(t, "meleerange") || iequals(t, "melee")) return MovementMode::MeleeRange;
    if (iequals(t, "rangedrange") || iequals(t, "ranged") || iequals(t, "caster")) return MovementMode::RangedRange;
    if (iequals(t, "disengagedistance") || iequals(t, "disengage")) return MovementMode::DisengageDistance;
    return fallback;
}

TargetKind parse_target_kind(std::string_view text, TargetKind fallback) {
    const std::string t = trim_copy(text);
    if (iequals(t, "HostileTarget") || iequals(t, "target") || iequals(t, "hostile")) return TargetKind::HostileTarget;
    if (iequals(t, "Self") || iequals(t, "me")) return TargetKind::Self;
    if (iequals(t, "Pet")) return TargetKind::Pet;
    if (iequals(t, "GroupTank") || iequals(t, "tank") || iequals(t, "maintank")) return TargetKind::GroupTank;
    if (iequals(t, "GroupLowestHp") || iequals(t, "lowest") || iequals(t, "group")) return TargetKind::GroupLowestHp;
    if (iequals(t, "GroupMember")) return TargetKind::GroupMember;
    return fallback;
}

CombatCondition parse_combat_condition(std::string_view text, CombatCondition fallback) {
    const std::string t = trim_copy(text);
    if (iequals(t, "Always")) return CombatCondition::Always;
    if (iequals(t, "InCombatOnly") || iequals(t, "InCombat")) return CombatCondition::InCombatOnly;
    if (iequals(t, "OutOfCombatOnly") || iequals(t, "OutOfCombat")) return CombatCondition::OutOfCombatOnly;
    if (iequals(t, "EngagedTargetOnly") || iequals(t, "Engaged")) return CombatCondition::EngagedTargetOnly;
    return fallback;
}

SpellActionKind parse_spell_action_kind(std::string_view text, SpellActionKind fallback) {
    const std::string t = trim_copy(text);
    if (iequals(t, "CastSpell") || iequals(t, "cast") || iequals(t, "spell")) return SpellActionKind::CastSpell;
    if (iequals(t, "CombatAbility") || iequals(t, "ability") || iequals(t, "disc")) return SpellActionKind::CombatAbility;
    if (iequals(t, "PetAttack") || iequals(t, "pet")) return SpellActionKind::PetAttack;
    if (iequals(t, "MeleeAttack") || iequals(t, "melee")) return SpellActionKind::MeleeAttack;
    return fallback;
}

CombatProfile default_combat_profile_for_class(std::string_view class_short) {
    const std::string cls = upper_copy(trim_copy(class_short));
    if (cls == "NEC") return make_nec_profile();
    if (cls == "ROG") return make_rog_profile();
    if (cls == "CLR") return make_clr_profile();
    if (cls == "WAR") return make_war_profile();

    // Clean general fallback profiles for other EQ classes.
    if (cls == "SHD" || cls == "PAL" || cls == "MNK" || cls == "BER" || cls == "RNG" || cls == "BST" || cls == "BRD") {
        CombatProfile p = make_war_profile();
        p.class_short = cls.empty() ? "WAR" : cls;
        p.profile_name = p.class_short + " Native Default";
        p.role = (cls == "SHD" || cls == "PAL") ? "tank" : "melee_dps";
        p.assist_target = (p.role == "tank") ? "" : "main_assist";
        p.rules.clear();
        if (cls == "SHD" || cls == "PAL") {
            CombatSpellRule r;
            r.name = "Taunt";
            r.spell_name = "Taunt";
            r.action_kind = SpellActionKind::CombatAbility;
            r.target = TargetKind::HostileTarget;
            r.combat_condition = CombatCondition::EngagedTargetOnly;
            r.priority = 100;
            r.max_range = 14.0f;
            r.cooldown_ms = 6000;
            p.rules.push_back(std::move(r));
        } else {
            CombatSpellRule r;
            r.name = "Kick";
            r.spell_name = "Kick";
            r.action_kind = SpellActionKind::CombatAbility;
            r.target = TargetKind::HostileTarget;
            r.combat_condition = CombatCondition::EngagedTargetOnly;
            r.priority = 80;
            r.max_range = 14.0f;
            r.cooldown_ms = 6000;
            p.rules.push_back(std::move(r));
        }
        return p;
    }

    // Caster / Priest fallback (WIZ, MAG, ENC, SHM, DRU)
    CombatProfile p;
    p.class_short = cls.empty() ? "CAS" : cls;
    p.profile_name = p.class_short + " Native Default";
    p.role = (cls == "SHM" || cls == "DRU") ? "healer" : "ranged_dps";
    p.mode = "assist";
    p.assist_target = "main_assist";
    p.allow_melee_attack = false;
    p.allow_pet_attack = (cls == "MAG" || cls == "ENC" || cls == "SHM");
    p.movement_mode = MovementMode::RangedRange;
    p.assist_range = 85.0f;
    p.ranged_min_range = 20.0f;
    p.ranged_max_range = 130.0f;
    p.disengage_distance = 175.0f;
    p.retreat_hp_pct = 20;
    p.retreat_distance = 45.0f;
    return p;
}

bool load_combat_profile(const fs::path& path, CombatProfile& out, std::string& error) {
    IniFile ini;
    if (!ini.load(path)) {
        error = "Could not load combat profile: " + path.string();
        return false;
    }
    const std::string cls = trim_copy(ini.get_string("General", "class", ""));
    if (!cls.empty()) {
        out = default_combat_profile_for_class(cls);
    }
    apply_ini_to_profile(ini, out);
    out.source_path = path.string();
    return true;
}

bool apply_combat_profile_overrides(CombatProfile& base, const fs::path& override_path, std::string& error) {
    IniFile ini;
    if (!ini.load(override_path)) {
        error = "Could not load combat profile override: " + override_path.string();
        return false;
    }
    apply_ini_to_profile(ini, base, true);
    base.source_path = override_path.string();
    return true;
}

bool save_combat_profile(const CombatProfile& profile, const fs::path& path, std::string& error) {
    IniFile ini;
    ini.set("General", "profile_name", profile.profile_name);
    ini.set("General", "class", profile.class_short);
    ini.set("General", "role", profile.role);
    ini.set("General", "mode", profile.mode);
    ini.set("General", "enabled", profile.enabled ? "true" : "false");
    ini.set("General", "dry_run", profile.dry_run ? "true" : "false");
    ini.set("General", "auto_loadout", profile.auto_loadout ? "true" : "false");
    ini.set("General", "rememorize_missing", profile.rememorize_missing ? "true" : "false");
    ini.set("General", "allow_melee_attack", profile.allow_melee_attack ? "true" : "false");
    ini.set("General", "allow_pet_attack", profile.allow_pet_attack ? "true" : "false");
    ini.set("General", "use_abilities", profile.use_abilities ? "true" : "false");

    ini.set("Targeting", "assist_target", profile.assist_target);
    ini.set_int("Targeting", "engage_at_hp_pct", profile.engage_at_hp_pct);
    ini.set("Targeting", "max_engage_distance", format_float(profile.max_engage_distance));
    ini.set("Targeting", "follow_target", profile.follow_target);
    ini.set("Targeting", "follow_distance", format_float(profile.follow_distance));

    ini.set("Movement", "movement_mode", std::string(movement_mode_name(profile.movement_mode)));
    ini.set("Movement", "assist_range", format_float(profile.assist_range));
    ini.set("Movement", "melee_range", format_float(profile.melee_range));
    ini.set("Movement", "ranged_min_range", format_float(profile.ranged_min_range));
    ini.set("Movement", "ranged_max_range", format_float(profile.ranged_max_range));
    ini.set("Movement", "disengage_distance", format_float(profile.disengage_distance));
    ini.set("Movement", "melee_behind", profile.melee_behind ? "true" : "false");
    ini.set("Movement", "pause_on_manual_move", profile.pause_on_manual_move ? "true" : "false");
    ini.set("Movement", "manual_override_cooldown_ms", std::to_string(profile.manual_override_cooldown_ms));

    ini.set_int("Safety", "retreat_hp_pct", profile.retreat_hp_pct);
    ini.set("Safety", "retreat_distance", format_float(profile.retreat_distance));
    ini.set("Safety", "stop_cast_on_retreat", profile.stop_cast_on_retreat ? "true" : "false");
    ini.set("Safety", "stop_attack_on_target_loss", profile.stop_attack_on_target_loss ? "true" : "false");

    for (const auto& entry : profile.loadout) {
        if (entry.gem_index < 0 || entry.gem_index >= 12 || entry.spell_name.empty()) continue;
        const std::string key = "gem" + std::to_string(entry.gem_index + 1);
        ini.set("Loadout", key, entry.spell_name);
        ini.set("Loadout", key + "_required", entry.required ? "true" : "false");
    }

    for (std::size_t i = 0; i < profile.rules.size(); ++i) {
        const auto& r = profile.rules[i];
        const std::string rname = r.name.empty() ? ("Rule" + std::to_string(i + 1)) : r.name;
        const std::string sec = "Rule." + rname;
        ini.set(sec, "spell", r.spell_name);
        ini.set_int(sec, "gem", r.preferred_gem);
        ini.set(sec, "action", std::string(spell_action_kind_name(r.action_kind)));
        ini.set(sec, "target", std::string(target_kind_name(r.target)));
        ini.set(sec, "condition", std::string(combat_condition_name(r.combat_condition)));
        ini.set_int(sec, "priority", r.priority);
        ini.set_int(sec, "min_mana_pct", r.min_mana_pct);
        ini.set_int(sec, "max_mana_pct", r.max_mana_pct);
        ini.set_int(sec, "target_min_hp_pct", r.target_min_hp_pct);
        ini.set_int(sec, "target_max_hp_pct", r.target_max_hp_pct);
        ini.set_int(sec, "self_min_hp_pct", r.self_min_hp_pct);
        ini.set_int(sec, "self_max_hp_pct", r.self_max_hp_pct);
        ini.set("sec_unused_guard", "min_range", "0"); // noop removed right below
        ini.erase_section("sec_unused_guard");
        ini.set(sec, "min_range", format_float(r.min_range));
        ini.set(sec, "max_range", format_float(r.max_range));
        ini.set(sec, "cooldown_ms", std::to_string(r.cooldown_ms));
        ini.set(sec, "require_buff_missing", r.require_buff_missing ? "true" : "false");
        ini.set(sec, "detrimental", r.detrimental ? "true" : "false");
        ini.set(sec, "enabled", r.enabled ? "true" : "false");
    }

    if (!ini.save(path)) {
        error = "Failed to save combat profile: " + path.string();
        return false;
    }
    return true;
}

bool resolve_character_combat_profile(const fs::path& data_dir,
                                      const fs::path& character_dir,
                                      std::string_view group_id,
                                      std::string_view class_short,
                                      CombatProfile& out,
                                      std::string& error) {
    out = default_combat_profile_for_class(class_short);
    error.clear();

    if (!group_id.empty()) {
        const std::string safe_gid = safe_filename_stem(group_id);
        if (!safe_gid.empty()) {
            const fs::path grp_combat = data_dir / "profiles" / (safe_gid + ".combat.ini");
            std::error_code ec;
            if (is_path_within_root(data_dir, grp_combat) && fs::exists(grp_combat, ec)) {
                std::string grp_err;
                if (!apply_combat_profile_overrides(out, grp_combat, grp_err)) {
                    error = grp_err;
                    return false;
                }
                out.group_defaults_path = grp_combat.string();
            }
        }
    }

    if (!character_dir.empty()) {
        const fs::path resolved_char_dir =
            character_dir.is_absolute() ? character_dir : (data_dir / character_dir);
        if (!is_path_within_root(data_dir, resolved_char_dir)) {
            error = "Rejected combat profile directory outside Hailstone data root";
            return false;
        }
        const fs::path char_combat = resolved_char_dir / "combat_profile.ini";
        std::error_code ec;
        if (fs::exists(char_combat, ec)) {
            std::string char_err;
            if (!apply_combat_profile_overrides(out, char_combat, char_err)) {
                error = char_err;
                return false;
            }
        }
        out.source_path = char_combat.string();
    }
    return true;
}

bool migrate_imported_combat_settings(const fs::path& data_dir,
                                      const fs::path& legacy_ini_path,
                                      CombatProfile& in_out,
                                      int* imported_fields) {
    if (imported_fields) *imported_fields = 0;
    if (data_dir.empty() || legacy_ini_path.empty()) return false;
    const fs::path candidate = legacy_ini_path.is_absolute() ? legacy_ini_path : (data_dir / legacy_ini_path);
    if (!is_path_within_root(data_dir, candidate)) return false;

    IniFile ini;
    if (!ini.load(candidate)) return false;

    int count = 0;
    auto check_set_str = [&](std::string_view sec, std::string_view key, std::string& dst) {
        if (is_combat_secret_key(key)) return;
        if (auto v = ini.get(sec, key); v && !trim_copy(*v).empty()) {
            dst = trim_copy(*v);
            ++count;
        }
    };

    check_set_str("Combat", "assist_target", in_out.assist_target);
    check_set_str("Combat", "follow_target", in_out.follow_target);
    if (auto v = ini.get("Combat", "melee_range")) {
        in_out.melee_range = std::max(3.0f, parse_float_val(*v, in_out.melee_range));
        ++count;
    }
    if (auto v = ini.get("Combat", "ranged_max_range")) {
        in_out.ranged_max_range = std::max(10.0f, parse_float_val(*v, in_out.ranged_max_range));
        ++count;
    }
    if (auto v = ini.get("Combat", "retreat_hp_pct")) {
        in_out.retreat_hp_pct = std::clamp(parse_int_val(*v, in_out.retreat_hp_pct), 0, 95);
        ++count;
    }
    if (auto v = ini.get("Combat", "auto_loadout")) {
        in_out.auto_loadout = parse_bool_val(*v, in_out.auto_loadout);
        ++count;
    }
    for (int g = 1; g <= 12; ++g) {
        const std::string key = "gem" + std::to_string(g);
        if (auto v = ini.get("Combat", key); v && !trim_copy(*v).empty()) {
            set_gem(in_out, g - 1, trim_copy(*v), true);
            ++count;
        }
    }

    if (imported_fields) *imported_fields = count;
    return count > 0;
}

} // namespace hailstone::combat
