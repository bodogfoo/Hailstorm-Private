#include "combat/support_policy.h"

#include <algorithm>
#include <string>

#include "combat/spell_manager.h"

namespace hailstone::combat {

namespace {

const CombatSpellRule* find_rule(const CombatProfile& profile, std::string_view name) {
    for (const auto& rule : profile.rules) {
        if (rule.name == name && rule.enabled) return &rule;
    }
    return nullptr;
}

bool member_can_be_helped(const GroupMemberSnapshot& m) {
    return !m.offline && m.in_zone && !m.dead;
}

bool has_buff(const std::vector<ActiveBuffInfo>& buffs, std::string_view spec) {
    for (const auto& buff : buffs) {
        if (spell_name_matches(buff.spell_name, spec)) return true;
    }
    return false;
}

} // namespace

SupportSettings support_settings_from_profile(const CombatProfile& profile,
                                              int heal_at_hp_pct,
                                              int pet_heal_at_hp_pct) {
    SupportSettings s;
    s.heal_at_hp_pct = heal_at_hp_pct;
    s.critical_hp_pct = std::min(heal_at_hp_pct, 55);
    s.pet_heal_at_hp_pct = pet_heal_at_hp_pct;

    if (const CombatSpellRule* r = find_rule(profile, "Heal")) s.heal_spell = r->spell_name;
    if (const CombatSpellRule* r = find_rule(profile, "HealCritical")) {
        s.critical_heal_spell = r->spell_name;
    }
    if (const CombatSpellRule* r = find_rule(profile, "GroupHeal")) s.group_heal_spell = r->spell_name;
    if (const CombatSpellRule* r = find_rule(profile, "Resurrect")) {
        s.rez_spell = r->spell_name;
        s.allow_rez = true;
    } else {
        s.allow_rez = false;
    }
    if (const CombatSpellRule* r = find_rule(profile, "PetHeal")) {
        s.pet_heal_spell = r->spell_name;
        s.allow_pet_heal = profile.allow_pet_attack;
    } else {
        s.allow_pet_heal = false;
    }
    if (const CombatSpellRule* r = find_rule(profile, "Cure")) s.cure_spell = r->spell_name;
    return s;
}

SupportSettings support_settings_for_profile(const CombatProfile& profile) {
    // The class table stores its heal point as the Heal rule's target_max_hp_pct
    // and the pet point on the PetHeal rule; a saved profile uses the same rules.
    int heal_at = 78;
    int pet_heal_at = 40;
    if (const CombatSpellRule* r = find_rule(profile, "Heal")) heal_at = r->target_max_hp_pct;
    if (const CombatSpellRule* r = find_rule(profile, "PetHeal")) pet_heal_at = r->target_max_hp_pct;
    SupportSettings s = support_settings_from_profile(profile, heal_at, pet_heal_at);
    if (const CombatSpellRule* r = find_rule(profile, "HealCritical")) s.critical_hp_pct = r->target_max_hp_pct;
    return s;
}

std::optional<SupportDecision> choose_heal(const CombatSnapshot& snapshot, const SupportSettings& settings) {
    if (settings.heal_spell.empty() && settings.group_heal_spell.empty()) return std::nullopt;

    // The worst-hurt living member, with the character itself as one candidate.
    struct Candidate {
        std::string name;
        std::uint32_t spawn_id = 0;
        int hp_pct = 100;
        bool self = false;
    };
    std::optional<Candidate> worst;
    int hurt_members = 0;

    for (const auto& m : snapshot.group_members) {
        if (!member_can_be_helped(m)) continue;
        if (static_cast<int>(m.hp_pct) > settings.heal_at_hp_pct) continue;
        ++hurt_members;
        if (!worst.has_value() || m.hp_pct < worst->hp_pct) {
            worst = Candidate{m.name, m.spawn_id, static_cast<int>(m.hp_pct), m.name == snapshot.character_name};
        }
    }

    if (settings.allow_self_heal && !snapshot.me.dead &&
        static_cast<int>(snapshot.hp_pct) <= settings.heal_at_hp_pct) {
        // Only count the character separately when the group list does not hold it.
        const bool listed = worst.has_value() && worst->self;
        if (!listed) ++hurt_members;
        if (!worst.has_value() || snapshot.hp_pct < worst->hp_pct) {
            worst = Candidate{snapshot.character_name, snapshot.me.id, static_cast<int>(snapshot.hp_pct), true};
        }
    }

    if (!worst.has_value()) return std::nullopt;

    // A member about to die outranks a group heal, as the rule table's own order
    // says (HealCritical 110 over GroupHeal 95): the group heal spreads its healing
    // over everyone and is the wrong tool for the one member at the bottom.
    const bool critical = worst->hp_pct <= settings.critical_hp_pct && !settings.critical_heal_spell.empty();
    if (!critical) {
        // A group heal only pays off when more than one member is hurt.
        if (settings.allow_group_heal && !settings.group_heal_spell.empty() &&
            hurt_members >= std::max(2, settings.group_heal_min_members)) {
            SupportDecision d;
            d.kind = SupportActionKind::GroupHeal;
            d.spell_spec = settings.group_heal_spell;
            d.target = TargetKind::Self;
            d.target_name = snapshot.character_name;
            d.target_hp_pct = worst->hp_pct;
            d.priority = 115;
            d.detail = std::to_string(hurt_members) + " members below " + std::to_string(settings.heal_at_hp_pct) + "%";
            return d;
        }
    }

    const std::string& spec = critical ? settings.critical_heal_spell : settings.heal_spell;
    if (spec.empty()) return std::nullopt;

    SupportDecision d;
    d.kind = critical ? SupportActionKind::CriticalHeal : SupportActionKind::Heal;
    d.spell_spec = spec;
    d.target = worst->self ? TargetKind::Self : TargetKind::GroupMember;
    d.target_name = worst->name;
    d.target_spawn_id = worst->spawn_id;
    d.target_hp_pct = worst->hp_pct;
    d.priority = critical ? 130 : 100;
    d.detail = worst->name + " at " + std::to_string(worst->hp_pct) + "%";
    return d;
}

std::optional<SupportDecision> choose_rez(const CombatSnapshot& snapshot, const SupportSettings& settings) {
    if (!settings.allow_rez || settings.rez_spell.empty()) return std::nullopt;
    if (static_cast<int>(snapshot.mana_pct) < settings.rez_min_mana_pct) return std::nullopt;

    for (const auto& m : snapshot.group_members) {
        if (!m.dead || m.offline || !m.in_zone) continue;
        if (m.name == snapshot.character_name) continue; // cannot resurrect yourself

        SupportDecision d;
        d.kind = SupportActionKind::Resurrect;
        d.spell_spec = settings.rez_spell;
        d.target = TargetKind::GroupMember;
        d.target_name = m.name;
        d.target_spawn_id = m.spawn_id;
        d.target_hp_pct = 0;
        d.priority = 150;
        d.detail = m.name + " is dead";
        return d;
    }
    return std::nullopt;
}

std::optional<SupportDecision> choose_pet_heal(const CombatSnapshot& snapshot, const SupportSettings& settings) {
    if (!settings.allow_pet_heal || settings.pet_heal_spell.empty()) return std::nullopt;
    if (!snapshot.has_pet || snapshot.pet_id == 0) return std::nullopt;
    if (static_cast<int>(snapshot.pet_hp_pct) > settings.pet_heal_at_hp_pct) return std::nullopt;

    SupportDecision d;
    d.kind = SupportActionKind::PetHeal;
    d.spell_spec = settings.pet_heal_spell;
    d.target = TargetKind::Pet;
    d.target_spawn_id = snapshot.pet_id;
    d.target_hp_pct = snapshot.pet_hp_pct;
    d.priority = 90;
    d.detail = "pet at " + std::to_string(snapshot.pet_hp_pct) + "%";
    return d;
}

std::optional<SupportDecision> choose_buff(const CombatSnapshot& snapshot,
                                           const std::vector<std::string>& buff_specs) {
    if (buff_specs.empty()) return std::nullopt;
    if (snapshot.me.dead || snapshot.casting || snapshot.memorizing) return std::nullopt;

    // Only the character's own buff list is in the snapshot, so a buff is only
    // maintained on the character here. Buffing the tank from another box needs
    // the tank's buffs, which the group snapshot does not carry yet.
    for (const auto& spec : buff_specs) {
        if (spec.empty()) continue;
        if (has_buff(snapshot.self_buffs, spec)) continue;

        SupportDecision d;
        d.kind = SupportActionKind::Buff;
        d.spell_spec = spec;
        d.target = TargetKind::Self;
        d.target_name = snapshot.character_name;
        d.target_spawn_id = snapshot.me.id;
        d.target_hp_pct = 100;
        d.priority = 60;
        d.detail = "missing " + spec;
        return d;
    }
    return std::nullopt;
}

std::optional<SupportDecision> choose_support_action(const CombatSnapshot& snapshot,
                                                     const SupportSettings& settings,
                                                     const std::vector<std::string>& buff_specs) {
    std::optional<SupportDecision> best;
    auto consider = [&best](std::optional<SupportDecision> d) {
        if (!d.has_value()) return;
        if (!best.has_value() || d->priority > best->priority) best = std::move(d);
    };

    consider(choose_rez(snapshot, settings));
    consider(choose_heal(snapshot, settings));
    consider(choose_pet_heal(snapshot, settings));
    consider(choose_buff(snapshot, buff_specs));
    return best;
}

} // namespace hailstone::combat
