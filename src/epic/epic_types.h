#pragma once

// Core types for Hailstone's native, character-aware epic quest and loot
// management system. Zero MacroQuest dependencies.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::epic {

inline constexpr int kEpicSchemaVersion = 1;

enum class EpicProgressState : std::uint8_t {
    Unknown = 0,
    NotStarted,
    InProgress,
    AwaitingItem,
    AwaitingTurnIn,
    ReadyForCompletion,
    Completed,
    CompletedWithUpgrade,
    Blocked,
    ManualOverride,
};

enum class LootDecisionOutcome : std::uint8_t {
    LootForCharacter = 0,
    LootForAnotherCharacter,
    LootAndAssignLater,
    LeaveForAnotherGroup,
    LeaveBecauseUnnecessary,
    DeferDecision,
    ManualReviewRequired,
    NeverLoot,
};

enum class ConflictResolutionRule : std::uint8_t {
    ActiveStageFirst = 0,
    HighestPriorityCharacter,
    MainCharacterFirst,
    HighestStageProgress,
    GroupPriorityThenCharacter,
    ClassPriority,
    ManualApproval,
};

enum class SignalSource : std::uint8_t {
    Unknown = 0,
    InventoryScan,
    EquippedScan,
    QuestState,
    ManualEntry,
    ManualOverride,
    Imported,
    Reconciliation,
    PeerBus,
};

enum class ConfidenceLevel : std::uint8_t {
    None = 0,
    Low,
    Medium,
    High,
    Confirmed,
};

const char* progress_state_name(EpicProgressState s) noexcept;
EpicProgressState parse_progress_state(std::string_view s, EpicProgressState fallback = EpicProgressState::Unknown) noexcept;

const char* loot_outcome_name(LootDecisionOutcome o) noexcept;
LootDecisionOutcome parse_loot_outcome(std::string_view s, LootDecisionOutcome fallback = LootDecisionOutcome::DeferDecision) noexcept;

const char* conflict_rule_name(ConflictResolutionRule r) noexcept;
ConflictResolutionRule parse_conflict_rule(std::string_view s, ConflictResolutionRule fallback = ConflictResolutionRule::ActiveStageFirst) noexcept;

const char* signal_source_name(SignalSource src) noexcept;
SignalSource parse_signal_source(std::string_view s, SignalSource fallback = SignalSource::Unknown) noexcept;

const char* confidence_level_name(ConfidenceLevel c) noexcept;
ConfidenceLevel parse_confidence_level(std::string_view s, ConfidenceLevel fallback = ConfidenceLevel::None) noexcept;

// Composite identity key: never key on character name alone.
struct CharacterEpicKey {
    std::string server;
    std::string character;
    std::string class_short;
    std::string epic_variant;

    std::string storage_key() const;
    bool matches(const CharacterEpicKey& other) const;
};

struct EpicItemRequirement {
    std::uint32_t item_id = 0;
    std::string item_name;
    int quantity = 1;
    std::vector<std::uint32_t> alternative_item_ids;
    std::vector<std::string> alternative_item_names;
    bool no_drop = false;
    bool lore = false;
    bool tradeable = true;
    bool temporary = false;
    bool quest_turn_in = false;
    bool optional = false;
    bool obsolete_after_completion = true;
    bool useful_for_others = false;
    std::string source_zone;
    std::string source_npc;
    std::string notes;
};

struct EpicStageDefinition {
    std::string stage_id;
    int order = 1;
    std::string name;
    std::string description;
    std::vector<std::string> prerequisites;
    std::vector<std::string> required_npcs;
    std::vector<std::string> required_zones;
    std::vector<EpicItemRequirement> required_items;
    std::vector<std::string> turn_in_npcs;
    std::vector<std::string> turn_in_items;
    std::vector<std::uint32_t> completion_reward_ids;
    std::vector<std::string> completion_reward_names;
    bool is_completion_stage = false;
};

struct ServerRuleOverride {
    std::string server_pattern; // e.g. "ez", "ez-server", "*"
    std::string notes;
    std::vector<std::string> skip_stages;
    std::vector<std::uint32_t> extra_completion_item_ids;
    std::vector<std::string> extra_completion_item_names;
};

struct EpicDefinition {
    std::string epic_id;          // e.g. "war_ez_v2", "clr_1_0"
    std::string family;           // e.g. "EZ_Server", "EQ1"
    std::string variant_name;     // e.g. "Variant 2", "Epic 1.5", "Epic 2.0"
    int version = 1;
    std::vector<std::string> class_restrictions; // empty = all; e.g. {"WAR"}
    std::vector<ServerRuleOverride> server_rules;
    std::vector<EpicStageDefinition> stages;
    std::vector<std::uint32_t> completion_item_ids;
    std::vector<std::string> completion_item_names;
    std::string upgrade_next_epic_id;
    std::vector<std::uint32_t> never_loot_after_completion_ids;
    std::vector<std::string> never_loot_after_completion_names;
    std::vector<std::uint32_t> shared_useful_item_ids;
    std::vector<std::string> shared_useful_item_names;

    bool supports_class(std::string_view class_short) const;
    bool supports_server(std::string_view server_name) const;
    const EpicStageDefinition* find_stage(std::string_view stage_id) const;
};

struct ItemOwnershipRecord {
    std::uint32_t item_id = 0;
    std::string item_name;
    int owned_qty = 0;
    int equipped_qty = 0;
    int bank_qty = 0;
    int turned_in_qty = 0;
    int required_qty = 0;
    int missing_qty = 0;
    bool optional = false;
    bool obsolete = false;
    std::string stage_id;
};

struct AuditEntry {
    std::uint64_t timestamp_ms = 0;
    std::string actor; // "system" or "user"
    std::string action;
    std::string previous_state;
    std::string new_state;
    std::string reason;
    SignalSource source = SignalSource::Unknown;
    ConfidenceLevel confidence = ConfidenceLevel::None;
    // Rollback snapshot fields for manual changes
    std::string previous_stage_id;
    std::vector<std::string> previous_completed_stages;
    bool previous_completed_epic = false;
    bool rollbackable = false;
};

struct CharacterPolicyOverride {
    bool has_override = false;
    bool epic_loot_enabled = true;
    bool loot_only_active_stage = false;
    bool loot_future_stage_items = true;
    bool stop_after_completion = true;
    bool allow_obsolete_items = false;
    bool require_confirm_no_drop = true;
    int priority = 50;
};

struct CharacterEpicProfile {
    std::string character;
    std::string class_short;
    std::string server;
    std::string group_name;
    std::string account; // non-secret account label only

    std::string epic_id;
    std::string epic_family;
    std::string epic_variant;

    EpicProgressState state = EpicProgressState::Unknown;
    std::string current_stage_id;
    std::vector<std::string> completed_stages;
    std::vector<ItemOwnershipRecord> required_items;
    std::vector<ItemOwnershipRecord> owned_items;
    std::vector<ItemOwnershipRecord> turned_in_items;
    std::vector<ItemOwnershipRecord> missing_items;
    std::vector<ItemOwnershipRecord> optional_or_obsolete_items;

    bool has_completed_epic = false;
    bool allow_future_epic_loot = true;
    bool needs_review = false;
    std::string review_reason;

    std::uint64_t last_scan_ms = 0;
    SignalSource source = SignalSource::Unknown;
    ConfidenceLevel confidence = ConfidenceLevel::None;
    std::vector<std::string> progress_reasons;

    bool manual_override_locked = false;
    CharacterPolicyOverride policy_override;
    std::vector<AuditEntry> audit_history;

    CharacterEpicKey key() const {
        return {server, character, class_short, epic_variant};
    }
    int total_stages = 0;
    double progress_percent() const;
};

struct GroupLootPolicy {
    std::string group_name;
    bool epic_loot_enabled = true;
    bool loot_only_active_stage = false;
    bool loot_future_stage_items = true;
    bool loot_for_other_characters = true;
    bool stop_after_completion = true;
    int priority = 50;
};

struct EpicLootPolicy {
    bool emergency_disable = false;
    bool epic_loot_enabled = true;
    bool dry_run = false;
    bool loot_only_active_stage = false;
    bool loot_future_stage_items = true;
    bool loot_for_other_characters = true;
    bool stop_after_completion = true;
    bool allow_obsolete_items = false;
    bool require_confirm_no_drop = true;
    bool allow_manual_conflict_approval = true;
    bool shared_raid_inventory_tracking = true;
    ConflictResolutionRule conflict_rule = ConflictResolutionRule::ActiveStageFirst;

    std::string main_character;
    std::map<std::string, int, std::less<>> character_priorities;
    std::map<std::string, int, std::less<>> group_priorities;
    std::map<std::string, int, std::less<>> class_priorities;
    std::map<std::string, std::string, std::less<>> reserved_items; // item key (id or lower name) -> character
    std::map<std::string, GroupLootPolicy, std::less<>> group_policies;
};

struct EpicDiagnosticEntry {
    std::uint64_t timestamp_ms = 0;
    std::string character;
    std::string class_short;
    std::string server;
    std::string group_name;
    std::string epic_id;
    std::string variant;
    std::string stage;
    std::uint32_t item_id = 0;
    std::string item_name;
    LootDecisionOutcome decision = LootDecisionOutcome::DeferDecision;
    std::string recipient;
    std::string reason;
    ConfidenceLevel confidence = ConfidenceLevel::None;
    SignalSource source = SignalSource::Unknown;
    bool success = true;
    bool dry_run = false;
};

struct ObservedItem {
    std::uint32_t item_id = 0;
    std::string item_name;
    int quantity = 1;
    bool equipped = false;
    bool in_bank = false;
    bool no_drop = false;
    bool lore = false;
    bool temporary = false;
};

struct CharacterInventorySnapshot {
    std::string server;
    std::string character;
    std::string class_short;
    std::string group_name;
    std::string account;
    bool online = true;
    std::uint64_t observed_ms = 0;
    std::vector<ObservedItem> items;
    std::vector<std::string> quest_flags;
};

} // namespace hailstone::epic
