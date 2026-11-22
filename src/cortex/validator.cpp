#include "cortex/validator.h"

namespace hailstone::cortex {

namespace {

std::string extract_json_field(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    ++pos;
    std::string out;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '\\' && pos < json.size()) {
            char next = json[pos++];
            out += next;
        } else if (c == '"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace

Decision validate_decision_json(const std::string& raw_output,
                                std::uint64_t observed_ms,
                                std::uint64_t now_ms,
                                std::uint32_t latency_ms,
                                const ValidationConfig& cfg) {
    Decision d;
    d.observed_ms = observed_ms;
    d.timestamp_ms = now_ms;
    d.latency_ms = latency_ms;

    auto lbrace = raw_output.find('{');
    auto rbrace = raw_output.rfind('}');
    if (lbrace == std::string::npos || rbrace == std::string::npos || rbrace <= lbrace) {
        d.valid = false;
        d.rejection_reason = "malformed_json";
        return d;
    }

    std::string json = raw_output.substr(lbrace, rbrace - lbrace + 1);
    d.action_raw = extract_json_field(json, "action");
    d.reason_code = extract_json_field(json, "reason");
    d.explanation = extract_json_field(json, "explanation");
    if (d.explanation.empty()) {
        d.explanation = d.reason_code;
    }

    if (d.action_raw.empty()) {
        d.valid = false;
        d.rejection_reason = "missing_action";
        return d;
    }

    d.intention = parse_intention(d.action_raw);
    if (d.intention == Intention::None) {
        d.valid = false;
        d.rejection_reason = "unsupported_action:" + d.action_raw;
        return d;
    }

    if (now_ms > observed_ms && (now_ms - observed_ms) > cfg.max_decision_age_ms) {
        d.valid = false;
        d.rejection_reason = "stale_observation";
        return d;
    }

    d.valid = true;
    return d;
}

ArbitrationResult arbitrate(const Decision& decision, const ArbitrationContext& ctx) {
    if (ctx.emergency_stop) {
        return {false, "emergency_stop", "global emergency stop active"};
    }
    if (ctx.manual_player_control) {
        return {false, "manual_player", "player has manual control"};
    }
    if (ctx.deterministic_emergency_active) {
        return {false, "deterministic_combat", "deterministic emergency combat active"};
    }
    if (ctx.mode == OperatingMode::Disabled) {
        return {false, "rgmercs", "cortex disabled"};
    }
    if (ctx.mode == OperatingMode::Observe) {
        return {false, "rgmercs", "observation mode: proposals only"};
    }
    if (!decision.valid || decision.intention == Intention::None) {
        return {false, "rgmercs", "no valid cortex decision"};
    }
    if (ctx.mode == OperatingMode::Assisted) {
        return {false, "rgmercs", "assisted mode: awaiting confirmation"};
    }
    return {true, "cortex", "validated autonomous intention"};
}

} // namespace hailstone::cortex
