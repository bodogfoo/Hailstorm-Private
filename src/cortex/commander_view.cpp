#include "cortex/commander_view.h"

#include <unordered_set>

namespace hailstone::cortex {

CortexStatusView to_commander_view(const RuntimeReport& report, bool running) {
    CortexStatusView v;
    v.available = true;
    v.running = running;

    const SupervisorSnapshot& sup = report.supervisor;
    v.mode = sup.mode;
    v.emergency_stop = sup.emergency_stop;
    v.inference_healthy = sup.inference_healthy;
    v.model_name = sup.model_name;
    v.endpoint = sup.endpoint;
    v.pending_queue_size = sup.pending_queue_size;
    v.total_requests = sup.total_requests;
    v.total_decisions = sup.total_decisions;
    v.total_errors = sup.total_errors;

    v.observed_ms = report.observed_ms;
    v.roster_size = report.roster_size;
    v.online_count = report.online_count;

    // Which characters hold a proposal right now, so each one can be shown as
    // waiting for a human next to its state.
    std::unordered_set<std::string> proposing;
    proposing.reserve(report.proposals.size());
    for (const auto& p : report.proposals) proposing.insert(p.key);

    v.characters.reserve(sup.agents.size());
    for (const auto& agent : sup.agents) {
        CortexCharacterView c;
        c.key = agent.key;
        c.server_name = agent.identity.server_name;
        c.character_name = agent.identity.character_name;
        c.status = agent.status;
        c.mode = agent.mode;
        c.enabled = agent.enabled;
        c.objective = agent.current_objective;
        c.intention = agent.current_intention;
        c.has_decision = agent.latest_decision.valid;
        c.decision_action = agent.latest_decision.valid ? agent.latest_decision.action_raw : std::string{};
        c.consecutive_errors = agent.consecutive_errors;
        c.last_error = agent.last_error;
        c.proposal_pending = proposing.count(agent.key) > 0;
        v.characters.push_back(std::move(c));
    }

    v.proposals.reserve(report.proposals.size());
    for (const auto& p : report.proposals) {
        CortexProposalView q;
        q.key = p.key;
        q.character_name = p.character_name;
        q.intention = p.intention;
        q.action = p.action;
        q.reason_code = p.reason_code;
        q.explanation = p.explanation;
        q.arbitration_owner = p.arbitration_owner;
        q.arbitration_reason = p.arbitration_reason;
        q.decided_ms = p.decided_ms;
        q.awaiting_confirmation = p.awaiting_confirmation;
        q.executed = p.executed; // always false: the runtime never executes
        v.proposals.push_back(std::move(q));
    }
    return v;
}

} // namespace hailstone::cortex
