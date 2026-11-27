#pragma once

// Why a cast can or cannot go out, in one structured answer.
//
// The engine used to decide this inline and only say "no rule fired", which is
// what made a missing spell on a boxed character invisible. Every refusal now
// names its blocker, so the diagnostics can report "Child of Fire is not
// memorized" once instead of the character silently standing still.
//
// docs/native-combat.md "Casting" lists the blockers and what clears each.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "combat/combat_types.h"

namespace hailstone::combat {

enum class CastBlocker : std::uint8_t {
    None = 0,
    NotInZone,
    CharacterDead,
    Stunned,
    AlreadyCasting,
    Memorizing,
    NotInSpellbook,
    LevelTooLow,
    SpellDataMissing,
    NotMemorized,
    GemLocked,
    NotEnoughMana,
    CooldownActive,
    NoTarget,
    TargetOutOfRange,
};

inline constexpr std::string_view cast_blocker_name(CastBlocker b) noexcept {
    switch (b) {
    case CastBlocker::None: return "None";
    case CastBlocker::NotInZone: return "NotInZone";
    case CastBlocker::CharacterDead: return "CharacterDead";
    case CastBlocker::Stunned: return "Stunned";
    case CastBlocker::AlreadyCasting: return "AlreadyCasting";
    case CastBlocker::Memorizing: return "Memorizing";
    case CastBlocker::NotInSpellbook: return "NotInSpellbook";
    case CastBlocker::LevelTooLow: return "LevelTooLow";
    case CastBlocker::SpellDataMissing: return "SpellDataMissing";
    case CastBlocker::NotMemorized: return "NotMemorized";
    case CastBlocker::GemLocked: return "GemLocked";
    case CastBlocker::NotEnoughMana: return "NotEnoughMana";
    case CastBlocker::CooldownActive: return "CooldownActive";
    case CastBlocker::NoTarget: return "NoTarget";
    case CastBlocker::TargetOutOfRange: return "TargetOutOfRange";
    }
    return "None";
}

struct CastReadiness {
    bool castable = false;
    CastBlocker blocker = CastBlocker::None;
    // Player-facing sentence, e.g. "Child of Fire is not memorized".
    std::string reason;
    // 0-based gem the spell would cast from, or -1.
    int gem_index = -1;
    std::uint32_t spell_id = 0;
    int book_slot = -1;
    std::uint32_t mana_cost = 0;
    float distance = 0.0f;
    float spell_range = 0.0f;
};

// `spell_spec` supports the profile's "Primary|Fallback1" form.
// `preferred_gem_1based` is 0 when the rule does not pin a gem.
// `last_used_ms`/`cooldown_ms` implement the rule cooldown (0 disables it).
CastReadiness evaluate_cast_readiness(const CombatSnapshot& snapshot,
                                      std::string_view spell_spec,
                                      int preferred_gem_1based,
                                      std::uint64_t last_used_ms,
                                      std::uint64_t cooldown_ms);

// One entry per spec that names a spell the class needs. `specs` entries may use
// "Primary|Fallback" so a class stays useful at a level before its top spell.
struct RequiredSpellStatus {
    std::string spec;
    std::string resolved_name;
    bool in_spellbook = false;
    bool memorized = false;
    int gem_index = -1;
    std::uint32_t spell_id = 0;
};

std::vector<RequiredSpellStatus> check_required_spells(const CombatSnapshot& snapshot,
                                                       const std::vector<std::string>& specs);

// True when every required spec resolved to a spell that is in the book.
bool all_required_spells_known(const std::vector<RequiredSpellStatus>& status);

} // namespace hailstone::combat
