#include "epic/progress_detector.h"

#include <algorithm>
#include <format>
#include <map>

#include "core/ascii_case.h"
#include "epic/epic_store.h"

namespace hailstone::epic {
namespace {

void collect_item(const net::Item& item, bool equipped, bool in_bank, std::vector<ObservedItem>& out) {
    if (item.id == 0 && item.name.empty()) return;
    ObservedItem obs;
    obs.item_id = item.id;
    obs.item_name = item.name;
    obs.quantity = static_cast<int>(std::max<std::uint32_t>(1, item.stack_count));
    obs.equipped = equipped;
    obs.in_bank = in_bank;
    obs.no_drop = item.no_trade();
    obs.lore = item.is_lore();
    obs.temporary = (item.no_rent == 0);
    out.push_back(std::move(obs));
    for (const auto& bi : item.contents) {
        collect_item(bi.item, false, in_bank, out);
    }
}

int count_matching(const std::vector<ObservedItem>& items, const EpicItemRequirement& req,
                   int* equipped_out = nullptr, int* bank_out = nullptr) {
    int total = 0;
    int eq = 0;
    int bk = 0;
    for (const auto& it : items) {
        if (ProgressDetector::matches_requirement(it, req)) {
            total += it.quantity;
            if (it.equipped) eq += it.quantity;
            if (it.in_bank) bk += it.quantity;
        }
    }
    if (equipped_out) *equipped_out = eq;
    if (bank_out) *bank_out = bk;
    return total;
}

bool contains_ci(const std::vector<std::string>& list, std::string_view val) {
    for (const auto& s : list) {
        if (iequals(s, val)) return true;
    }
    return false;
}

void add_unique_ci(std::vector<std::string>& list, std::string_view val) {
    if (!val.empty() && !contains_ci(list, val)) {
        list.emplace_back(val);
    }
}

} // namespace

CharacterInventorySnapshot snapshot_from_inventory(
    const game::Inventory& inv,
    std::string_view server,
    std::string_view character,
    std::string_view class_short,
    std::string_view group_name,
    std::string_view account,
    std::uint64_t now_ms,
    const std::vector<std::string>& quest_flags) {
    CharacterInventorySnapshot snap;
    snap.server = std::string(server);
    snap.character = std::string(character);
    snap.class_short = std::string(class_short);
    snap.group_name = std::string(group_name);
    snap.account = std::string(account);
    snap.observed_ms = now_ms;
    snap.quest_flags = quest_flags;

    for (const auto& [key, item] : inv.items()) {
        const auto type = key.first;
        const auto slot = key.second;
        const bool equipped = (type == net::kInvPossessions && slot >= 0 && slot < net::kEquipmentSlots);
        const bool in_bank = (type == net::kInvBank || type == net::kInvSharedBank);
        collect_item(item, equipped, in_bank, snap.items);
    }
    return snap;
}

bool ProgressDetector::matches_item_id_or_name(std::uint32_t id, std::string_view name,
                                               const std::vector<std::uint32_t>& ids,
                                               const std::vector<std::string>& names) {
    if (id != 0) {
        for (std::uint32_t target_id : ids) {
            if (target_id != 0 && target_id == id) return true;
        }
    }
    if (!name.empty()) {
        for (const auto& target_name : names) {
            if (!target_name.empty() && iequals(target_name, name)) return true;
        }
    }
    return false;
}

bool ProgressDetector::matches_requirement(const ObservedItem& item, const EpicItemRequirement& req) {
    if (item.item_id != 0 && req.item_id != 0 && item.item_id == req.item_id) return true;
    if (!item.item_name.empty() && !req.item_name.empty() && iequals(item.item_name, req.item_name)) return true;
    if (matches_item_id_or_name(item.item_id, item.item_name, req.alternative_item_ids, req.alternative_item_names)) {
        return true;
    }
    return false;
}

ReconciliationReport ProgressDetector::scan_and_reconcile(
    const EpicDefinition& def,
    const CharacterInventorySnapshot& snapshot,
    CharacterEpicProfile& profile,
    const EpicDefinitionCatalog* catalog) {
    ReconciliationReport report;
    const bool first_scan = (profile.last_scan_ms == 0);
    const EpicProgressState prev_state = profile.state;

    // Update identity metadata
    profile.server = snapshot.server;
    profile.character = snapshot.character;
    if (!snapshot.class_short.empty()) profile.class_short = snapshot.class_short;
    if (!snapshot.group_name.empty()) profile.group_name = snapshot.group_name;
    if (!snapshot.account.empty()) profile.account = snapshot.account;
    profile.epic_id = def.epic_id;
    profile.epic_family = def.family;
    profile.epic_variant = def.variant_name;
    profile.total_stages = static_cast<int>(def.stages.size());
    profile.last_scan_ms = snapshot.observed_ms;

    // Map previous owned quantities for reconciliation diffing
    std::map<std::string, int, std::less<>> prev_owned_by_name;
    for (const auto& rec : profile.owned_items) {
        prev_owned_by_name[rec.item_name] = rec.owned_qty;
    }

    // Check completion item presence
    bool has_completion_item = false;
    bool completion_equipped = false;
    std::string completion_item_found;
    for (const auto& it : snapshot.items) {
        if (matches_item_id_or_name(it.item_id, it.item_name, def.completion_item_ids, def.completion_item_names)) {
            has_completion_item = true;
            if (it.equipped) completion_equipped = true;
            completion_item_found = it.item_name;
            if (!first_scan) {
                report.has_changes = true;
                report.events.push_back({
                    ReconciliationEventKind::CompletionItemDetected,
                    it.item_id,
                    it.item_name,
                    0,
                    it.quantity,
                    std::format("Detected epic completion item '{}'", it.item_name),
                });
            }
            break;
        }
    }

    // Rebuild item ownership lists across all stages
    std::vector<ItemOwnershipRecord> new_required;
    std::vector<ItemOwnershipRecord> new_owned;
    std::vector<ItemOwnershipRecord> new_missing;
    std::vector<ItemOwnershipRecord> new_obsolete;
    std::vector<std::string> reasons;

    bool any_stage_item_owned = false;
    for (const auto& st : def.stages) {
        const bool stage_done = contains_ci(profile.completed_stages, st.stage_id);
        for (const auto& req : st.required_items) {
            int eq_qty = 0;
            int bk_qty = 0;
            const int qty = count_matching(snapshot.items, req, &eq_qty, &bk_qty);

            ItemOwnershipRecord rec;
            rec.item_id = req.item_id;
            rec.item_name = req.item_name;
            rec.owned_qty = qty;
            rec.equipped_qty = eq_qty;
            rec.bank_qty = bk_qty;
            rec.required_qty = req.quantity;
            rec.missing_qty = std::max(0, req.quantity - qty);
            rec.optional = req.optional;
            rec.obsolete = (profile.has_completed_epic && req.obsolete_after_completion);
            rec.stage_id = st.stage_id;

            new_required.push_back(rec);
            if (qty > 0) {
                any_stage_item_owned = true;
                new_owned.push_back(rec);
                if (qty > req.quantity) {
                    report.events.push_back({
                        ReconciliationEventKind::DuplicateQuantity,
                        req.item_id,
                        req.item_name,
                        req.quantity,
                        qty,
                        std::format("Duplicate quantity for '{}': owns {} (requires {})",
                                    req.item_name, qty, req.quantity),
                    });
                }
            }
            if (rec.missing_qty > 0 && !stage_done && !rec.optional && !profile.has_completed_epic) {
                new_missing.push_back(rec);
            }
            if (rec.obsolete) {
                new_obsolete.push_back(rec);
            }

            // Reconcile against previous scan
            if (!first_scan) {
                const auto pit = prev_owned_by_name.find(req.item_name);
                const int prev_q = (pit != prev_owned_by_name.end()) ? pit->second : 0;
                if (qty > prev_q) {
                    report.has_changes = true;
                    report.events.push_back({
                        ReconciliationEventKind::ItemAcquired,
                        req.item_id,
                        req.item_name,
                        prev_q,
                        qty,
                        std::format("Acquired '{}' ({} -> {})", req.item_name, prev_q, qty),
                    });
                } else if (qty < prev_q) {
                    report.has_changes = true;
                    if (stage_done || profile.has_completed_epic) {
                        report.events.push_back({
                            ReconciliationEventKind::ItemConsumedOrTurnedIn,
                            req.item_id,
                            req.item_name,
                            prev_q,
                            qty,
                            std::format("Item '{}' consumed/turned in ({} -> {})", req.item_name, prev_q, qty),
                        });
                    } else {
                        report.inconsistency_detected = true;
                        std::string warn = std::format(
                            "Item '{}' missing from inventory ({} -> {}) without stage '{}' marked complete",
                            req.item_name, prev_q, qty, st.stage_id);
                        report.warnings.push_back(warn);
                        report.events.push_back({
                            ReconciliationEventKind::ItemMissingUnexpectedly,
                            req.item_id,
                            req.item_name,
                            prev_q,
                            qty,
                            warn,
                        });
                    }
                }
            }
        }
    }

    profile.required_items = std::move(new_required);
    profile.owned_items = std::move(new_owned);
    profile.missing_items = std::move(new_missing);
    profile.optional_or_obsolete_items = std::move(new_obsolete);

    // Check upgrade availability if completed
    bool upgrade_available = false;
    if (!def.upgrade_next_epic_id.empty() && catalog != nullptr) {
        if (const auto* next_def = catalog->find_by_id(def.upgrade_next_epic_id)) {
            (void)next_def;
            upgrade_available = true;
        }
    }

    // If manually locked, preserve state and stage, but still flag inconsistencies for review.
    if (profile.manual_override_locked) {
        if (report.inconsistency_detected && !report.warnings.empty()) {
            profile.needs_review = true;
            profile.review_reason = report.warnings.front();
        }
        profile.source = SignalSource::ManualOverride;
        return report;
    }

    // Conservative progress inference
    if (has_completion_item) {
        const bool all_prior_done = def.stages.empty() ||
                                    profile.completed_stages.size() >= def.stages.size() ||
                                    contains_ci(snapshot.quest_flags, def.epic_id + "_complete");
        if (all_prior_done) {
            profile.has_completed_epic = true;
            profile.state = upgrade_available ? EpicProgressState::CompletedWithUpgrade
                                              : EpicProgressState::Completed;
            profile.confidence = completion_equipped ? ConfidenceLevel::Confirmed : ConfidenceLevel::High;
            profile.source = completion_equipped ? SignalSource::EquippedScan : SignalSource::InventoryScan;
            reasons.push_back(std::format("Completion item '{}' present with verified stage history", completion_item_found));
        } else {
            // Never permanently assume completion from a single uncertain signal!
            profile.has_completed_epic = false;
            profile.needs_review = true;
            profile.review_reason = std::format(
                "{} has epic completion item '{}', but the quest stage is unknown or earlier stages are unverified.",
                profile.character, completion_item_found);
            if (profile.state == EpicProgressState::Unknown || profile.state == EpicProgressState::NotStarted) {
                profile.state = EpicProgressState::Unknown;
            }
            profile.confidence = ConfidenceLevel::Low;
            profile.source = completion_equipped ? SignalSource::EquippedScan : SignalSource::InventoryScan;
            reasons.push_back(profile.review_reason);
        }
    } else if (profile.has_completed_epic) {
        // Previously marked complete, now completion item is not in possessions/bank
        if (!first_scan) {
            report.inconsistency_detected = true;
            profile.needs_review = true;
            profile.review_reason = "Profile was marked Completed, but epic completion item was not found in live inventory.";
            report.warnings.push_back(profile.review_reason);
        }
        profile.state = upgrade_available ? EpicProgressState::CompletedWithUpgrade
                                          : EpicProgressState::Completed;
        reasons.push_back("Previously marked completed; completion item not currently visible in scan");
    } else {
        // Determine active stage from first incomplete stage
        const EpicStageDefinition* active_stage = nullptr;
        for (const auto& st : def.stages) {
            if (!contains_ci(profile.completed_stages, st.stage_id)) {
                active_stage = &st;
                break;
            }
        }
        if (!active_stage && !def.stages.empty()) {
            profile.state = EpicProgressState::ReadyForCompletion;
            profile.current_stage_id = def.stages.back().stage_id;
            profile.confidence = ConfidenceLevel::High;
            reasons.push_back("All stages marked complete; ready for final epic completion");
        } else if (active_stage) {
            profile.current_stage_id = active_stage->stage_id;
            bool stage_has_all_items = !active_stage->required_items.empty();
            for (const auto& req : active_stage->required_items) {
                if (req.optional) continue;
                if (count_matching(snapshot.items, req) < req.quantity) {
                    stage_has_all_items = false;
                    break;
                }
            }
            if (stage_has_all_items) {
                profile.state = active_stage->is_completion_stage
                                    ? EpicProgressState::ReadyForCompletion
                                    : EpicProgressState::AwaitingTurnIn;
                profile.confidence = ConfidenceLevel::High;
                profile.source = SignalSource::InventoryScan;
                reasons.push_back(std::format("All required items owned for stage '{}'", active_stage->stage_id));
            } else if (any_stage_item_owned || !profile.completed_stages.empty()) {
                profile.state = EpicProgressState::AwaitingItem;
                profile.confidence = ConfidenceLevel::Medium;
                profile.source = SignalSource::InventoryScan;
                reasons.push_back(std::format("Awaiting item(s) for stage '{}'", active_stage->stage_id));
            } else if (contains_ci(snapshot.quest_flags, def.epic_id + "_started")) {
                profile.state = EpicProgressState::InProgress;
                profile.confidence = ConfidenceLevel::Medium;
                profile.source = SignalSource::QuestState;
                reasons.push_back("Quest flag indicates epic is in progress");
            } else {
                profile.state = EpicProgressState::NotStarted;
                profile.confidence = ConfidenceLevel::Medium;
                profile.source = SignalSource::InventoryScan;
                reasons.push_back("No epic quest items or completed stages detected");
            }
        }
    }

    if (report.inconsistency_detected && !report.warnings.empty()) {
        profile.needs_review = true;
        if (profile.review_reason.empty()) profile.review_reason = report.warnings.front();
    }

    profile.progress_reasons = std::move(reasons);

    if (prev_state != profile.state && !first_scan) {
        AuditEntry entry;
        entry.timestamp_ms = snapshot.observed_ms;
        entry.actor = "system";
        entry.action = "scan_and_reconcile";
        entry.previous_state = progress_state_name(prev_state);
        entry.new_state = progress_state_name(profile.state);
        entry.reason = profile.progress_reasons.empty() ? "Inventory scan" : profile.progress_reasons.front();
        entry.source = profile.source;
        entry.confidence = profile.confidence;
        EpicProgressStore::append_audit(profile, std::move(entry));
    }

    return report;
}

void ProgressDetector::mark_stage_complete(const EpicDefinition& def,
                                           CharacterEpicProfile& profile,
                                           std::string_view stage_id,
                                           std::uint64_t now_ms,
                                           std::string_view reason) {
    AuditEntry entry;
    entry.timestamp_ms = now_ms;
    entry.actor = "user";
    entry.action = std::format("mark_stage_complete:{}", stage_id);
    entry.previous_state = progress_state_name(profile.state);
    entry.previous_stage_id = profile.current_stage_id;
    entry.previous_completed_stages = profile.completed_stages;
    entry.previous_completed_epic = profile.has_completed_epic;
    entry.rollbackable = true;
    entry.source = SignalSource::ManualEntry;
    entry.confidence = ConfidenceLevel::Confirmed;
    entry.reason = std::string(reason);

    add_unique_ci(profile.completed_stages, stage_id);

    const EpicStageDefinition* st = def.find_stage(stage_id);
    if (st && st->is_completion_stage) {
        profile.has_completed_epic = true;
        profile.state = EpicProgressState::Completed;
    } else {
        // Advance current_stage_id to next incomplete stage
        bool found_next = false;
        for (const auto& s : def.stages) {
            if (!contains_ci(profile.completed_stages, s.stage_id)) {
                profile.current_stage_id = s.stage_id;
                profile.state = EpicProgressState::InProgress;
                found_next = true;
                break;
            }
        }
        if (!found_next) {
            profile.state = EpicProgressState::ReadyForCompletion;
        }
    }
    profile.needs_review = false;
    profile.review_reason.clear();
    profile.source = SignalSource::ManualEntry;
    profile.confidence = ConfidenceLevel::Confirmed;
    profile.progress_reasons = {std::string(reason)};
    entry.new_state = progress_state_name(profile.state);
    EpicProgressStore::append_audit(profile, std::move(entry));
}

void ProgressDetector::set_manual_state(CharacterEpicProfile& profile,
                                        EpicProgressState state,
                                        std::string_view stage_id,
                                        bool completed_epic,
                                        bool lock_override,
                                        std::uint64_t now_ms,
                                        std::string_view reason) {
    AuditEntry entry;
    entry.timestamp_ms = now_ms;
    entry.actor = "user";
    entry.action = "manual_override";
    entry.previous_state = progress_state_name(profile.state);
    entry.previous_stage_id = profile.current_stage_id;
    entry.previous_completed_stages = profile.completed_stages;
    entry.previous_completed_epic = profile.has_completed_epic;
    entry.rollbackable = true;
    entry.source = SignalSource::ManualOverride;
    entry.confidence = ConfidenceLevel::Confirmed;
    entry.reason = std::string(reason);

    profile.state = state;
    if (!stage_id.empty()) profile.current_stage_id = std::string(stage_id);
    profile.has_completed_epic = completed_epic;
    profile.manual_override_locked = lock_override;
    profile.needs_review = false;
    profile.review_reason.clear();
    profile.source = SignalSource::ManualOverride;
    profile.confidence = ConfidenceLevel::Confirmed;
    profile.progress_reasons = {std::string(reason)};
    entry.new_state = progress_state_name(profile.state);
    EpicProgressStore::append_audit(profile, std::move(entry));
}

} // namespace hailstone::epic
