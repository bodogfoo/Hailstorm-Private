#pragma once

// Hailstone Cortex: the observation layer of the agent runtime.
//
// One character's game state, as that character's agent would see it. It is built
// only from what the client already publishes about itself on the session bus
// (`ipc::SessionSnapshot`, filled by `publish_bus_telemetry()` in
// src/app/client_app.cpp), so an observation cannot carry anything the game did not
// report.
//
// Read-only by construction: nothing in this file sends a command, moves, casts,
// targets or writes a file. It is the first stage of the Cortex pipeline
// (observation -> model -> decision -> validated intention); the later stages and
// every live action stay disabled until their safety and regression tests exist
// (docs/cortex-status.md).
//
// Layering: `cortex` sits above `ipc` and below `app`, and depends on `core` only.

#include <cstdint>
#include <string>
#include <vector>

#include "ipc/session_bus.h"

namespace hailstone::cortex {

// One other character of the group, from this character's point of view. Every
// field is that peer's own published value, not something inferred here.
struct PeerObservation {
    std::string session_id;
    std::string character_name;
    std::string class_short;
    std::uint8_t level = 0;
    std::string zone_short;
    std::string target_name;
    std::uint8_t hp_pct = 0;
    std::uint8_t mana_pct = 0;
    std::uint8_t end_pct = 0;
    std::string combat_state;
    std::string script_state;
    std::string group_leader; // the peer's own leader, as it published it
    bool is_leader = false;   // the group's named leader is this peer
};

// What one character can observe about itself and its group.
//
// The group is the set of online roster peers on the same server that name the
// same leader (`SessionSnapshot::group_leader`), plus that leader. A character in
// no group (empty `group_leader`) observes an empty group, and so does a
// character whose peers publish no leader.
struct CharacterObservation {
    // The clock reading the observation was taken at, in milliseconds. Callers
    // pass it in, so a test's observation is reproducible.
    std::uint64_t observed_ms = 0;

    // Identity.
    std::string session_id;
    std::string character_name;
    std::string server_name;
    std::string class_short;
    std::uint8_t level = 0;
    bool online = false;

    // Where.
    std::string zone_short;

    // Condition.
    std::uint8_t hp_pct = 0;
    std::uint8_t mana_pct = 0;
    std::uint8_t end_pct = 0;

    // What it is doing.
    std::string target_name;
    std::uint32_t target_id = 0;
    std::string combat_state;     // Idle, Combat, Paused, EmergencyStop
    std::string connection_state; // in_game, zoning, char_select, login, ...
    std::string follow_target;
    std::string active_profile;
    std::string script_name;
    std::string script_state;  // stopped, running, paused, error, ...
    std::string script_health; // OK, or "ERR: ..."

    // How it is running.
    std::string execution_state; // app::execution_state_name()
    std::uint32_t ping_ms = 0;
    std::uint32_t rss_mib = 0;
    std::uint32_t vram_mib = 0;

    // Group.
    std::string group_leader; // empty when the character is in no group
    bool self_is_leader = false;
    std::vector<PeerObservation> group; // sorted by character_name, never contains self
};

// Observe `self` against the bus roster. The roster may contain `self` and peers
// of other servers; peers are filtered as the struct's comment describes.
CharacterObservation observe(const ipc::SessionSnapshot& self,
                             const std::vector<ipc::SessionSnapshot>& roster,
                             std::uint64_t now_ms);

// The observation as JSON: one line, fixed key order, every string escaped, group
// members sorted. Two observations of the same input produce the same bytes.
std::string to_json(const CharacterObservation& observation);

// The observation as one line of plain text, for a model's prompt context or a
// log line. Nothing is abbreviated or rounded beyond what the observation holds.
std::string to_prompt_context(const CharacterObservation& observation);

} // namespace hailstone::cortex
