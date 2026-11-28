#pragma once

// Progression-aware epic loot decision engine and multi-client corpse claim
// coordinator. Conservative, deterministic, and MacroQuest-free.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "epic/epic_definitions.h"
#include "epic/epic_types.h"
#include "net/item_packets.h"

namespace hailstone::epic {

struct LootCandidate {
    std::uint32_t corpse_id = 0;
    std::int16_t corpse_slot = 0;
    std::uint32_t item_id = 0;
    std::string item_name;
    int quantity = 1;
    bool no_drop = false;
    bool lore = false;
    bool tradeable = true;
    bool temporary = false;
    bool quest_item = false;
    std::uint32_t class_mask = 0xFFFFFFFFu; // bitmask of EQ classes; 0xFFFFFFFF = all
};

LootCandidate candidate_from_net_item(std::uint32_t corpse_id, std::int16_t corpse_slot, const net::Item& item);

struct PeerAvailability {
    std::string server;
    std::string character;
    std::string class_short;
    std::string group_name;
    bool online = false;
    bool in_same_group = false;
    bool in_same_raid = false;
    bool in_same_zone = false;
    float distance = 0.0f;
    std::uint64_t last_seen_ms = 0;
};

struct LootEvaluationContext {
    std::string looter_server;
    std::string looter_character;
    std::string looter_class;
    std::string looter_group;
    bool looter_online = true;
    bool looter_zoning = false;
    bool looter_dead = false;
    std::uint64_t now_ms = 0;
    std::vector<PeerAvailability> peers;
};

struct CandidateNeed {
    std::string character;
    std::string class_short;
    std::string group_name;
    std::string epic_id;
    std::string variant_name;
    std::string stage_id;
    int stage_order = 0;
    bool is_active_stage = false;
    int required_qty = 1;
    int owned_qty = 0;
    int missing_qty = 1;
    int effective_priority = 50;
    bool in_same_group = false;
    bool in_same_raid = false;
    bool online = false;
    bool needs_review = false;
    std::string review_reason;
};

struct LootDecision {
    LootDecisionOutcome outcome = LootDecisionOutcome::LeaveBecauseUnnecessary;
    std::string recipient;
    std::string reason;
    std::vector<std::string> conflict_characters;
    std::string epic_id;
    std::string variant;
    std::string stage_id;
    ConfidenceLevel confidence = ConfidenceLevel::None;
    SignalSource source = SignalSource::Unknown;
    bool dry_run = false;
    bool requires_confirmation = false;
};

class LootDecisionEngine {
public:
    static LootDecision evaluate(const LootCandidate& item,
                                 const LootEvaluationContext& ctx,
                                 const std::vector<CharacterEpicProfile>& profiles,
                                 const EpicDefinitionCatalog& catalog,
                                 const EpicLootPolicy& policy);

    // Aggregate item-focused needs across all tracked profiles (for the UI Items & Conflicts view).
    struct ItemNeedSummary {
        std::uint32_t item_id = 0;
        std::string item_name;
        std::string source_zone;
        int total_required = 0;
        int total_owned = 0;
        int total_missing = 0;
        int top_priority = 0;
        bool has_conflict = false;
        std::vector<std::string> needing_characters;
        std::string recommended_action;
        std::string reason;
    };
    static std::vector<ItemNeedSummary> summarize_item_needs(const std::vector<CharacterEpicProfile>& profiles,
                                                             const EpicDefinitionCatalog& catalog,
                                                             const EpicLootPolicy& policy);
};

// Coordinates corpse/slot claims across local ticks and IPC peers so two clients
// or loot passes never act on the same corpse slot simultaneously or duplicate claims.
class CorpseLootCoordinator {
public:
    struct Claim {
        std::uint32_t corpse_id = 0;
        std::int16_t corpse_slot = -1;
        std::string owner_character;
        std::uint64_t claimed_ms = 0;
        std::uint64_t expires_ms = 0;
        bool completed = false;
    };

    bool try_claim(std::uint32_t corpse_id,
                   std::int16_t corpse_slot,
                   std::string_view owner_character,
                   std::uint64_t now_ms,
                   std::uint64_t ttl_ms = 10000);

    bool mark_completed(std::uint32_t corpse_id, std::int16_t corpse_slot, std::string_view owner_character);
    void release_claim(std::uint32_t corpse_id, std::int16_t corpse_slot, std::string_view owner_character);
    void expire_corpse(std::uint32_t corpse_id);
    void release_character(std::string_view owner_character);
    void prune_expired(std::uint64_t now_ms);
    void clear();

    bool is_claimed_by_other(std::uint32_t corpse_id,
                             std::int16_t corpse_slot,
                             std::string_view character,
                             std::uint64_t now_ms) const;
    bool is_completed(std::uint32_t corpse_id, std::int16_t corpse_slot) const;
    std::size_t active_claim_count(std::uint64_t now_ms) const;

private:
    std::map<std::pair<std::uint32_t, std::int16_t>, Claim> claims_;
};

} // namespace hailstone::epic
