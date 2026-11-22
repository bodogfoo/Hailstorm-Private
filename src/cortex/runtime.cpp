#include "cortex/runtime.h"

#include <algorithm>
#include <utility>

#include "cortex/types.h"
#include "ipc/session_bus.h"

namespace hailstone::cortex {

namespace {

std::string agent_key(const CharacterObservation& obs) {
    return obs.server_name + ":" + obs.character_name;
}

// The proposals assisted mode allows right now: characters observed on the
// last tick, enabled, whose latest decision is still valid and resolves to an
// allowlisted intention, with arbitration run over it. Anything else — a stale
// decision, a character that left the roster, Observe mode, the emergency stop
// — yields nothing.
std::vector<AssistedProposal> build_proposals(const SupervisorSnapshot& snap,
                                              const std::vector<std::string>& observed_keys) {
    std::vector<AssistedProposal> proposals;
    if (snap.emergency_stop) return proposals;

    for (const auto& agent : snap.agents) {
        if (!agent.enabled || agent.mode != OperatingMode::Assisted) continue;
        if (std::find(observed_keys.begin(), observed_keys.end(), agent.key) == observed_keys.end()) {
            continue;
        }
        if (!agent.latest_decision.valid) continue;

        const Intention intention = parse_intention(agent.latest_decision.action_raw);
        if (intention == Intention::None || intention != agent.latest_decision.intention) continue;

        ArbitrationContext ctx;
        ctx.emergency_stop = snap.emergency_stop;
        ctx.mode = agent.mode;
        const ArbitrationResult arbitration = arbitrate(agent.latest_decision, ctx);
        // A proposal is exactly what arbitration refuses to run on our behalf: in
        // assisted mode that is a valid allowlisted decision awaiting human
        // confirmation. What arbitration would let Cortex execute itself is
        // autonomous behaviour, which this runtime never exposes.
        if (arbitration.execute_cortex) continue;

        AssistedProposal proposal;
        proposal.key = agent.key;
        proposal.character_name = agent.identity.character_name;
        proposal.intention = intention;
        proposal.action = agent.latest_decision.action_raw;
        proposal.reason_code = agent.latest_decision.reason_code;
        proposal.explanation = agent.latest_decision.explanation;
        proposal.observed_ms = agent.latest_decision.observed_ms;
        proposal.decided_ms = agent.latest_decision.timestamp_ms;
        proposal.arbitration_owner = arbitration.owner;
        proposal.arbitration_reason = arbitration.reason;
        proposals.push_back(std::move(proposal));
    }

    std::sort(proposals.begin(), proposals.end(),
              [](const AssistedProposal& a, const AssistedProposal& b) { return a.key < b.key; });
    return proposals;
}

} // namespace

CortexRuntime::CortexRuntime(std::shared_ptr<CortexSupervisor> supervisor)
    : supervisor_(std::move(supervisor)) {}

RuntimeReport CortexRuntime::tick(const std::vector<ipc::SessionSnapshot>& roster,
                                  std::uint64_t now_ms) {
    last_observed_ms_ = now_ms;
    last_roster_size_ = roster.size();
    last_observations_.clear();
    observed_keys_.clear();

    if (supervisor_) {
        for (const auto& self : roster) {
            // Only a named, online character is observable: an offline peer has
            // nothing published about it, and an unnamed one has no identity to
            // key an agent on.
            if (!self.online || self.character_name.empty() || self.server_name.empty()) continue;

            CharacterObservation obs = observe(self, roster, now_ms);
            const std::string key = agent_key(obs);
            if (std::find(observed_keys_.begin(), observed_keys_.end(), key) != observed_keys_.end()) {
                continue; // duplicate roster entry for the same character
            }
            observed_keys_.push_back(key);
            last_observations_.push_back(obs);
            // Enable first: submit_observation() only queues inference for an
            // enabled agent, and enabling also stamps the agent's mode.
            supervisor_->set_agent_enabled(key, true);
            supervisor_->submit_observation(obs);
        }
    }

    std::sort(last_observations_.begin(), last_observations_.end(),
              [](const CharacterObservation& a, const CharacterObservation& b) {
                  return agent_key(a) < agent_key(b);
              });
    return report();
}

RuntimeReport CortexRuntime::tick(const ipc::SessionBusClient& bus, std::uint64_t now_ms) {
    return tick(bus.cached_roster(), now_ms);
}

RuntimeReport CortexRuntime::report() const {
    RuntimeReport out;
    out.observed_ms = last_observed_ms_;
    out.roster_size = last_roster_size_;
    out.observations = last_observations_;
    out.online_count = out.observations.size();

    if (supervisor_) {
        out.supervisor = supervisor_->snapshot();
        out.proposals = build_proposals(out.supervisor, observed_keys_);
    }
    return out;
}

} // namespace hailstone::cortex
