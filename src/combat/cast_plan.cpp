#include "combat/cast_plan.h"

#include <string>
#include <utility>

#include "combat/spell_manager.h"

namespace hailstone::combat {

namespace {

CastReadiness refuse(CastBlocker blocker, std::string reason) {
    CastReadiness r;
    r.castable = false;
    r.blocker = blocker;
    r.reason = std::move(reason);
    return r;
}

// resolve_memorized_gem() hides an unusable gem, which would report a greyed-out
// spell as "not memorized". This finds it either way so the two cases differ.
const SpellGemInfo* find_gem_any_state(const CombatSnapshot& snapshot, std::string_view spec, int preferred_gem_1based) {
    if (preferred_gem_1based >= 1) {
        const int idx = preferred_gem_1based - 1;
        for (const auto& gem : snapshot.gems) {
            if (gem.gem_index == idx && gem.spell_id != 0 && spell_name_matches(gem.spell_name, spec)) return &gem;
        }
    }
    for (const auto& gem : snapshot.gems) {
        if (gem.spell_id != 0 && spell_name_matches(gem.spell_name, spec)) return &gem;
    }
    return nullptr;
}

} // namespace

CastReadiness evaluate_cast_readiness(const CombatSnapshot& snapshot,
                                      std::string_view spell_spec,
                                      int preferred_gem_1based,
                                      std::uint64_t last_used_ms,
                                      std::uint64_t cooldown_ms) {
    if (!snapshot.in_zone) return refuse(CastBlocker::NotInZone, "Not in zone");
    if (snapshot.me.dead) return refuse(CastBlocker::CharacterDead, "Character is dead");
    if (snapshot.stunned) return refuse(CastBlocker::Stunned, "Character is stunned");
    if (snapshot.casting) return refuse(CastBlocker::AlreadyCasting, "Casting in progress");
    if (snapshot.memorizing) return refuse(CastBlocker::Memorizing, "Memorization in progress");

    const KnownSpellInfo* known = resolve_known_spell(snapshot, spell_spec);
    if (known == nullptr) {
        return refuse(CastBlocker::NotInSpellbook, "\"" + std::string(spell_spec) + "\" is not in the spellbook");
    }

    CastReadiness r;
    r.book_slot = known->book_slot;
    r.spell_id = known->spell_id;
    r.mana_cost = known->mana_cost;
    r.spell_range = known->range;

    if (known->spell_data_missing) {
        return refuse(CastBlocker::SpellDataMissing,
                      known->spell_name + " has no spell data in this client's spells file");
    }
    if (known->min_level > snapshot.level && known->min_level > 0) {
        return refuse(CastBlocker::LevelTooLow,
                      known->spell_name + " needs level " + std::to_string(known->min_level) + ", character is " +
                          std::to_string(snapshot.level));
    }

    const SpellGemInfo* gem = resolve_memorized_gem(snapshot, spell_spec, preferred_gem_1based);
    if (gem == nullptr) {
        if (const SpellGemInfo* any = find_gem_any_state(snapshot, spell_spec, preferred_gem_1based)) {
            return refuse(CastBlocker::GemLocked,
                          any->spell_name + " is in gem " + std::to_string(any->gem_index + 1) + " but not usable");
        }
        return refuse(CastBlocker::NotMemorized,
                      known->spell_name + (preferred_gem_1based > 0
                                               ? " is not memorized in gem " + std::to_string(preferred_gem_1based)
                                               : " is not memorized"));
    }

    r.gem_index = gem->gem_index;
    r.spell_id = gem->spell_id;
    r.mana_cost = gem->mana_cost;
    r.spell_range = gem->range;

    if (!gem->usable) {
        return refuse(CastBlocker::GemLocked,
                      known->spell_name + " is in gem " + std::to_string(gem->gem_index + 1) + " but not usable");
    }
    if (gem->mana_cost > 0 && snapshot.mana < gem->mana_cost) {
        return refuse(CastBlocker::NotEnoughMana,
                      "Need " + std::to_string(gem->mana_cost) + " mana for " + known->spell_name + ", have " +
                          std::to_string(snapshot.mana));
    }
    if (cooldown_ms > 0 && last_used_ms > 0 && snapshot.now_ms >= last_used_ms) {
        const std::uint64_t elapsed = snapshot.now_ms - last_used_ms;
        if (elapsed < cooldown_ms) {
            return refuse(CastBlocker::CooldownActive,
                          known->spell_name + " cooling down for " + std::to_string(cooldown_ms - elapsed) + " ms");
        }
    }

    if (gem->detrimental) {
        if (!snapshot.has_target) {
            return refuse(CastBlocker::NoTarget, known->spell_name + " needs a hostile target");
        }
        r.distance = snapshot.me.position.distance_3d(snapshot.target.position);
        if (r.spell_range > 0.0f && r.distance > r.spell_range) {
            CastReadiness out = refuse(CastBlocker::TargetOutOfRange,
                                       known->spell_name + " is out of range (" +
                                           std::to_string(static_cast<int>(r.distance)) + " > " +
                                           std::to_string(static_cast<int>(r.spell_range)) + ")");
            // Keep what was measured: the caller reports the distance, and callers
            // that retarget need to know how far the target actually was.
            out.distance = r.distance;
            out.spell_range = r.spell_range;
            return out;
        }
    }

    r.castable = true;
    r.blocker = CastBlocker::None;
    r.reason.clear();
    return r;
}

std::vector<RequiredSpellStatus> check_required_spells(const CombatSnapshot& snapshot,
                                                       const std::vector<std::string>& specs) {
    std::vector<RequiredSpellStatus> out;
    out.reserve(specs.size());
    for (const auto& spec : specs) {
        RequiredSpellStatus st;
        st.spec = spec;
        if (const KnownSpellInfo* known = resolve_known_spell(snapshot, spec)) {
            st.in_spellbook = true;
            st.resolved_name = known->spell_name;
            st.spell_id = known->spell_id;
        }
        if (const SpellGemInfo* gem = resolve_memorized_gem(snapshot, spec)) {
            st.memorized = true;
            st.gem_index = gem->gem_index;
            if (st.resolved_name.empty()) st.resolved_name = gem->spell_name;
            if (st.spell_id == 0) st.spell_id = gem->spell_id;
        }
        out.push_back(std::move(st));
    }
    return out;
}

bool all_required_spells_known(const std::vector<RequiredSpellStatus>& status) {
    for (const auto& st : status) {
        if (!st.in_spellbook) return false;
    }
    return true;
}

} // namespace hailstone::combat
