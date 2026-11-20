#include "combat/spell_manager.h"

#include <algorithm>
#include <cctype>
#include <numeric>

namespace hailstone::combat {
namespace {

std::string trim_copy(std::string_view s) {
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return std::string(s.substr(b, e - b));
}

std::string to_lower_copy(std::string_view s) {
    std::string out(s);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::string normalize_rank_suffix(std::string_view s) {
    std::string lower = to_lower_copy(trim_copy(s));
    const char* suffixes[] = {" rk. iii", " rk. ii", " rk. i", " rk iii", " rk ii"};
    for (const char* suf : suffixes) {
        std::string_view sv(suf);
        if (lower.size() > sv.size() &&
            lower.compare(lower.size() - sv.size(), sv.size(), sv) == 0) {
            return trim_copy(lower.substr(0, lower.size() - sv.size()));
        }
    }
    return lower;
}

bool single_spell_matches_exact(std::string_view candidate, std::string_view target) {
    const std::string a = to_lower_copy(trim_copy(candidate));
    const std::string b = to_lower_copy(trim_copy(target));
    return !a.empty() && a == b;
}

bool single_spell_matches_norm(std::string_view candidate, std::string_view target) {
    const std::string a = normalize_rank_suffix(candidate);
    const std::string b = normalize_rank_suffix(target);
    return !a.empty() && a == b;
}

bool has_buff_matching(const std::vector<ActiveBuffInfo>& buffs,
                       std::string_view spec,
                       std::uint32_t resolved_spell_id) {
    for (const auto& buff : buffs) {
        if (resolved_spell_id != 0 && buff.spell_id == resolved_spell_id) {
            return true;
        }
        if (spell_name_matches(buff.spell_name, spec)) {
            return true;
        }
    }
    return false;
}

const SpawnSnapshot* find_spawn_by_id(const CombatSnapshot& snap, std::uint32_t id) {
    if (id == 0) {
        return nullptr;
    }
    if (snap.me.id == id) {
        return &snap.me;
    }
    if (snap.has_target && snap.target.id == id) {
        return &snap.target;
    }
    for (const auto& s : snap.spawns) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

struct ResolvedRuleTarget {
    std::uint32_t spawn_id = 0;
    std::string name;
    std::uint8_t hp_pct = 100;
    float distance = 0.0f;
    const std::vector<ActiveBuffInfo>* buffs = nullptr;
    bool valid = false;
};

ResolvedRuleTarget resolve_rule_target(const CombatSnapshot& snapshot,
                                       const SpawnSnapshot* hostile_target,
                                       TargetKind kind) {
    ResolvedRuleTarget out;
    switch (kind) {
    case TargetKind::HostileTarget: {
        if (!hostile_target || hostile_target->id == 0 || hostile_target->dead ||
            hostile_target->is_corpse || hostile_target->hp_pct == 0) {
            return out;
        }
        out.spawn_id = hostile_target->id;
        out.name = hostile_target->name;
        out.hp_pct = hostile_target->hp_pct;
        out.distance = snapshot.me.position.distance_2d(hostile_target->position);
        if (snapshot.has_target && snapshot.target.id == hostile_target->id) {
            out.buffs = &snapshot.target_buffs;
        }
        out.valid = true;
        return out;
    }
    case TargetKind::Self: {
        if (snapshot.me.dead) {
            return out;
        }
        out.spawn_id = snapshot.me.id;
        out.name = snapshot.me.name.empty() ? snapshot.character_name : snapshot.me.name;
        out.hp_pct = snapshot.hp_pct;
        out.distance = 0.0f;
        out.buffs = &snapshot.self_buffs;
        out.valid = true;
        return out;
    }
    case TargetKind::Pet: {
        if (!snapshot.has_pet || snapshot.pet_id == 0) {
            return out;
        }
        out.spawn_id = snapshot.pet_id;
        out.hp_pct = snapshot.pet_hp_pct;
        out.buffs = &snapshot.pet_buffs;
        if (const auto* pet_spawn = find_spawn_by_id(snapshot, snapshot.pet_id)) {
            if (pet_spawn->dead || pet_spawn->is_corpse) {
                return out;
            }
            out.name = pet_spawn->name;
            out.distance = snapshot.me.position.distance_2d(pet_spawn->position);
        } else {
            out.name = "Pet";
            out.distance = 0.0f;
        }
        out.valid = true;
        return out;
    }
    case TargetKind::GroupTank: {
        const GroupMemberSnapshot* tank = nullptr;
        for (const auto& m : snapshot.group_members) {
            if (m.offline || !m.in_zone || m.spawn_id == 0) {
                continue;
            }
            if (m.main_tank) {
                tank = &m;
                break;
            }
        }
        if (!tank) {
            for (const auto& m : snapshot.group_members) {
                if (m.offline || !m.in_zone || m.spawn_id == 0) {
                    continue;
                }
                if (m.is_leader) {
                    tank = &m;
                    break;
                }
            }
        }
        if (!tank) {
            return out;
        }
        out.spawn_id = tank->spawn_id;
        out.name = tank->name;
        out.hp_pct = tank->hp_pct;
        out.distance = tank->distance;
        if (const auto* sp = find_spawn_by_id(snapshot, tank->spawn_id)) {
            out.distance = snapshot.me.position.distance_2d(sp->position);
            if (sp->dead || sp->is_corpse) {
                return out;
            }
        }
        out.valid = true;
        return out;
    }
    case TargetKind::GroupLowestHp:
    case TargetKind::GroupMember: {
        const GroupMemberSnapshot* lowest = nullptr;
        for (const auto& m : snapshot.group_members) {
            if (m.offline || !m.in_zone || m.spawn_id == 0 || m.hp_pct == 0) {
                continue;
            }
            if (!lowest || m.hp_pct < lowest->hp_pct) {
                lowest = &m;
            }
        }
        if (!lowest) {
            return out;
        }
        out.spawn_id = lowest->spawn_id;
        out.name = lowest->name;
        out.hp_pct = lowest->hp_pct;
        out.distance = lowest->distance;
        if (const auto* sp = find_spawn_by_id(snapshot, lowest->spawn_id)) {
            out.distance = snapshot.me.position.distance_2d(sp->position);
            if (sp->dead || sp->is_corpse) {
                return out;
            }
        }
        out.valid = true;
        return out;
    }
    }
    return out;
}

} // namespace

std::vector<std::string> split_pipe_alternatives(std::string_view spec) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= spec.size()) {
        std::size_t bar = spec.find('|', pos);
        std::string item = trim_copy(bar == std::string_view::npos
                                         ? spec.substr(pos)
                                         : spec.substr(pos, bar - pos));
        if (!item.empty()) {
            out.push_back(std::move(item));
        }
        if (bar == std::string_view::npos) {
            break;
        }
        pos = bar + 1;
    }
    return out;
}

bool spell_name_matches(std::string_view candidate_name, std::string_view spec) {
    const auto alts = split_pipe_alternatives(spec);
    for (const auto& alt : alts) {
        if (single_spell_matches_exact(candidate_name, alt)) {
            return true;
        }
    }
    for (const auto& alt : alts) {
        if (single_spell_matches_norm(candidate_name, alt)) {
            return true;
        }
    }
    return false;
}

const KnownSpellInfo* resolve_known_spell(const CombatSnapshot& snapshot, std::string_view spec) {
    const auto alts = split_pipe_alternatives(spec);
    for (const auto& alt : alts) {
        for (const auto& known : snapshot.spellbook) {
            if (known.spell_id != 0 && single_spell_matches_exact(known.spell_name, alt)) {
                return &known;
            }
        }
    }
    for (const auto& alt : alts) {
        for (const auto& known : snapshot.spellbook) {
            if (known.spell_id != 0 && single_spell_matches_norm(known.spell_name, alt)) {
                return &known;
            }
        }
    }
    return nullptr;
}

const SpellGemInfo* resolve_memorized_gem(const CombatSnapshot& snapshot,
                                          std::string_view spec,
                                          int preferred_gem_1based) {
    const auto alts = split_pipe_alternatives(spec);
    if (alts.empty()) {
        return nullptr;
    }

    if (preferred_gem_1based >= 1) {
        const int target_idx = preferred_gem_1based - 1;
        for (const auto& gem : snapshot.gems) {
            if (gem.gem_index == target_idx && gem.usable && gem.spell_id != 0 &&
                spell_name_matches(gem.spell_name, spec)) {
                return &gem;
            }
        }
    }

    for (const auto& alt : alts) {
        for (const auto& gem : snapshot.gems) {
            if (gem.usable && gem.spell_id != 0 &&
                single_spell_matches_exact(gem.spell_name, alt)) {
                return &gem;
            }
        }
    }
    for (const auto& alt : alts) {
        for (const auto& gem : snapshot.gems) {
            if (gem.usable && gem.spell_id != 0 &&
                single_spell_matches_norm(gem.spell_name, alt)) {
                return &gem;
            }
        }
    }
    return nullptr;
}

LoadoutAudit audit_loadout(const CombatSnapshot& snapshot, const CombatProfile& profile) {
    LoadoutAudit audit;
    int max_slots = std::clamp(snapshot.usable_gems, 0, 12);
    for (const auto& entry : profile.loadout) {
        if (entry.gem_index >= 0 && entry.gem_index < 12) {
            max_slots = std::max(max_slots, entry.gem_index + 1);
        }
    }
    max_slots = std::max(max_slots, static_cast<int>(std::min<std::size_t>(snapshot.gems.size(), 12)));

    audit.slots.reserve(static_cast<std::size_t>(max_slots));
    for (int idx = 0; idx < max_slots; ++idx) {
        GemAuditEntry slot;
        slot.gem_index = idx;

        const SpellGemInfo* current_gem = nullptr;
        for (const auto& g : snapshot.gems) {
            if (g.gem_index == idx) {
                current_gem = &g;
                break;
            }
        }
        if (current_gem) {
            slot.current_spell_id = current_gem->spell_id;
            slot.current_spell_name = current_gem->spell_name;
        }

        const DesiredGemEntry* desired = nullptr;
        for (const auto& d : profile.loadout) {
            if (d.gem_index == idx) {
                desired = &d;
                break;
            }
        }

        if (!desired || desired->spell_name.empty()) {
            slot.status = (idx >= snapshot.usable_gems) ? GemSyncKind::SlotLocked : GemSyncKind::UnusedSlot;
            audit.slots.push_back(std::move(slot));
            continue;
        }

        slot.desired_spec = desired->spell_name;
        if (idx >= snapshot.usable_gems) {
            slot.status = GemSyncKind::SlotLocked;
            audit.slots.push_back(std::move(slot));
            continue;
        }

        const KnownSpellInfo* known = resolve_known_spell(snapshot, desired->spell_name);
        if (known) {
            slot.resolved_spell_id = known->spell_id;
            slot.resolved_spell_name = known->spell_name;
            slot.book_slot = known->book_slot;

            const bool id_match = (slot.current_spell_id != 0 && slot.current_spell_id == known->spell_id);
            const bool name_match = (slot.current_spell_id != 0 &&
                                     spell_name_matches(slot.current_spell_name, desired->spell_name));
            if (id_match || name_match) {
                slot.status = GemSyncKind::Match;
            } else if (slot.current_spell_id == 0) {
                slot.status = GemSyncKind::EmptyMissing;
                audit.any_missing = true;
                audit.missing_spells.push_back(
                    "Gem " + std::to_string(idx + 1) + ": " + known->spell_name);
            } else {
                slot.status = GemSyncKind::ManuallyChanged;
                audit.any_manual_conflict = true;
                audit.manually_changed_gems.push_back(
                    "Gem " + std::to_string(idx + 1) + ": " + slot.current_spell_name +
                    " (desired: " + known->spell_name + ")");
            }
        } else {
            if (slot.current_spell_id != 0 &&
                spell_name_matches(slot.current_spell_name, desired->spell_name)) {
                slot.resolved_spell_id = slot.current_spell_id;
                slot.resolved_spell_name = slot.current_spell_name;
                slot.status = GemSyncKind::Match;
            } else {
                slot.status = GemSyncKind::UnknownInBook;
                audit.unknown_spells.push_back(
                    "Gem " + std::to_string(idx + 1) + ": " + desired->spell_name);
            }
        }

        audit.slots.push_back(std::move(slot));
    }

    return audit;
}

RememorizePlan plan_rememorize(const CombatSnapshot& snapshot,
                               const CombatProfile& profile,
                               const LoadoutAudit& audit,
                               bool user_requested) {
    RememorizePlan plan;

    if (!snapshot.in_zone) {
        plan.blocked_reason = "Not in zone";
        return plan;
    }
    if (snapshot.me.dead) {
        plan.blocked_reason = "Character is dead";
        return plan;
    }
    if (snapshot.in_combat) {
        plan.blocked_reason = "In combat";
        return plan;
    }
    if (snapshot.stunned) {
        plan.blocked_reason = "Character is stunned";
        return plan;
    }
    if (snapshot.casting) {
        plan.blocked_reason = "Casting in progress";
        return plan;
    }
    if (snapshot.memorizing) {
        plan.blocked_reason = "Memorization already in progress";
        return plan;
    }
    if (snapshot.cursor_busy) {
        plan.blocked_reason = "Cursor is busy";
        return plan;
    }
    if (snapshot.player_moving_manually || snapshot.manual_move_backward || snapshot.nav_active) {
        plan.blocked_reason = "Movement active";
        return plan;
    }
    if (!user_requested && !profile.rememorize_missing) {
        plan.blocked_reason = "Automatic rememorize_missing is disabled";
        return plan;
    }

    bool skipped_manual_only = false;
    for (const auto& slot : audit.slots) {
        if (slot.resolved_spell_id == 0 || slot.book_slot < 0) {
            continue;
        }
        if (slot.status == GemSyncKind::EmptyMissing) {
            plan.slots_to_memorize.push_back(slot);
        } else if (slot.status == GemSyncKind::ManuallyChanged) {
            // Hard safety rule: NEVER overwrite manually changed gems unless auto_loadout is enabled.
            if (profile.auto_loadout) {
                plan.slots_to_memorize.push_back(slot);
            } else {
                skipped_manual_only = true;
            }
        }
    }

    if (plan.slots_to_memorize.empty()) {
        if (skipped_manual_only) {
            plan.blocked_reason = "Manually changed gems protected (enable auto_loadout to overwrite)";
        } else {
            plan.blocked_reason = "No missing spells to rememorize";
        }
        return plan;
    }

    plan.allowed = true;
    plan.use_batch_set = (plan.slots_to_memorize.size() > 1);
    return plan;
}

RuleSelection select_next_rule(const CombatSnapshot& snapshot,
                               const CombatProfile& profile,
                               const SpawnSnapshot* hostile_target,
                               bool in_combat_state,
                               bool has_engaged_target,
                               const std::unordered_map<std::string, std::uint64_t>& last_used_ms) {
    RuleSelection best;
    if (snapshot.me.dead || snapshot.stunned || snapshot.casting) {
        return best;
    }

    std::vector<std::size_t> order(profile.rules.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return profile.rules[a].priority > profile.rules[b].priority;
    });

    for (std::size_t idx : order) {
        const CombatSpellRule& rule = profile.rules[idx];
        if (!rule.enabled) {
            continue;
        }

        if (rule.cooldown_ms > 0) {
            auto it = last_used_ms.find(rule.name);
            if (it != last_used_ms.end() && snapshot.now_ms >= it->second &&
                (snapshot.now_ms - it->second) < rule.cooldown_ms) {
                continue;
            }
        }

        bool cond_ok = true;
        switch (rule.combat_condition) {
        case CombatCondition::Always:
            cond_ok = true;
            break;
        case CombatCondition::InCombatOnly:
            cond_ok = in_combat_state;
            break;
        case CombatCondition::OutOfCombatOnly:
            cond_ok = !in_combat_state;
            break;
        case CombatCondition::EngagedTargetOnly:
            cond_ok = in_combat_state && has_engaged_target;
            break;
        }
        if (!cond_ok) {
            continue;
        }

        if (snapshot.mana_pct < rule.min_mana_pct || snapshot.mana_pct > rule.max_mana_pct) {
            continue;
        }
        if (snapshot.hp_pct < rule.self_min_hp_pct || snapshot.hp_pct > rule.self_max_hp_pct) {
            continue;
        }

        ResolvedRuleTarget resolved_target = resolve_rule_target(snapshot, hostile_target, rule.target);
        if (!resolved_target.valid) {
            continue;
        }
        if (resolved_target.hp_pct < rule.target_min_hp_pct ||
            resolved_target.hp_pct > rule.target_max_hp_pct) {
            continue;
        }
        if (resolved_target.distance < rule.min_range || resolved_target.distance > rule.max_range) {
            continue;
        }

        if (rule.action_kind == SpellActionKind::CastSpell) {
            const SpellGemInfo* gem = resolve_memorized_gem(
                snapshot, rule.spell_name, rule.preferred_gem);
            if (!gem || !gem->ready || gem->spell_id == 0) {
                continue;
            }
            if (snapshot.mana < gem->mana_cost && gem->mana_cost > 0 && snapshot.max_mana > 0) {
                continue;
            }
            if (rule.require_buff_missing) {
                const std::vector<ActiveBuffInfo>* buffs = resolved_target.buffs;
                if (buffs && has_buff_matching(*buffs, rule.spell_name, gem->spell_id)) {
                    continue;
                }
                if (!buffs && rule.target == TargetKind::HostileTarget &&
                    snapshot.has_target && snapshot.target.id == resolved_target.spawn_id &&
                    has_buff_matching(snapshot.target_buffs, rule.spell_name, gem->spell_id)) {
                    continue;
                }
            }

            best.rule = &rule;
            best.gem = gem;
            best.target_spawn_id = resolved_target.spawn_id;
            best.target_name = resolved_target.name;
            best.resolved_spell_name = gem->spell_name;
            best.summary = "Cast " + gem->spell_name + " (Gem " +
                           std::to_string(gem->gem_index + 1) + ") -> " +
                           resolved_target.name;
            return best;
        }

        if (rule.action_kind == SpellActionKind::CombatAbility) {
            if (!profile.use_abilities || rule.spell_name.empty()) {
                continue;
            }
            const auto alts = split_pipe_alternatives(rule.spell_name);
            if (alts.empty()) {
                continue;
            }
            best.rule = &rule;
            best.gem = nullptr;
            best.target_spawn_id = resolved_target.spawn_id;
            best.target_name = resolved_target.name;
            best.resolved_spell_name = alts.front();
            best.summary = "Ability " + alts.front() + " -> " + resolved_target.name;
            return best;
        }

        if (rule.action_kind == SpellActionKind::PetAttack) {
            if (!profile.allow_pet_attack || !snapshot.has_pet || snapshot.pet_id == 0) {
                continue;
            }
            best.rule = &rule;
            best.gem = nullptr;
            best.target_spawn_id = resolved_target.spawn_id;
            best.target_name = resolved_target.name;
            best.resolved_spell_name = "PetAttack";
            best.summary = "PetAttack -> " + resolved_target.name;
            return best;
        }

        if (rule.action_kind == SpellActionKind::MeleeAttack) {
            if (!profile.allow_melee_attack) {
                continue;
            }
            best.rule = &rule;
            best.gem = nullptr;
            best.target_spawn_id = resolved_target.spawn_id;
            best.target_name = resolved_target.name;
            best.resolved_spell_name = "MeleeAttack";
            best.summary = "MeleeAttack -> " + resolved_target.name;
            return best;
        }
    }

    return best;
}

} // namespace hailstone::combat
