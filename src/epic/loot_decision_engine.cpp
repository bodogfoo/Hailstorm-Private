#include "epic/loot_decision_engine.h"

#include <algorithm>
#include <format>

#include "core/ascii_case.h"
#include "epic/progress_detector.h"

namespace hailstone::epic {
namespace {

bool item_matches_req(const LootCandidate& item, const EpicItemRequirement& req) {
    if (item.item_id != 0 && req.item_id != 0 && item.item_id == req.item_id) return true;
    if (!item.item_name.empty() && !req.item_name.empty() && iequals(item.item_name, req.item_name)) return true;
    return ProgressDetector::matches_item_id_or_name(item.item_id, item.item_name,
                                                     req.alternative_item_ids, req.alternative_item_names);
}

bool is_reserved_for(const EpicLootPolicy& policy, const LootCandidate& item, std::string& out_char) {
    if (item.item_id != 0) {
        auto it = policy.reserved_items.find(std::to_string(item.item_id));
        if (it != policy.reserved_items.end() && !it->second.empty()) {
            out_char = it->second;
            return true;
        }
    }
    for (const auto& [k, v] : policy.reserved_items) {
        if (!item.item_name.empty() && iequals(k, item.item_name) && !v.empty()) {
            out_char = v;
            return true;
        }
    }
    return false;
}

int compute_priority(const CharacterEpicProfile& prof, const EpicLootPolicy& policy) {
    int prio = prof.policy_override.has_override ? prof.policy_override.priority : 50;
    if (auto it = policy.character_priorities.find(prof.character); it != policy.character_priorities.end()) {
        prio = std::max(prio, it->second);
    }
    if (!prof.group_name.empty()) {
        if (auto it = policy.group_priorities.find(prof.group_name); it != policy.group_priorities.end()) {
            prio += it->second / 4;
        }
    }
    if (!prof.class_short.empty()) {
        if (auto it = policy.class_priorities.find(prof.class_short); it != policy.class_priorities.end()) {
            prio += it->second / 4;
        }
    }
    if (!policy.main_character.empty() && iequals(prof.character, policy.main_character)) {
        prio += 25;
    }
    return prio;
}

const PeerAvailability* find_peer(const LootEvaluationContext& ctx, std::string_view char_name) {
    for (const auto& p : ctx.peers) {
        if (iequals(p.character, char_name)) return &p;
    }
    return nullptr;
}

bool is_class_allowed_by_mask(std::uint32_t mask, std::string_view class_short) {
    if (mask == 0xFFFFFFFFu || mask == 0) return true;
    struct ClassBit { const char* name; int bit; };
    static constexpr ClassBit kBits[] = {
        {"WAR", 0}, {"CLR", 1}, {"PAL", 2}, {"RNG", 3},
        {"SHD", 4}, {"DRU", 5}, {"MNK", 6}, {"BRD", 7},
        {"ROG", 8}, {"SHM", 9}, {"NEC", 10}, {"WIZ", 11},
        {"MAG", 12}, {"ENC", 13}, {"BST", 14}, {"BER", 15},
    };
    for (const auto& cb : kBits) {
        if (iequals(cb.name, class_short)) {
            return (mask & (1u << cb.bit)) != 0;
        }
    }
    return true;
}

} // namespace

LootCandidate candidate_from_net_item(std::uint32_t corpse_id, std::int16_t corpse_slot, const net::Item& item) {
    LootCandidate c;
    c.corpse_id = corpse_id;
    c.corpse_slot = corpse_slot;
    c.item_id = item.id;
    c.item_name = item.name;
    c.quantity = static_cast<int>(std::max<std::uint32_t>(1, item.stack_count));
    c.no_drop = item.no_trade();
    c.lore = item.is_lore();
    c.tradeable = !c.no_drop;
    c.temporary = (item.no_rent == 0);
    c.quest_item = (item.quest_item != 0);
    c.class_mask = item.classes == 0 ? 0xFFFFFFFFu : item.classes;
    return c;
}

LootDecision LootDecisionEngine::evaluate(const LootCandidate& item,
                                          const LootEvaluationContext& ctx,
                                          const std::vector<CharacterEpicProfile>& profiles,
                                          const EpicDefinitionCatalog& catalog,
                                          const EpicLootPolicy& policy) {
    LootDecision d;
    d.dry_run = policy.dry_run;

    if (policy.emergency_disable) {
        d.outcome = LootDecisionOutcome::NeverLoot;
        d.reason = "Emergency disable switch is active; all epic loot actions halted.";
        return d;
    }
    if (!policy.epic_loot_enabled) {
        d.outcome = LootDecisionOutcome::DeferDecision;
        d.reason = "Epic-aware looting is disabled in global policy.";
        return d;
    }
    if (!ctx.looter_online || ctx.looter_zoning || ctx.looter_dead) {
        d.outcome = LootDecisionOutcome::DeferDecision;
        d.reason = "Defer decision: looter session is offline, zoning, or dead.";
        return d;
    }

    // Check explicit reservation first
    std::string reserved_char;
    const bool has_reservation = is_reserved_for(policy, item, reserved_char);

    std::vector<CandidateNeed> needs;
    std::vector<std::string> completed_chars;
    std::vector<std::string> review_chars;

    for (const auto& prof : profiles) {
        if (!ctx.looter_server.empty() && !prof.server.empty() &&
            !iequals(ctx.looter_server, prof.server)) {
            continue;
        }
        const EpicDefinition* def = !prof.epic_id.empty()
                                        ? catalog.find_by_id(prof.epic_id)
                                        : catalog.find_for_character(prof.class_short, prof.epic_variant, prof.server);
        if (!def) continue;

        // Variant / class safety: never loot for the wrong epic variant or restricted class
        if (!def->supports_class(prof.class_short)) continue;
        if (!is_class_allowed_by_mask(item.class_mask, prof.class_short) && item.no_drop) continue;

        // Check if this item belongs to this epic definition at all
        bool matches_any_stage = false;
        const bool stop_after_comp = prof.policy_override.has_override
                                         ? prof.policy_override.stop_after_completion
                                         : policy.stop_after_completion;
        const bool allow_obsolete = prof.policy_override.has_override
                                        ? prof.policy_override.allow_obsolete_items
                                        : policy.allow_obsolete_items;
        const bool only_active = prof.policy_override.has_override
                                     ? prof.policy_override.loot_only_active_stage
                                     : policy.loot_only_active_stage;
        const bool allow_future = prof.policy_override.has_override
                                      ? prof.policy_override.loot_future_stage_items
                                      : policy.loot_future_stage_items;
        const bool char_enabled = prof.policy_override.has_override
                                      ? prof.policy_override.epic_loot_enabled
                                      : true;

        if (!char_enabled) continue;

        // Check never-loot-after-completion list
        if (prof.has_completed_epic &&
            ProgressDetector::matches_item_id_or_name(item.item_id, item.item_name,
                                                      def->never_loot_after_completion_ids,
                                                      def->never_loot_after_completion_names)) {
            completed_chars.push_back(prof.character);
            continue;
        }

        for (const auto& st : def->stages) {
            for (const auto& req : st.required_items) {
                if (!item_matches_req(item, req)) continue;
                matches_any_stage = true;

                if (prof.needs_review) {
                    review_chars.push_back(prof.character + ": " + prof.review_reason);
                    continue;
                }
                if (prof.has_completed_epic && stop_after_comp && def->upgrade_next_epic_id.empty()) {
                    if (!(req.useful_for_others || (allow_obsolete && !req.obsolete_after_completion))) {
                        completed_chars.push_back(prof.character);
                        continue;
                    }
                }
                if (prof.has_completed_epic && stop_after_comp && !def->upgrade_next_epic_id.empty()) {
                    // Has completed this epic, check if item is actually for this completed epic
                    completed_chars.push_back(prof.character);
                    continue;
                }

                // Check ownership
                int owned = 0;
                for (const auto& ow : prof.owned_items) {
                    if ((req.item_id != 0 && ow.item_id == req.item_id) ||
                        (!req.item_name.empty() && iequals(ow.item_name, req.item_name))) {
                        owned = ow.owned_qty;
                        break;
                    }
                }
                if (owned >= req.quantity) continue;
                if (item.lore && owned > 0) continue;

                const bool is_active = iequals(prof.current_stage_id, st.stage_id);
                if (only_active && !is_active) continue;
                if (!is_active && !allow_future) continue;

                const bool is_looter = iequals(prof.character, ctx.looter_character);
                const PeerAvailability* peer = is_looter ? nullptr : find_peer(ctx, prof.character);
                const bool online = is_looter ? ctx.looter_online : (peer && peer->online);
                const bool same_group = is_looter || (peer && peer->in_same_group) ||
                                        (!ctx.looter_group.empty() && iequals(prof.group_name, ctx.looter_group));
                const bool same_raid = is_looter || (peer && peer->in_same_raid);

                if (has_reservation && !iequals(prof.character, reserved_char)) {
                    continue;
                }

                CandidateNeed need;
                need.character = prof.character;
                need.class_short = prof.class_short;
                need.group_name = prof.group_name;
                need.epic_id = def->epic_id;
                need.variant_name = def->variant_name;
                need.stage_id = st.stage_id;
                need.stage_order = st.order;
                need.is_active_stage = is_active;
                need.required_qty = req.quantity;
                need.owned_qty = owned;
                need.missing_qty = req.quantity - owned;
                need.effective_priority = compute_priority(prof, policy);
                need.in_same_group = same_group;
                need.in_same_raid = same_raid;
                need.online = online;
                needs.push_back(std::move(need));
            }
        }
        (void)matches_any_stage;
    }

    if (needs.empty()) {
        if (!review_chars.empty()) {
            d.outcome = LootDecisionOutcome::ManualReviewRequired;
            d.reason = "Manual review: " + review_chars.front();
            d.confidence = ConfidenceLevel::Low;
            return d;
        }
        if (!completed_chars.empty()) {
            d.outcome = LootDecisionOutcome::NeverLoot;
            d.reason = std::format("Do not loot: {} completed this epic and no upgrade path is configured.",
                                   completed_chars.front());
            d.confidence = ConfidenceLevel::High;
            return d;
        }
        d.outcome = LootDecisionOutcome::LeaveBecauseUnnecessary;
        d.reason = "Leave item: no active character requires this item.";
        d.confidence = ConfidenceLevel::High;
        return d;
    }

    // Multiple characters need this item -> record conflict list and sort by policy rule
    if (needs.size() > 1) {
        for (const auto& n : needs) d.conflict_characters.push_back(n.character);
        if (policy.conflict_rule == ConflictResolutionRule::ManualApproval &&
            policy.allow_manual_conflict_approval) {
            d.outcome = LootDecisionOutcome::ManualReviewRequired;
            d.reason = std::format("Manual review required: {} characters need '{}' ({}).",
                                   needs.size(), item.item_name, d.conflict_characters.front());
            d.confidence = ConfidenceLevel::Medium;
            return d;
        }
    }

    std::sort(needs.begin(), needs.end(), [&](const CandidateNeed& a, const CandidateNeed& b) {
        switch (policy.conflict_rule) {
        case ConflictResolutionRule::ActiveStageFirst:
            if (a.is_active_stage != b.is_active_stage) return a.is_active_stage > b.is_active_stage;
            if (a.effective_priority != b.effective_priority) return a.effective_priority > b.effective_priority;
            return a.missing_qty > b.missing_qty;
        case ConflictResolutionRule::HighestPriorityCharacter:
            if (a.effective_priority != b.effective_priority) return a.effective_priority > b.effective_priority;
            return a.is_active_stage > b.is_active_stage;
        case ConflictResolutionRule::MainCharacterFirst: {
            const bool a_main = !policy.main_character.empty() && iequals(a.character, policy.main_character);
            const bool b_main = !policy.main_character.empty() && iequals(b.character, policy.main_character);
            if (a_main != b_main) return a_main > b_main;
            return a.effective_priority > b.effective_priority;
        }
        case ConflictResolutionRule::HighestStageProgress:
            if (a.stage_order != b.stage_order) return a.stage_order > b.stage_order;
            return a.effective_priority > b.effective_priority;
        case ConflictResolutionRule::GroupPriorityThenCharacter:
            if (a.in_same_group != b.in_same_group) return a.in_same_group > b.in_same_group;
            return a.effective_priority > b.effective_priority;
        case ConflictResolutionRule::ClassPriority:
            return a.effective_priority > b.effective_priority;
        case ConflictResolutionRule::ManualApproval:
            return a.effective_priority > b.effective_priority;
        }
        return a.effective_priority > b.effective_priority;
    });

    const CandidateNeed& winner = needs.front();
    d.recipient = winner.character;
    d.epic_id = winner.epic_id;
    d.variant = winner.variant_name;
    d.stage_id = winner.stage_id;
    d.confidence = ConfidenceLevel::High;
    d.source = SignalSource::InventoryScan;

    const bool winner_is_looter = iequals(winner.character, ctx.looter_character);
    if (winner_is_looter) {
        d.outcome = LootDecisionOutcome::LootForCharacter;
        d.requires_confirmation = item.no_drop && policy.require_confirm_no_drop;
        d.reason = std::format("Loot for {}: required for {} Epic {}, {}; quantity {} of {} missing.",
                               winner.character, winner.class_short, winner.variant_name,
                               winner.stage_id, winner.missing_qty, winner.required_qty);
        return d;
    }

    // Recipient is another character
    if (item.no_drop) {
        if (!winner.in_same_group && !winner.in_same_raid) {
            d.outcome = LootDecisionOutcome::LeaveForAnotherGroup;
            d.reason = std::format("Leave for another group: '{}' is NO DROP and required by {} ({}).",
                                   item.item_name, winner.character, winner.group_name);
        } else {
            d.outcome = LootDecisionOutcome::LootForAnotherCharacter;
            d.requires_confirmation = policy.require_confirm_no_drop;
            d.reason = std::format("Loot for another character ({}): '{}' is NO DROP and required for {} {}.",
                                   winner.character, item.item_name, winner.variant_name, winner.stage_id);
        }
        return d;
    }

    if (!policy.loot_for_other_characters) {
        d.outcome = LootDecisionOutcome::LeaveForAnotherGroup;
        d.reason = std::format("Leave item: '{}' is needed by {}, and looting for others is disabled.",
                               item.item_name, winner.character);
        return d;
    }

    d.outcome = LootDecisionOutcome::LootAndAssignLater;
    d.reason = std::format("Loot and assign later to {}: tradeable item '{}' required for {} {} ({} of {} missing).",
                           winner.character, item.item_name, winner.variant_name, winner.stage_id,
                           winner.missing_qty, winner.required_qty);
    return d;
}

std::vector<LootDecisionEngine::ItemNeedSummary> LootDecisionEngine::summarize_item_needs(
    const std::vector<CharacterEpicProfile>& profiles,
    const EpicDefinitionCatalog& catalog,
    const EpicLootPolicy& policy) {
    std::map<std::string, ItemNeedSummary, std::less<>> by_item;

    for (const auto& prof : profiles) {
        if (prof.has_completed_epic && policy.stop_after_completion) continue;
        const EpicDefinition* def = !prof.epic_id.empty()
                                        ? catalog.find_by_id(prof.epic_id)
                                        : catalog.find_for_character(prof.class_short, prof.epic_variant, prof.server);
        if (!def) continue;

        for (const auto& req : prof.missing_items) {
            std::string key = req.item_id != 0 ? std::to_string(req.item_id) : req.item_name;
            auto& sum = by_item[key];
            if (sum.item_name.empty()) {
                sum.item_id = req.item_id;
                sum.item_name = req.item_name;
                if (const auto* st = def->find_stage(req.stage_id)) {
                    if (!st->required_zones.empty()) sum.source_zone = st->required_zones.front();
                }
            }
            sum.total_required += req.required_qty;
            sum.total_owned += req.owned_qty;
            sum.total_missing += req.missing_qty;
            sum.top_priority = std::max(sum.top_priority, compute_priority(prof, policy));
            sum.needing_characters.push_back(prof.character);
        }
    }

    std::vector<ItemNeedSummary> out;
    out.reserve(by_item.size());
    for (auto& [k, sum] : by_item) {
        (void)k;
        sum.has_conflict = sum.needing_characters.size() > 1;
        if (sum.has_conflict) {
            sum.recommended_action = policy.conflict_rule == ConflictResolutionRule::ManualApproval
                                         ? "Manual review required"
                                         : "Resolve by " + std::string(conflict_rule_name(policy.conflict_rule));
            sum.reason = std::format("{} characters need this item", sum.needing_characters.size());
        } else if (!sum.needing_characters.empty()) {
            sum.recommended_action = "Loot for " + sum.needing_characters.front();
            sum.reason = std::format("Needed by {} ({} missing)", sum.needing_characters.front(), sum.total_missing);
        }
        out.push_back(std::move(sum));
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.has_conflict != b.has_conflict) return a.has_conflict > b.has_conflict;
        return a.top_priority > b.top_priority;
    });
    return out;
}

bool CorpseLootCoordinator::try_claim(std::uint32_t corpse_id,
                                      std::int16_t corpse_slot,
                                      std::string_view owner_character,
                                      std::uint64_t now_ms,
                                      std::uint64_t ttl_ms) {
    prune_expired(now_ms);
    const auto key = std::make_pair(corpse_id, corpse_slot);
    auto it = claims_.find(key);
    if (it != claims_.end()) {
        if (it->second.completed) return false;
        if (it->second.expires_ms > now_ms && !iequals(it->second.owner_character, owner_character)) {
            return false;
        }
    }
    Claim c;
    c.corpse_id = corpse_id;
    c.corpse_slot = corpse_slot;
    c.owner_character = std::string(owner_character);
    c.claimed_ms = now_ms;
    c.expires_ms = now_ms + ttl_ms;
    c.completed = false;
    claims_[key] = std::move(c);
    return true;
}

bool CorpseLootCoordinator::mark_completed(std::uint32_t corpse_id, std::int16_t corpse_slot, std::string_view owner_character) {
    const auto key = std::make_pair(corpse_id, corpse_slot);
    auto it = claims_.find(key);
    if (it == claims_.end()) return false;
    if (!iequals(it->second.owner_character, owner_character)) return false;
    it->second.completed = true;
    return true;
}

void CorpseLootCoordinator::release_claim(std::uint32_t corpse_id, std::int16_t corpse_slot, std::string_view owner_character) {
    const auto key = std::make_pair(corpse_id, corpse_slot);
    auto it = claims_.find(key);
    if (it != claims_.end() && !it->second.completed && iequals(it->second.owner_character, owner_character)) {
        claims_.erase(it);
    }
}

void CorpseLootCoordinator::expire_corpse(std::uint32_t corpse_id) {
    for (auto it = claims_.begin(); it != claims_.end();) {
        if (it->first.first == corpse_id) it = claims_.erase(it);
        else ++it;
    }
}

void CorpseLootCoordinator::release_character(std::string_view owner_character) {
    for (auto it = claims_.begin(); it != claims_.end();) {
        if (!it->second.completed && iequals(it->second.owner_character, owner_character)) {
            it = claims_.erase(it);
        } else {
            ++it;
        }
    }
}

void CorpseLootCoordinator::prune_expired(std::uint64_t now_ms) {
    for (auto it = claims_.begin(); it != claims_.end();) {
        if (!it->second.completed && it->second.expires_ms <= now_ms) {
            it = claims_.erase(it);
        } else {
            ++it;
        }
    }
}

void CorpseLootCoordinator::clear() {
    claims_.clear();
}

bool CorpseLootCoordinator::is_claimed_by_other(std::uint32_t corpse_id,
                                                std::int16_t corpse_slot,
                                                std::string_view character,
                                                std::uint64_t now_ms) const {
    const auto key = std::make_pair(corpse_id, corpse_slot);
    auto it = claims_.find(key);
    if (it == claims_.end()) return false;
    if (it->second.completed) return true;
    if (it->second.expires_ms <= now_ms) return false;
    return !iequals(it->second.owner_character, character);
}

bool CorpseLootCoordinator::is_completed(std::uint32_t corpse_id, std::int16_t corpse_slot) const {
    const auto key = std::make_pair(corpse_id, corpse_slot);
    auto it = claims_.find(key);
    return it != claims_.end() && it->second.completed;
}

std::size_t CorpseLootCoordinator::active_claim_count(std::uint64_t now_ms) const {
    std::size_t count = 0;
    for (const auto& [k, c] : claims_) {
        (void)k;
        if (!c.completed && c.expires_ms > now_ms) ++count;
    }
    return count;
}

} // namespace hailstone::epic
