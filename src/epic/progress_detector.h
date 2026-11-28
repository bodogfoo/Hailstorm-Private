#pragma once

// Progress detection and live inventory reconciliation for epic quests.
// Conservative: never assumes completion from a single uncertain signal,
// records uncertainty and reasons, and flags inconsistencies for review.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "epic/epic_definitions.h"
#include "epic/epic_types.h"
#include "game/inventory.h"

namespace hailstone::epic {

enum class ReconciliationEventKind : std::uint8_t {
    ItemAcquired = 0,
    ItemConsumedOrTurnedIn,
    ItemMissingUnexpectedly,
    DuplicateQuantity,
    QuantityChanged,
    CompletionItemDetected,
    UpgradeItemDetected,
};

struct ReconciliationEvent {
    ReconciliationEventKind kind = ReconciliationEventKind::ItemAcquired;
    std::uint32_t item_id = 0;
    std::string item_name;
    int previous_qty = 0;
    int current_qty = 0;
    std::string detail;
};

struct ReconciliationReport {
    bool has_changes = false;
    bool inconsistency_detected = false;
    std::vector<ReconciliationEvent> events;
    std::vector<std::string> warnings;
};

// Build a CharacterInventorySnapshot from native game::Inventory.
CharacterInventorySnapshot snapshot_from_inventory(
    const game::Inventory& inv,
    std::string_view server,
    std::string_view character,
    std::string_view class_short,
    std::string_view group_name,
    std::string_view account,
    std::uint64_t now_ms,
    const std::vector<std::string>& quest_flags = {});

class ProgressDetector {
public:
    // Scan a character's live inventory/quest state against an EpicDefinition and
    // update (or initialize) their CharacterEpicProfile.
    static ReconciliationReport scan_and_reconcile(
        const EpicDefinition& def,
        const CharacterInventorySnapshot& snapshot,
        CharacterEpicProfile& profile,
        const EpicDefinitionCatalog* catalog = nullptr);

    // Manual progress controls (always recorded in audit_history with rollback support).
    static void mark_stage_complete(const EpicDefinition& def,
                                    CharacterEpicProfile& profile,
                                    std::string_view stage_id,
                                    std::uint64_t now_ms,
                                    std::string_view reason = "Marked complete by user");

    static void set_manual_state(CharacterEpicProfile& profile,
                                 EpicProgressState state,
                                 std::string_view stage_id,
                                 bool completed_epic,
                                 bool lock_override,
                                 std::uint64_t now_ms,
                                 std::string_view reason);

    // Helper: check if an ObservedItem matches an EpicItemRequirement (by ID, name, or alternatives).
    static bool matches_requirement(const ObservedItem& item, const EpicItemRequirement& req);
    static bool matches_item_id_or_name(std::uint32_t id, std::string_view name,
                                        const std::vector<std::uint32_t>& ids,
                                        const std::vector<std::string>& names);
};

} // namespace hailstone::epic
