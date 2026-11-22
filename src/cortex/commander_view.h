#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cortex/runtime.h"
#include "cortex/types.h"

namespace hailstone::cortex {

// One agent of the shared supervisor as the Commander status path shows it:
// state, and nothing an instruction could hide in.
struct CortexCharacterView {
    std::string key;           // "<server_name>:<character_name>"
    std::string server_name;
    std::string character_name;
    BrainStatus status = BrainStatus::Disabled;
    OperatingMode mode = OperatingMode::Observe;
    bool enabled = false;
    std::string objective;
    Intention intention = Intention::None;
    bool has_decision = false;
    std::string decision_action; // the last decision's allowlisted name, when it is valid
    std::uint32_t consecutive_errors = 0;
    std::string last_error;
    bool proposal_pending = false; // a proposal for this character awaits a human
};

// One assisted proposal as display data: an allowlisted intention held for a
// human to confirm, plus who refused to run it and why. There is no command
// field and nothing here can be handed to the game: `executed` is always false.
struct CortexProposalView {
    std::string key;
    std::string character_name;
    Intention intention = Intention::None;
    std::string action;         // to_string(intention), allowlisted
    std::string reason_code;    // the model's reason_code, untrusted text
    std::string explanation;    // the model's explanation, untrusted text
    std::string arbitration_owner;
    std::string arbitration_reason;
    std::uint64_t decided_ms = 0;
    bool awaiting_confirmation = true;
    bool executed = false;
};

// The Cortex slice of UiHost::CommanderStatusSnapshot: what the supervisor
// holds right now, in the shape hs.CommanderStatus() hands to the UI pack.
//
// Data only. The view is built by to_commander_view() from a read-only
// RuntimeReport; there is no method here that runs, confirms or dispatches
// anything, so the display path cannot become an execution path.
struct CortexStatusView {
    bool available = false;  // the application owns a service wired in
    bool running = false;    // the shared supervisor is started
    OperatingMode mode = OperatingMode::Observe;
    bool emergency_stop = false;
    bool inference_healthy = false;
    std::string model_name;
    std::string endpoint;
    std::size_t pending_queue_size = 0;
    std::uint64_t total_requests = 0;
    std::uint64_t total_decisions = 0;
    std::uint64_t total_errors = 0;
    // The tick the characters below were observed on.
    std::uint64_t observed_ms = 0;
    std::size_t roster_size = 0;
    std::size_t online_count = 0;
    std::vector<CortexCharacterView> characters;  // by key, supervisor order
    std::vector<CortexProposalView> proposals;    // by key, awaiting a human
};

// Map one runtime report onto the Commander view. Read-only: the report is
// copied into display fields, nothing is kept, queued or run. `running` is the
// service's own started flag, which the report does not carry.
CortexStatusView to_commander_view(const RuntimeReport& report, bool running);

} // namespace hailstone::cortex
