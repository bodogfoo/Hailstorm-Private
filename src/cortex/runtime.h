#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cortex/observation.h"
#include "cortex/supervisor.h"
#include "ipc/session_bus.h"

namespace hailstone::cortex {

// One allowlisted intention held for a human to confirm. Assisted mode only.
//
// It is data: nothing in it is an instruction to a client, there is no command
// field, and `action` can only ever be a name from the closed intention
// allowlist — validate_decision_json() (cortex/validator.h) rejects anything
// else before a decision exists. `executed` is always false; this runtime has
// no execution path.
struct AssistedProposal {
    std::string key;            // "<server_name>:<character_name>"
    std::string character_name;
    Intention intention = Intention::None;
    std::string action;         // to_string(intention), allowlisted
    std::string reason_code;    // the model's reason_code, untrusted text
    std::string explanation;    // the model's explanation, untrusted text
    std::uint64_t observed_ms = 0;
    std::uint64_t decided_ms = 0;
    std::string arbitration_owner;
    std::string arbitration_reason;
    bool awaiting_confirmation = true;
    bool executed = false;
};

// A read-only view of one tick: what was observed, what the shared supervisor
// currently holds, and what may be proposed for confirmation.
struct RuntimeReport {
    std::uint64_t observed_ms = 0;
    std::size_t roster_size = 0;    // entries in the roster, online or not
    std::size_t online_count = 0;   // characters observed this tick
    std::vector<CharacterObservation> observations; // online entries, roster order
    SupervisorSnapshot supervisor;  // fresh snapshot of the shared supervisor
    std::vector<AssistedProposal> proposals;        // sorted by key
};

// Connects session-bus telemetry to one shared CortexSupervisor.
//
// Each tick observes every *online*, named roster entry with cortex::observe()
// and submits that observation to the shared supervisor, enabling (and
// registering, on first sight) that character as an agent. `report()` is a
// read-only view: a fresh supervisor snapshot plus the proposals that assisted
// mode currently allows.
//
// Deliberately absent: any way to run an intention. Proposals stop at data —
// the caller shows them, a human confirms them, and the client's own
// RGMercs/validator path remains the only thing that can act.
//
// Not thread-safe: drive one runtime from one thread (the supervisor behind it
// is thread-safe).
class CortexRuntime {
public:
    explicit CortexRuntime(std::shared_ptr<CortexSupervisor> supervisor);

    CortexRuntime(const CortexRuntime&) = delete;
    CortexRuntime& operator=(const CortexRuntime&) = delete;
    CortexRuntime(CortexRuntime&&) = delete;
    CortexRuntime& operator=(CortexRuntime&&) = delete;

    // Observe each online roster entry, submit it, and report. The roster may
    // be empty; the supervisor is untouched in that case.
    RuntimeReport tick(const std::vector<ipc::SessionSnapshot>& roster, std::uint64_t now_ms);

    // Same, taking the roster straight from a session-bus client's cache.
    RuntimeReport tick(const ipc::SessionBusClient& bus, std::uint64_t now_ms);

    // The current view: last tick's observations plus a fresh supervisor
    // snapshot and the proposals it allows right now. Empty before the first
    // tick.
    RuntimeReport report() const;

private:
    std::shared_ptr<CortexSupervisor> supervisor_;
    std::uint64_t last_observed_ms_ = 0;
    std::size_t last_roster_size_ = 0;
    std::vector<CharacterObservation> last_observations_;
    std::vector<std::string> observed_keys_; // characters observed on the last tick
};

} // namespace hailstone::cortex
