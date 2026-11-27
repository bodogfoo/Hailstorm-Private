#pragma once

#include <cstdint>

namespace hailstone::combat {

// What the local spawn table says about an id one of the assist sources named.
// Only a spawn this zone holds as a live, attackable NPC can be assisted: a
// stale id (the mob died, zoned, or the source re-targeted) must be skipped,
// not handed on to the engine.
struct AssistSpawnFacts {
    bool present = false;
    bool is_npc = false;
    bool is_corpse = false;
    bool dead = false;
    bool is_pet = false;
    bool is_mercenary = false;
};

inline bool assist_spawn_usable(const AssistSpawnFacts& facts) {
    return facts.present && facts.is_npc && !facts.is_corpse && !facts.dead &&
           !facts.is_pet && !facts.is_mercenary;
}

// Which mob an assist profile attacks. `configured_id` is the target the
// profile's own assist names, read from that peer's SessionBus snapshot (the
// player's choice); `group_id` is the group's main-assist target, which the
// server sends as OP_SetGroupTarget number 0 (GroupAssistNPC[0], the original's
// name). The configured assist wins; a candidate that is not usable is skipped
// so it cannot shadow the other source, which is how a stale GroupAssistNPC id
// used to keep every follower waiting forever while the peer published a target.
template <typename Usable>
inline std::uint32_t select_assist_target(std::uint32_t configured_id,
                                          std::uint32_t group_id,
                                          Usable&& usable) {
    if (configured_id != 0 && usable(configured_id)) return configured_id;
    if (group_id != 0 && usable(group_id)) return group_id;
    return 0;
}

} // namespace hailstone::combat
