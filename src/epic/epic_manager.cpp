#include "epic/epic_manager.h"

#include <algorithm>
#include <format>

#include "core/ascii_case.h"
#include "core/log.h"

namespace hailstone::epic {
namespace {

constexpr std::size_t kMaxDiagnosticEntries = 200;

} // namespace

EpicManager::EpicManager(std::filesystem::path data_dir) : data_dir_(std::move(data_dir)) {
    catalog_.load_builtin_defaults();
}

void EpicManager::set_data_dir(std::filesystem::path data_dir) {
    std::lock_guard<std::mutex> lock(mu_);
    data_dir_ = std::move(data_dir);
}

std::filesystem::path EpicManager::progress_db_path() const {
    if (data_dir_.empty()) return {};
    return data_dir_ / "epic_progress.json";
}

std::filesystem::path EpicManager::definitions_path() const {
    if (data_dir_.empty()) return {};
    return data_dir_ / "epic_quests.json";
}

bool EpicManager::initialize(std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    catalog_.load_builtin_defaults();

    const auto def_path = definitions_path();
    if (!def_path.empty() && std::filesystem::exists(def_path)) {
        std::string err;
        if (!catalog_.load_from_file(def_path, &err, true)) {
            log::warn("[epic] failed to load custom definitions {}: {}", def_path.string(), err);
            if (error) *error = err;
        }
    }

    const auto db_path = progress_db_path();
    if (!db_path.empty() && std::filesystem::exists(db_path)) {
        std::string err;
        if (!store_.load(db_path, &err)) {
            log::warn("[epic] failed to load progress database {}: {}", db_path.string(), err);
            if (error) *error = err;
            return false;
        }
        std::error_code ec;
        last_db_mtime_ = std::filesystem::last_write_time(db_path, ec);
    }
    return true;
}

void EpicManager::sync_from_disk_if_newer_unlocked() const {
    const auto db_path = progress_db_path();
    if (db_path.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(db_path, ec)) return;
    const auto mtime = std::filesystem::last_write_time(db_path, ec);
    if (ec || mtime == last_db_mtime_) return;
    EpicProgressStore disk;
    if (disk.load(db_path)) {
        for (const auto& p : disk.profiles()) {
            const auto* existing = store_.find_profile(p.key());
            if (!existing || p.last_scan_ms >= existing->last_scan_ms) {
                store_.upsert_profile(p);
            }
        }
        last_db_mtime_ = mtime;
    }
}

void EpicManager::save_unlocked() const {
    const auto db_path = progress_db_path();
    if (db_path.empty()) return;
    if (store_.save(db_path)) {
        std::error_code ec;
        last_db_mtime_ = std::filesystem::last_write_time(db_path, ec);
    }
}

bool EpicManager::save(std::string* error) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto db_path = progress_db_path();
    if (db_path.empty()) return true;
    const bool ok = store_.save(db_path, error);
    if (ok) {
        std::error_code ec;
        last_db_mtime_ = std::filesystem::last_write_time(db_path, ec);
    }
    return ok;
}

bool EpicManager::reload_definitions(std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    catalog_.load_builtin_defaults();
    const auto def_path = definitions_path();
    if (!def_path.empty() && std::filesystem::exists(def_path)) {
        return catalog_.load_from_file(def_path, error, true);
    }
    return true;
}

std::string EpicManager::resolve_epic_id_unlocked(std::string_view server,
                                                  std::string_view character,
                                                  std::string_view class_short,
                                                  std::string_view preferred_variant) const {
    if (!preferred_variant.empty()) {
        if (const auto* d = catalog_.find_for_character(class_short, preferred_variant, server)) {
            return d->epic_id;
        }
    }
    for (const auto& p : store_.profiles()) {
        if (iequals(p.server, server) && iequals(p.character, character) && !p.epic_id.empty()) {
            return p.epic_id;
        }
    }
    if (const auto* d = catalog_.find_for_character(class_short, "", server)) {
        return d->epic_id;
    }
    return {};
}

ReconciliationReport EpicManager::scan_character(const CharacterInventorySnapshot& snapshot,
                                                 std::string_view preferred_variant) {
    ReconciliationReport report;
    std::lock_guard<std::mutex> lock(mu_);
    sync_from_disk_if_newer_unlocked();

    const std::string epic_id = resolve_epic_id_unlocked(
        snapshot.server, snapshot.character, snapshot.class_short, preferred_variant);
    const EpicDefinition* def = catalog_.find_by_id(epic_id);
    if (!def) {
        report.warnings.push_back("No matching epic definition for class " + snapshot.class_short);
        return report;
    }

    const CharacterEpicKey key{snapshot.server, snapshot.character, snapshot.class_short, def->variant_name};
    CharacterEpicProfile* existing = store_.find_profile(key);
    CharacterEpicProfile working = existing ? *existing : CharacterEpicProfile{};
    report = ProgressDetector::scan_and_reconcile(*def, snapshot, working, &catalog_);
    store_.upsert_profile(std::move(working));

    save_unlocked();
    return report;
}

LootDecision EpicManager::evaluate_loot(const LootCandidate& item,
                                        const LootEvaluationContext& ctx,
                                        bool claim_if_looting) {
    std::lock_guard<std::mutex> lock(mu_);

    LootDecision decision;
    if (coordinator_.is_claimed_by_other(item.corpse_id, item.corpse_slot, ctx.looter_character, ctx.now_ms)) {
        decision.outcome = LootDecisionOutcome::DeferDecision;
        decision.reason = "Slot is already claimed or completed by another client.";
        decision.dry_run = store_.policy().dry_run;
        return decision;
    }

    decision = LootDecisionEngine::evaluate(item, ctx, store_.profiles(), catalog_, store_.policy());

    if (claim_if_looting && !decision.dry_run &&
        (decision.outcome == LootDecisionOutcome::LootForCharacter ||
         decision.outcome == LootDecisionOutcome::LootForAnotherCharacter ||
         decision.outcome == LootDecisionOutcome::LootAndAssignLater)) {
        if (!coordinator_.try_claim(item.corpse_id, item.corpse_slot, ctx.looter_character, ctx.now_ms)) {
            decision.outcome = LootDecisionOutcome::DeferDecision;
            decision.reason = "Failed to acquire corpse slot claim (concurrent claim).";
        }
    }

    EpicDiagnosticEntry diag;
    diag.timestamp_ms = ctx.now_ms;
    diag.character = ctx.looter_character;
    diag.class_short = ctx.looter_class;
    diag.server = ctx.looter_server;
    diag.group_name = ctx.looter_group;
    diag.epic_id = decision.epic_id;
    diag.variant = decision.variant;
    diag.stage = decision.stage_id;
    diag.item_id = item.item_id;
    diag.item_name = item.item_name;
    diag.decision = decision.outcome;
    diag.recipient = decision.recipient;
    diag.reason = decision.reason;
    diag.confidence = decision.confidence;
    diag.source = decision.source;
    diag.success = (decision.outcome != LootDecisionOutcome::ManualReviewRequired);
    diag.dry_run = decision.dry_run;
    record_diagnostic(std::move(diag));

    return decision;
}

void EpicManager::on_corpse_expired(std::uint32_t corpse_id) {
    std::lock_guard<std::mutex> lock(mu_);
    coordinator_.expire_corpse(corpse_id);
}

void EpicManager::on_character_disconnect_or_zone(std::string_view character) {
    std::lock_guard<std::mutex> lock(mu_);
    coordinator_.release_character(character);
}

void EpicManager::release_slot_claim(std::uint32_t corpse_id, std::int16_t corpse_slot, std::string_view character) {
    std::lock_guard<std::mutex> lock(mu_);
    coordinator_.release_claim(corpse_id, corpse_slot, character);
}

void EpicManager::mark_slot_looted(std::uint32_t corpse_id, std::int16_t corpse_slot, std::string_view character) {
    std::lock_guard<std::mutex> lock(mu_);
    coordinator_.mark_completed(corpse_id, corpse_slot, character);
}

bool EpicManager::mark_stage_complete(const CharacterEpicKey& key, std::string_view stage_id,
                                      std::uint64_t now_ms, std::string_view reason) {
    std::lock_guard<std::mutex> lock(mu_);
    CharacterEpicProfile* prof = store_.find_profile(key);
    if (!prof) return false;
    const EpicDefinition* def = catalog_.find_by_id(prof->epic_id);
    if (!def) return false;
    ProgressDetector::mark_stage_complete(*def, *prof, stage_id, now_ms, reason);
    if (!progress_db_path().empty()) store_.save(progress_db_path());
    return true;
}

bool EpicManager::set_manual_state(const CharacterEpicKey& key, EpicProgressState state, std::string_view stage_id,
                                   bool completed_epic, bool lock_override, std::uint64_t now_ms,
                                   std::string_view reason) {
    std::lock_guard<std::mutex> lock(mu_);
    CharacterEpicProfile* prof = store_.find_profile(key);
    if (!prof) return false;
    ProgressDetector::set_manual_state(*prof, state, stage_id, completed_epic, lock_override, now_ms, reason);
    if (!progress_db_path().empty()) store_.save(progress_db_path());
    return true;
}

bool EpicManager::set_character_variant(std::string_view server, std::string_view character,
                                        std::string_view class_short, std::string_view new_variant,
                                        std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    const EpicDefinition* def = catalog_.find_for_character(class_short, new_variant, server);
    if (!def) return false;

    CharacterEpicProfile prof;
    prof.server = std::string(server);
    prof.character = std::string(character);
    prof.class_short = std::string(class_short);
    prof.epic_id = def->epic_id;
    prof.epic_family = def->family;
    prof.epic_variant = def->variant_name;
    prof.total_stages = static_cast<int>(def->stages.size());
    if (!def->stages.empty()) prof.current_stage_id = def->stages.front().stage_id;
    prof.state = EpicProgressState::NotStarted;
    prof.last_scan_ms = now_ms;
    prof.source = SignalSource::ManualEntry;
    prof.confidence = ConfidenceLevel::Medium;
    store_.upsert_profile(std::move(prof));
    if (!progress_db_path().empty()) store_.save(progress_db_path());
    return true;
}

bool EpicManager::rollback_manual_change(const CharacterEpicKey& key, std::uint64_t now_ms, std::string* msg) {
    std::lock_guard<std::mutex> lock(mu_);
    CharacterEpicProfile* prof = store_.find_profile(key);
    if (!prof) return false;
    const bool ok = EpicProgressStore::rollback_last_manual_change(*prof, now_ms, msg);
    if (ok && !progress_db_path().empty()) store_.save(progress_db_path());
    return ok;
}

bool EpicManager::clear_review_flag(const CharacterEpicKey& key, std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    CharacterEpicProfile* prof = store_.find_profile(key);
    if (!prof) return false;
    prof->needs_review = false;
    prof->review_reason.clear();
    AuditEntry entry;
    entry.timestamp_ms = now_ms;
    entry.actor = "user";
    entry.action = "clear_review_flag";
    entry.previous_state = progress_state_name(prof->state);
    entry.new_state = entry.previous_state;
    entry.reason = "Review flag cleared by user";
    entry.source = SignalSource::ManualEntry;
    entry.confidence = prof->confidence;
    EpicProgressStore::append_audit(*prof, std::move(entry));
    if (!progress_db_path().empty()) store_.save(progress_db_path());
    return true;
}

EpicLootPolicy EpicManager::policy() const {
    std::lock_guard<std::mutex> lock(mu_);
    return store_.policy();
}

void EpicManager::set_policy(EpicLootPolicy policy) {
    std::lock_guard<std::mutex> lock(mu_);
    store_.mutable_policy() = std::move(policy);
    if (!progress_db_path().empty()) store_.save(progress_db_path());
}

void EpicManager::set_emergency_disable(bool disabled) {
    std::lock_guard<std::mutex> lock(mu_);
    store_.mutable_policy().emergency_disable = disabled;
    if (!progress_db_path().empty()) store_.save(progress_db_path());
}

void EpicManager::set_dry_run(bool dry_run) {
    std::lock_guard<std::mutex> lock(mu_);
    store_.mutable_policy().dry_run = dry_run;
    if (!progress_db_path().empty()) store_.save(progress_db_path());
}

void EpicManager::set_character_priority(std::string_view character, int priority) {
    std::lock_guard<std::mutex> lock(mu_);
    store_.mutable_policy().character_priorities[std::string(character)] = priority;
    if (!progress_db_path().empty()) store_.save(progress_db_path());
}

void EpicManager::set_reserved_item(std::string_view item_key, std::string_view character) {
    std::lock_guard<std::mutex> lock(mu_);
    store_.mutable_policy().reserved_items[std::string(item_key)] = std::string(character);
    if (!progress_db_path().empty()) store_.save(progress_db_path());
}

void EpicManager::clear_reserved_item(std::string_view item_key) {
    std::lock_guard<std::mutex> lock(mu_);
    store_.mutable_policy().reserved_items.erase(std::string(item_key));
    if (!progress_db_path().empty()) store_.save(progress_db_path());
}

std::string EpicManager::export_database_json() const {
    std::lock_guard<std::mutex> lock(mu_);
    return store_.export_json();
}

bool EpicManager::import_database_json(std::string_view json_text, bool merge, std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    const bool ok = store_.import_json(json_text, error, merge);
    if (ok && !progress_db_path().empty()) store_.save(progress_db_path());
    return ok;
}

bool EpicManager::backup_database(const std::filesystem::path& path, std::string* error) const {
    std::lock_guard<std::mutex> lock(mu_);
    return store_.backup_to(path, error);
}

bool EpicManager::restore_database(const std::filesystem::path& path, std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    const bool ok = store_.restore_from(path, error);
    if (ok && !progress_db_path().empty()) store_.save(progress_db_path());
    return ok;
}

std::vector<CharacterEpicProfile> EpicManager::profiles_snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    sync_from_disk_if_newer_unlocked();
    return store_.profiles();
}

std::vector<EpicDefinition> EpicManager::definitions_snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return catalog_.all();
}

std::vector<LootDecisionEngine::ItemNeedSummary> EpicManager::item_needs_snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    sync_from_disk_if_newer_unlocked();
    return LootDecisionEngine::summarize_item_needs(store_.profiles(), catalog_, store_.policy());
}

std::vector<EpicDiagnosticEntry> EpicManager::diagnostics_snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return {diagnostics_.begin(), diagnostics_.end()};
}

void EpicManager::clear_diagnostics() {
    std::lock_guard<std::mutex> lock(mu_);
    diagnostics_.clear();
}

void EpicManager::record_diagnostic(EpicDiagnosticEntry entry) {
    log::info("[epic_loot] char={} class={} server={} group={} epic={} variant={} stage={} "
              "item_id={} item=\"{}\" decision=\"{}\" recipient=\"{}\" confidence={} source={} dry_run={} reason=\"{}\"",
              entry.character, entry.class_short, entry.server, entry.group_name,
              entry.epic_id, entry.variant, entry.stage,
              entry.item_id, entry.item_name,
              loot_outcome_name(entry.decision), entry.recipient,
              confidence_level_name(entry.confidence), signal_source_name(entry.source),
              entry.dry_run ? "true" : "false", entry.reason);

    diagnostics_.push_back(std::move(entry));
    while (diagnostics_.size() > kMaxDiagnosticEntries) {
        diagnostics_.pop_front();
    }
}

} // namespace hailstone::epic
