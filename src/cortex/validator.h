#pragma once

#include <cstdint>
#include <string>

#include "cortex/types.h"

namespace hailstone::cortex {

struct ValidationConfig {
    std::uint64_t max_decision_age_ms = 10000;
};

struct ArbitrationContext {
    bool emergency_stop = false;
    bool manual_player_control = false;
    bool deterministic_emergency_active = false;
    OperatingMode mode = OperatingMode::Observe;
};

struct ArbitrationResult {
    bool execute_cortex = false;
    std::string owner; // "emergency_stop", "manual_player", "deterministic_combat", "cortex", "rgmercs"
    std::string reason;
};

// Parse and validate raw LLM JSON output into a Decision.
Decision validate_decision_json(const std::string& raw_output,
                                std::uint64_t observed_ms,
                                std::uint64_t now_ms,
                                std::uint32_t latency_ms,
                                const ValidationConfig& cfg = {});

// Enforce ownership & arbitration contract.
ArbitrationResult arbitrate(const Decision& decision, const ArbitrationContext& ctx);

} // namespace hailstone::cortex
