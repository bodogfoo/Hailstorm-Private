#include "epic/epic_types.h"

#include <algorithm>
#include <cctype>

#include "core/ascii_case.h"

namespace hailstone::epic {
namespace {

std::string normalize_key_part(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '_' || c == '-') {
            out.push_back(static_cast<char>(std::tolower(c)));
        } else if (std::isspace(c)) {
            out.push_back('_');
        }
    }
    return out;
}

} // namespace

const char* progress_state_name(EpicProgressState s) noexcept {
    switch (s) {
    case EpicProgressState::Unknown: return "Unknown";
    case EpicProgressState::NotStarted: return "Not started";
    case EpicProgressState::InProgress: return "In progress";
    case EpicProgressState::AwaitingItem: return "Awaiting item";
    case EpicProgressState::AwaitingTurnIn: return "Awaiting turn-in";
    case EpicProgressState::ReadyForCompletion: return "Ready for completion";
    case EpicProgressState::Completed: return "Completed";
    case EpicProgressState::CompletedWithUpgrade: return "Completed with upgrade available";
    case EpicProgressState::Blocked: return "Blocked";
    case EpicProgressState::ManualOverride: return "Manual override";
    }
    return "Unknown";
}

EpicProgressState parse_progress_state(std::string_view s, EpicProgressState fallback) noexcept {
    if (iequals(s, "Unknown")) return EpicProgressState::Unknown;
    if (iequals(s, "Not started") || iequals(s, "NotStarted")) return EpicProgressState::NotStarted;
    if (iequals(s, "In progress") || iequals(s, "InProgress")) return EpicProgressState::InProgress;
    if (iequals(s, "Awaiting item") || iequals(s, "AwaitingItem")) return EpicProgressState::AwaitingItem;
    if (iequals(s, "Awaiting turn-in") || iequals(s, "AwaitingTurnIn")) return EpicProgressState::AwaitingTurnIn;
    if (iequals(s, "Ready for completion") || iequals(s, "ReadyForCompletion")) return EpicProgressState::ReadyForCompletion;
    if (iequals(s, "Completed")) return EpicProgressState::Completed;
    if (iequals(s, "Completed with upgrade available") || iequals(s, "CompletedWithUpgrade")) return EpicProgressState::CompletedWithUpgrade;
    if (iequals(s, "Blocked")) return EpicProgressState::Blocked;
    if (iequals(s, "Manual override") || iequals(s, "ManualOverride")) return EpicProgressState::ManualOverride;
    return fallback;
}

const char* loot_outcome_name(LootDecisionOutcome o) noexcept {
    switch (o) {
    case LootDecisionOutcome::LootForCharacter: return "Loot for character";
    case LootDecisionOutcome::LootForAnotherCharacter: return "Loot for another character";
    case LootDecisionOutcome::LootAndAssignLater: return "Loot and assign later";
    case LootDecisionOutcome::LeaveForAnotherGroup: return "Leave for another group";
    case LootDecisionOutcome::LeaveBecauseUnnecessary: return "Leave because unnecessary";
    case LootDecisionOutcome::DeferDecision: return "Defer decision";
    case LootDecisionOutcome::ManualReviewRequired: return "Manual review required";
    case LootDecisionOutcome::NeverLoot: return "Never loot";
    }
    return "Defer decision";
}

LootDecisionOutcome parse_loot_outcome(std::string_view s, LootDecisionOutcome fallback) noexcept {
    if (iequals(s, "Loot for character") || iequals(s, "LootForCharacter")) return LootDecisionOutcome::LootForCharacter;
    if (iequals(s, "Loot for another character") || iequals(s, "LootForAnotherCharacter")) return LootDecisionOutcome::LootForAnotherCharacter;
    if (iequals(s, "Loot and assign later") || iequals(s, "LootAndAssignLater")) return LootDecisionOutcome::LootAndAssignLater;
    if (iequals(s, "Leave for another group") || iequals(s, "LeaveForAnotherGroup")) return LootDecisionOutcome::LeaveForAnotherGroup;
    if (iequals(s, "Leave because unnecessary") || iequals(s, "LeaveBecauseUnnecessary")) return LootDecisionOutcome::LeaveBecauseUnnecessary;
    if (iequals(s, "Defer decision") || iequals(s, "DeferDecision")) return LootDecisionOutcome::DeferDecision;
    if (iequals(s, "Manual review required") || iequals(s, "ManualReviewRequired")) return LootDecisionOutcome::ManualReviewRequired;
    if (iequals(s, "Never loot") || iequals(s, "NeverLoot")) return LootDecisionOutcome::NeverLoot;
    return fallback;
}

const char* conflict_rule_name(ConflictResolutionRule r) noexcept {
    switch (r) {
    case ConflictResolutionRule::ActiveStageFirst: return "ActiveStageFirst";
    case ConflictResolutionRule::HighestPriorityCharacter: return "HighestPriorityCharacter";
    case ConflictResolutionRule::MainCharacterFirst: return "MainCharacterFirst";
    case ConflictResolutionRule::HighestStageProgress: return "HighestStageProgress";
    case ConflictResolutionRule::GroupPriorityThenCharacter: return "GroupPriorityThenCharacter";
    case ConflictResolutionRule::ClassPriority: return "ClassPriority";
    case ConflictResolutionRule::ManualApproval: return "ManualApproval";
    }
    return "ActiveStageFirst";
}

ConflictResolutionRule parse_conflict_rule(std::string_view s, ConflictResolutionRule fallback) noexcept {
    if (iequals(s, "ActiveStageFirst")) return ConflictResolutionRule::ActiveStageFirst;
    if (iequals(s, "HighestPriorityCharacter")) return ConflictResolutionRule::HighestPriorityCharacter;
    if (iequals(s, "MainCharacterFirst")) return ConflictResolutionRule::MainCharacterFirst;
    if (iequals(s, "HighestStageProgress")) return ConflictResolutionRule::HighestStageProgress;
    if (iequals(s, "GroupPriorityThenCharacter")) return ConflictResolutionRule::GroupPriorityThenCharacter;
    if (iequals(s, "ClassPriority")) return ConflictResolutionRule::ClassPriority;
    if (iequals(s, "ManualApproval")) return ConflictResolutionRule::ManualApproval;
    return fallback;
}

const char* signal_source_name(SignalSource src) noexcept {
    switch (src) {
    case SignalSource::Unknown: return "Unknown";
    case SignalSource::InventoryScan: return "InventoryScan";
    case SignalSource::EquippedScan: return "EquippedScan";
    case SignalSource::QuestState: return "QuestState";
    case SignalSource::ManualEntry: return "ManualEntry";
    case SignalSource::ManualOverride: return "ManualOverride";
    case SignalSource::Imported: return "Imported";
    case SignalSource::Reconciliation: return "Reconciliation";
    case SignalSource::PeerBus: return "PeerBus";
    }
    return "Unknown";
}

SignalSource parse_signal_source(std::string_view s, SignalSource fallback) noexcept {
    if (iequals(s, "InventoryScan")) return SignalSource::InventoryScan;
    if (iequals(s, "EquippedScan")) return SignalSource::EquippedScan;
    if (iequals(s, "QuestState")) return SignalSource::QuestState;
    if (iequals(s, "ManualEntry")) return SignalSource::ManualEntry;
    if (iequals(s, "ManualOverride")) return SignalSource::ManualOverride;
    if (iequals(s, "Imported")) return SignalSource::Imported;
    if (iequals(s, "Reconciliation")) return SignalSource::Reconciliation;
    if (iequals(s, "PeerBus")) return SignalSource::PeerBus;
    return fallback;
}

const char* confidence_level_name(ConfidenceLevel c) noexcept {
    switch (c) {
    case ConfidenceLevel::None: return "None";
    case ConfidenceLevel::Low: return "Low";
    case ConfidenceLevel::Medium: return "Medium";
    case ConfidenceLevel::High: return "High";
    case ConfidenceLevel::Confirmed: return "Confirmed";
    }
    return "None";
}

ConfidenceLevel parse_confidence_level(std::string_view s, ConfidenceLevel fallback) noexcept {
    if (iequals(s, "Low")) return ConfidenceLevel::Low;
    if (iequals(s, "Medium")) return ConfidenceLevel::Medium;
    if (iequals(s, "High")) return ConfidenceLevel::High;
    if (iequals(s, "Confirmed")) return ConfidenceLevel::Confirmed;
    return fallback;
}

std::string CharacterEpicKey::storage_key() const {
    return normalize_key_part(server) + "::" +
           normalize_key_part(character) + "::" +
           normalize_key_part(class_short) + "::" +
           normalize_key_part(epic_variant);
}

bool CharacterEpicKey::matches(const CharacterEpicKey& other) const {
    return iequals(server, other.server) &&
           iequals(character, other.character) &&
           iequals(class_short, other.class_short) &&
           iequals(epic_variant, other.epic_variant);
}

bool EpicDefinition::supports_class(std::string_view class_short) const {
    if (class_restrictions.empty()) return true;
    for (const auto& c : class_restrictions) {
        if (iequals(c, "ALL") || iequals(c, class_short)) return true;
    }
    return false;
}

bool EpicDefinition::supports_server(std::string_view server_name) const {
    if (server_rules.empty() || server_name.empty()) return true;
    for (const auto& rule : server_rules) {
        if (rule.server_pattern.empty() || rule.server_pattern == "*" ||
            iequals(rule.server_pattern, server_name)) {
            return true;
        }
        // Prefix/substring match for patterns like "ez" matching "EZ Server" or "ez-server"
        std::string pat = normalize_key_part(rule.server_pattern);
        std::string srv = normalize_key_part(server_name);
        if (!pat.empty() && srv.find(pat) != std::string::npos) return true;
    }
    return true;
}

const EpicStageDefinition* EpicDefinition::find_stage(std::string_view stage_id) const {
    for (const auto& st : stages) {
        if (iequals(st.stage_id, stage_id)) return &st;
    }
    return nullptr;
}

double CharacterEpicProfile::progress_percent() const {
    if (has_completed_epic || state == EpicProgressState::Completed ||
        state == EpicProgressState::CompletedWithUpgrade) {
        return 100.0;
    }
    if (total_stages <= 0) return 0.0;
    const double pct = (static_cast<double>(completed_stages.size()) / static_cast<double>(total_stages)) * 100.0;
    return std::clamp(pct, 0.0, 100.0);
}

} // namespace hailstone::epic
