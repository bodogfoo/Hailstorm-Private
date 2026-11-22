#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hailstone::cortex {

enum class OperatingMode : std::uint8_t {
    Disabled = 0,
    Observe = 1,
    Assisted = 2,
    Autonomous = 3,
};

inline const char* to_string(OperatingMode mode) {
    switch (mode) {
    case OperatingMode::Disabled: return "DISABLED";
    case OperatingMode::Observe: return "CORTEX_OBSERVE";
    case OperatingMode::Assisted: return "CORTEX_ASSISTED";
    case OperatingMode::Autonomous: return "CORTEX_AUTONOMOUS";
    }
    return "DISABLED";
}

enum class BrainStatus : std::uint8_t {
    Disabled = 0,
    Observing = 1,
    Queued = 2,
    Thinking = 3,
    DecisionReady = 4,
    Waiting = 5,
    Error = 6,
};

inline const char* to_string(BrainStatus status) {
    switch (status) {
    case BrainStatus::Disabled: return "DISABLED";
    case BrainStatus::Observing: return "OBSERVING";
    case BrainStatus::Queued: return "QUEUED";
    case BrainStatus::Thinking: return "THINKING";
    case BrainStatus::DecisionReady: return "DECISION READY";
    case BrainStatus::Waiting: return "WAITING";
    case BrainStatus::Error: return "ERROR";
    }
    return "DISABLED";
}

enum class Intention : std::uint8_t {
    None = 0,
    HoldPull,
    ResumePull,
    RecoverResources,
    RequestAssistance,
    Regroup,
    FollowGroup,
    ContinueCurrentTask,
    ReportProblem,
    FightToLocation,
    ClearObjective,
};

inline const char* to_string(Intention intention) {
    switch (intention) {
    case Intention::None: return "none";
    case Intention::HoldPull: return "hold_pull";
    case Intention::ResumePull: return "resume_pull";
    case Intention::RecoverResources: return "recover_resources";
    case Intention::RequestAssistance: return "request_assistance";
    case Intention::Regroup: return "regroup";
    case Intention::FollowGroup: return "follow_group";
    case Intention::ContinueCurrentTask: return "continue_current_task";
    case Intention::ReportProblem: return "report_problem";
    case Intention::FightToLocation: return "fight_to_location";
    case Intention::ClearObjective: return "clear_objective";
    }
    return "none";
}

inline Intention parse_intention(const std::string& s) {
    if (s == "hold_pull") return Intention::HoldPull;
    if (s == "resume_pull") return Intention::ResumePull;
    if (s == "recover_resources") return Intention::RecoverResources;
    if (s == "request_assistance") return Intention::RequestAssistance;
    if (s == "regroup") return Intention::Regroup;
    if (s == "follow_group") return Intention::FollowGroup;
    if (s == "continue_current_task") return Intention::ContinueCurrentTask;
    if (s == "report_problem") return Intention::ReportProblem;
    if (s == "fight_to_location") return Intention::FightToLocation;
    if (s == "clear_objective") return Intention::ClearObjective;
    return Intention::None;
}

struct CharacterIdentity {
    std::string server_name;
    std::string character_name;
    std::string class_short;
    std::uint8_t level = 0;
    std::string role;        // e.g. "tank", "healer", "dps", "puller", "cc"
    std::string personality; // concise style hint

    std::string key() const {
        return server_name + ":" + character_name;
    }
};

struct Decision {
    Intention intention = Intention::None;
    std::string action_raw;
    std::string reason_code;
    std::string explanation;
    std::uint64_t timestamp_ms = 0;
    std::uint64_t observed_ms = 0;
    std::uint32_t latency_ms = 0;
    bool valid = false;
    bool executed = false;
    std::string rejection_reason;
};

enum class AgentMessageType : std::uint8_t {
    RequestAssistance = 0,
    ReportThreat = 1,
    ReportLowResources = 2,
    ReportObjectiveProgress = 3,
    RecommendTacticalChange = 4,
    ShareDiscovery = 5,
    ReportFailure = 6,
    RequestRegroup = 7,
    AssignResponsibility = 8,
};

inline const char* to_string(AgentMessageType t) {
    switch (t) {
    case AgentMessageType::RequestAssistance: return "request_assistance";
    case AgentMessageType::ReportThreat: return "report_threat";
    case AgentMessageType::ReportLowResources: return "report_low_resources";
    case AgentMessageType::ReportObjectiveProgress: return "report_objective_progress";
    case AgentMessageType::RecommendTacticalChange: return "recommend_tactical_change";
    case AgentMessageType::ShareDiscovery: return "share_discovery";
    case AgentMessageType::ReportFailure: return "report_failure";
    case AgentMessageType::RequestRegroup: return "request_regroup";
    case AgentMessageType::AssignResponsibility: return "assign_responsibility";
    }
    return "unknown";
}

struct AgentMessage {
    std::uint64_t id = 0;
    AgentMessageType type = AgentMessageType::RequestAssistance;
    std::string from_key;
    std::string to_key; // empty = group broadcast
    std::string group_leader;
    std::string payload;
    std::uint64_t timestamp_ms = 0;
    std::uint8_t ttl = 3; // loop prevention
};

} // namespace hailstone::cortex
