#include <doctest/doctest.h>

#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include <lua.hpp>

#include "app/lua_ui.h"
#include "app/lua_ui_caps.h"
#include "core/atomic_file.h"
#include "epic/epic_definitions.h"
#include "epic/epic_manager.h"
#include "epic/epic_store.h"
#include "epic/epic_types.h"
#include "epic/loot_decision_engine.h"
#include "epic/progress_detector.h"
#include "plugin/policy.h"

namespace fs = std::filesystem;
using namespace hailstone;
using namespace hailstone::epic;

namespace {

fs::path make_temp_dir(const char* tag) {
    fs::path p = fs::temp_directory_path() / std::format("hailstone_epic_test_{}", tag);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

CharacterInventorySnapshot make_snap(std::string server, std::string ch, std::string cls,
                                       std::string group, std::uint64_t ts,
                                       std::vector<ObservedItem> items = {},
                                       std::vector<std::string> flags = {}) {
    CharacterInventorySnapshot s;
    s.server = std::move(server);
    s.character = std::move(ch);
    s.class_short = std::move(cls);
    s.group_name = std::move(group);
    s.observed_ms = ts;
    s.items = std::move(items);
    s.quest_flags = std::move(flags);
    return s;
}

} // namespace

TEST_CASE("EpicDefinitions: loading, variant selection, class restrictions, and custom JSON") {
    EpicDefinitionCatalog cat;
    cat.load_builtin_defaults();
    CHECK(cat.all().size() >= 16);

    const EpicDefinition* war_v2 = cat.find_for_character("WAR", "Variant 2", "ez-server");
    REQUIRE(war_v2 != nullptr);
    CHECK(war_v2->epic_id == "war_ez_v2");
    CHECK(war_v2->stages.size() == 6);
    CHECK(war_v2->supports_class("WAR"));
    CHECK_FALSE(war_v2->supports_class("CLR"));

    const char* custom_json = R"({
      "schema_version": 1,
      "epics": [
        {
          "epic_id": "ez_custom_tank",
          "family": "EZ_Server",
          "variant_name": "EZ Epic 3.0",
          "class_restrictions": ["WAR", "PAL", "SHD"],
          "completion_item_ids": [99001],
          "completion_item_names": ["Blade of Ages 3.0"],
          "stages": [
            {
              "stage_id": "s1",
              "order": 1,
              "name": "Token Turn-in",
              "required_zones": ["qvic"],
              "is_completion_stage": true,
              "required_items": [
                {"item_id": 99000, "item_name": "Tank Epic Token", "quantity": 2, "no_drop": true, "lore": false}
              ]
            }
          ]
        }
      ]
    })";
    std::string err;
    REQUIRE(cat.load_from_json_string(custom_json, &err, true));
    const EpicDefinition* custom = cat.find_by_id("ez_custom_tank");
    REQUIRE(custom != nullptr);
    CHECK(custom->supports_class("PAL"));
    CHECK_FALSE(custom->supports_class("WIZ"));
    CHECK(custom->stages.front().required_items.front().quantity == 2);
}

TEST_CASE("ProgressDetector: conservative completion, alternative items, reconciliation, and manual rollback") {
    EpicDefinitionCatalog cat;
    cat.load_builtin_defaults();
    const EpicDefinition* war_v2 = cat.find_by_id("war_ez_v2");
    REQUIRE(war_v2 != nullptr);

    // 1. Character has completion item without prior stages -> must flag needs_review, never silently complete!
    CharacterEpicProfile roger;
    auto snap1 = make_snap("ez", "Rogerwho", "WAR", "Group1", 1000,
                           {{91100, "Blade of the Eternal Champion", 1, true, false, true, true, false}});
    auto rep1 = ProgressDetector::scan_and_reconcile(*war_v2, snap1, roger, &cat);
    CHECK(roger.needs_review);
    CHECK_FALSE(roger.has_completed_epic);
    CHECK(roger.confidence == ConfidenceLevel::Low);

    // 2. Alternative item satisfies Stage 6 requirement
    CharacterEpicProfile frank;
    for (int i = 1; i <= 5; ++i) {
        ProgressDetector::mark_stage_complete(*war_v2, frank, std::format("stage_{}", i), 1000 + i);
    }
    auto snap_frank_alt = make_snap("ez", "Frankdatank", "WAR", "Group1", 2000,
                                    {{91099, "Champion's Radiant Seal", 1, false, false, true, true, false}});
    ProgressDetector::scan_and_reconcile(*war_v2, snap_frank_alt, frank, &cat);
    CHECK(frank.state == EpicProgressState::ReadyForCompletion);

    // 3. Missing item reconciliation without stage completion flags inconsistency
    CharacterEpicProfile tank2;
    auto s_acq = make_snap("ez", "Tanktwo", "WAR", "Group2", 3000,
                           {{91001, "Warrior Epic Qvic Stone & Mold", 2, false, false, true, false, false}});
    auto r_acq = ProgressDetector::scan_and_reconcile(*war_v2, s_acq, tank2, &cat);
    CHECK(tank2.owned_items.size() == 1);
    CHECK(tank2.owned_items.front().owned_qty == 2);

    auto s_lost = make_snap("ez", "Tanktwo", "WAR", "Group2", 4000, {});
    auto r_lost = ProgressDetector::scan_and_reconcile(*war_v2, s_lost, tank2, &cat);
    CHECK(r_lost.inconsistency_detected);
    CHECK(tank2.needs_review);

    // 4. Manual override + rollback
    ProgressDetector::mark_stage_complete(*war_v2, tank2, "stage_1", 5000, "User verified turn-in");
    CHECK(tank2.completed_stages.size() == 1);
    std::string rb_msg;
    CHECK(EpicProgressStore::rollback_last_manual_change(tank2, 6000, &rb_msg));
    CHECK(tank2.completed_stages.empty());
}

TEST_CASE("LootDecisionEngine: active stage, completed epic, conflicts, no-drop, dry-run, and estop") {
    EpicDefinitionCatalog cat;
    cat.load_builtin_defaults();
    const EpicDefinition* war_v2 = cat.find_by_id("war_ez_v2");
    const EpicDefinition* war_1_0 = cat.find_by_id("war_1_0");
    REQUIRE(war_v2 != nullptr);
    REQUIRE(war_1_0 != nullptr);

    // Frankdatank at stage 6 missing Champion's Final Seal
    CharacterEpicProfile frank;
    for (int i = 1; i <= 5; ++i) {
        ProgressDetector::mark_stage_complete(*war_v2, frank, std::format("stage_{}", i), 100);
    }
    ProgressDetector::scan_and_reconcile(*war_v2, make_snap("ez", "Frankdatank", "WAR", "Group1", 200), frank, &cat);

    // Norosun completed Epic 2.0 (war_ez_v2 has no upgrade_next_epic_id)
    CharacterEpicProfile norosun;
    for (int i = 1; i <= 6; ++i) {
        ProgressDetector::mark_stage_complete(*war_v2, norosun, std::format("stage_{}", i), 100);
    }
    ProgressDetector::scan_and_reconcile(
        *war_v2,
        make_snap("ez", "Norosun", "WAR", "Group1", 200,
                  {{91100, "Blade of the Eternal Champion", 1, true, false, true, true, false}}),
        norosun, &cat);
    CHECK(norosun.has_completed_epic);

    // Upgrade path test: war_1_0 -> war_ez_v2
    CharacterEpicProfile upg;
    ProgressDetector::mark_stage_complete(*war_1_0, upg, "stage_1_briar", 100);
    ProgressDetector::mark_stage_complete(*war_1_0, upg, "stage_2_blades", 101);
    ProgressDetector::scan_and_reconcile(
        *war_1_0,
        make_snap("ez", "Upgrader", "WAR", "Group1", 200,
                  {{10908, "Jagged Blade of War", 1, true, false, true, true, false}}),
        upg, &cat);
    CHECK(upg.state == EpicProgressState::CompletedWithUpgrade);

    EpicLootPolicy policy;
    policy.require_confirm_no_drop = false;

    LootCandidate seal;
    seal.corpse_id = 5001;
    seal.corpse_slot = 23;
    seal.item_id = 91006;
    seal.item_name = "Champion's Final Seal";
    seal.no_drop = true;
    seal.lore = true;

    LootEvaluationContext ctx;
    ctx.looter_server = "ez";
    ctx.looter_character = "Frankdatank";
    ctx.looter_class = "WAR";
    ctx.looter_group = "Group1";
    ctx.now_ms = 1000;

    // 1. Loot for Frankdatank
    auto d1 = LootDecisionEngine::evaluate(seal, ctx, {frank, norosun}, cat, policy);
    CHECK(d1.outcome == LootDecisionOutcome::LootForCharacter);
    CHECK(d1.recipient == "Frankdatank");
    CHECK(d1.reason.find("Frankdatank") != std::string::npos);

    // 2. Completed Norosun alone -> NeverLoot
    auto d2 = LootDecisionEngine::evaluate(seal, ctx, {norosun}, cat, policy);
    CHECK(d2.outcome == LootDecisionOutcome::NeverLoot);
    CHECK(d2.reason.find("Norosun") != std::string::npos);

    // 3. Unneeded item -> LeaveBecauseUnnecessary
    LootCandidate junk{5001, 24, 99999, "Rusty Dagger", 1, false, false, true, false, false};
    auto d3 = LootDecisionEngine::evaluate(junk, ctx, {frank, norosun}, cat, policy);
    CHECK(d3.outcome == LootDecisionOutcome::LeaveBecauseUnnecessary);

    // 4. Multiple characters needing same no-drop item -> priority / conflict handling
    CharacterEpicProfile other_war;
    for (int i = 1; i <= 5; ++i) {
        ProgressDetector::mark_stage_complete(*war_v2, other_war, std::format("stage_{}", i), 100);
    }
    ProgressDetector::scan_and_reconcile(*war_v2, make_snap("ez", "Otherwar", "WAR", "Group2", 200), other_war, &cat);

    policy.character_priorities["Otherwar"] = 95;
    policy.character_priorities["Frankdatank"] = 50;
    policy.conflict_rule = ConflictResolutionRule::HighestPriorityCharacter;
    auto d4 = LootDecisionEngine::evaluate(seal, ctx, {frank, other_war}, cat, policy);
    CHECK(d4.conflict_characters.size() == 2);
    CHECK(d4.recipient == "Otherwar");
    CHECK(d4.outcome == LootDecisionOutcome::LeaveForAnotherGroup);

    // 5. Dry-run and Emergency Disable
    policy.dry_run = true;
    auto d5 = LootDecisionEngine::evaluate(seal, ctx, {frank}, cat, policy);
    CHECK(d5.dry_run);

    policy.emergency_disable = true;
    auto d6 = LootDecisionEngine::evaluate(seal, ctx, {frank}, cat, policy);
    CHECK(d6.outcome == LootDecisionOutcome::NeverLoot);
}

TEST_CASE("CorpseLootCoordinator & EpicManager: concurrency, expiration, disconnects, and 24-char persistence") {
    const fs::path dir = make_temp_dir("mgr");
    EpicManager mgr(dir);
    REQUIRE(mgr.initialize());

    // Simulate 24 characters across 4 groups
    for (int i = 1; i <= 24; ++i) {
        const std::string group = std::format("Group{}", ((i - 1) / 6) + 1);
        const std::string ch = std::format("Toon{:02d}", i);
        const char* cls = (i % 4 == 1) ? "WAR" : (i % 4 == 2) ? "CLR" : (i % 4 == 3) ? "WIZ" : "ROG";
        mgr.scan_character(make_snap("ez", ch, cls, group, 1000 + i), "");
    }
    CHECK(mgr.profiles_snapshot().size() == 24);

    // Export / import roundtrip
    const std::string exported = mgr.export_database_json();
    CHECK(exported.find("Toon24") != std::string::npos);

    EpicManager mgr2(dir);
    REQUIRE(mgr2.initialize());
    CHECK(mgr2.profiles_snapshot().size() == 24);

    // Corpse concurrency: two clients cannot claim the same corpse slot simultaneously
    CorpseLootCoordinator coord;
    CHECK(coord.try_claim(7001, 23, "Toon01", 1000, 5000));
    CHECK_FALSE(coord.try_claim(7001, 23, "Toon02", 1500, 5000));
    // Stale session expires after TTL
    CHECK(coord.try_claim(7001, 23, "Toon02", 7000, 5000));
    // Disconnect/zone releases claims
    coord.release_character("Toon02");
    CHECK_FALSE(coord.is_claimed_by_other(7001, 23, "Toon01", 7100));
    // Corpse expiration clears all slots for corpse
    CHECK(coord.try_claim(7002, 23, "Toon01", 8000, 5000));
    coord.expire_corpse(7002);
    CHECK(coord.active_claim_count(8100) == 0);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Epic UI contracts: hs.Epic* Lua adapters, capability gating, and special_functions.lua") {
    CHECK(app::ui_call_caps("EpicStatus") == std::optional<std::uint32_t>{0u});
    CHECK(app::ui_call_caps("EpicItems") == std::optional<std::uint32_t>{0u});
    CHECK(app::ui_call_caps("EpicAudit") == std::optional<std::uint32_t>{0u});
    CHECK(app::ui_call_caps("EpicExport") == std::optional<std::uint32_t>{0u});
    CHECK(app::ui_call_caps("EpicDo") ==
          std::optional<std::uint32_t>{plugin::kCapActions | plugin::kCapCommands});
    CHECK(app::ui_call_caps("EpicImport") ==
          std::optional<std::uint32_t>{plugin::kCapActions | plugin::kCapCommands});

    lua_State* L = luaL_newstate();
    REQUIRE(L != nullptr);
    luaL_openlibs(L);
    const fs::path sf_path = fs::path(HAILSTONE_SOURCE_DIR) / "ui/modern/special_functions.lua";
    REQUIRE(fs::exists(sf_path));
    CHECK(luaL_loadfile(L, sf_path.string().c_str()) == LUA_OK);
    lua_close(L);

    auto content = hailstone::read_text_file(sf_path);
    REQUIRE(content.has_value());
    CHECK(content->find("Epic Quests & Loot") != std::string::npos);
    CHECK(content->find("API.epic.Status") != std::string::npos);
    CHECK(content->find("API.epic.Items") != std::string::npos);
    CHECK(content->find("API.epic.Audit") != std::string::npos);
}
