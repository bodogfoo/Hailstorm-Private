#pragma once

// Thread-safe coordinator for Hailstone's epic quest and loot management system.
// Owns the definition catalog, persistent progress store, corpse claim coordinator,
// structured diagnostics buffer, and policy persistence.

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "epic/epic_definitions.h"
#include "epic/epic_store.h"
#include "epic/epic_types.h"
#include "epic/loot_decision_engine.h"
#include "epic/progress_detector.h"

namespace hailstone::epic {

class EpicManager {
public:
    explicit EpicManager(std::filesystem::path data_dir = {});

    void set_data_dir(std::filesystem::path data_dir);
    const std::filesystem::path& data_dir() const { return data_dir_; }
    std::filesystem::path progress_db_path() const;
    std::filesystem::path definitions_path() const;

    bool initialize(std::string* error = nullptr);
    bool save(std::string* error = nullptr) const;
    bool reload_definitions(std::string* error = nullptr);

    // Scan & reconcile a character's inventory against their assigned (or default) epic variant.
    ReconciliationReport scan_character(const CharacterInventorySnapshot& snapshot,
                                        std::string_view preferred_variant = {});

    // Evaluate a corpse item and optionally claim the slot if looting.
    LootDecision evaluate_loot(const LootCandidate& item,
                               const LootEvaluationContext& ctx,
                               bool claim_if_looting = false);

    // Corpse coordination hooks (zone change, corpse decay, disconnect, death).
    void on_corpse_expired(std::uint32_t corpse_id);
    void on_character_disconnect_or_zone(std::string_view character);
    void release_slot_claim(std::uint32_t corpse_id, std::int16_t corpse_slot, std::string_view character);
    void mark_slot_looted(std::uint32_t corpse_id, std::int16_t corpse_slot, std::string_view character);

    // Manual override & UI command helpers
    bool mark_stage_complete(const CharacterEpicKey& key, std::string_view stage_id, std::uint64_t now_ms,
                             std::string_view reason = "Marked complete via UI");
    bool set_manual_state(const CharacterEpicKey& key, EpicProgressState state, std::string_view stage_id,
                          bool completed_epic, bool lock_override, std::uint64_t now_ms, std::string_view reason);
    bool set_character_variant(std::string_view server, std::string_view character, std::string_view class_short,
                               std::string_view new_variant, std::uint64_t now_ms);
    bool rollback_manual_change(const CharacterEpicKey& key, std::uint64_t now_ms, std::string* msg = nullptr);
    bool clear_review_flag(const CharacterEpicKey& key, std::uint64_t now_ms);

    // Policy mutations (persist immediately)
    EpicLootPolicy policy() const;
    void set_policy(EpicLootPolicy policy);
    void set_emergency_disable(bool disabled);
    void set_dry_run(bool dry_run);
    void set_character_priority(std::string_view character, int priority);
    void set_reserved_item(std::string_view item_key, std::string_view character);
    void clear_reserved_item(std::string_view item_key);

    // Import/Export/Backup/Restore
    std::string export_database_json() const;
    bool import_database_json(std::string_view json_text, bool merge, std::string* error = nullptr);
    bool backup_database(const std::filesystem::path& path, std::string* error = nullptr) const;
    bool restore_database(const std::filesystem::path& path, std::string* error = nullptr);

    // Read-only snapshots for UI & diagnostics
    std::vector<CharacterEpicProfile> profiles_snapshot() const;
    std::vector<EpicDefinition> definitions_snapshot() const;
    std::vector<LootDecisionEngine::ItemNeedSummary> item_needs_snapshot() const;
    std::vector<EpicDiagnosticEntry> diagnostics_snapshot() const;
    void clear_diagnostics();

private:
    void record_diagnostic(EpicDiagnosticEntry entry);
    std::string resolve_epic_id_unlocked(std::string_view server, std::string_view character,
                                         std::string_view class_short, std::string_view preferred_variant) const;

    mutable std::mutex mu_;
    std::filesystem::path data_dir_;
    EpicDefinitionCatalog catalog_;
    EpicProgressStore store_;
    CorpseLootCoordinator coordinator_;
    std::deque<EpicDiagnosticEntry> diagnostics_;
};

} // namespace hailstone::epic
