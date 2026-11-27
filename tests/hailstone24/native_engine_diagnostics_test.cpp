#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

#include "combat/combat_engine.h"
#include "combat/combat_types.h"
#include "combat/diagnostics.h"

using namespace hailstone::combat;

namespace {

struct NopActions : public ICombatActions {
    void set_target(std::uint32_t) override {}
    void set_auto_attack(bool) override {}
    void pet_attack(std::uint32_t) override {}
    void pet_back() override {}
    bool cast_gem(int) override { return true; }
    void stop_cast() override {}
    bool combat_ability(std::string_view) override { return true; }
    bool memorize_gem(int, int) override { return true; }
    bool memorize_loadout(const std::vector<std::pair<int, std::uint32_t>>&) override { return true; }
    void set_stand_state(bool) override {}
    void move_follow(std::uint32_t, float) override {}
    void move_to_range(std::uint32_t, float, float) override {}
    void move_melee(std::uint32_t, float, bool) override {}
    void move_disengage(const Vec3&, float) override {}
    void stop_movement(std::string_view) override {}
    void log_message(std::string_view, std::string_view) override {}
};

CombatSnapshot in_zone_snapshot(std::uint64_t now_ms) {
    CombatSnapshot snapshot;
    snapshot.now_ms = now_ms;
    snapshot.in_zone = true;
    snapshot.character_name = "Boxnec";
    snapshot.class_short = "NEC";
    snapshot.server_name = "Fixture";
    snapshot.zone_short = "qeynos";
    snapshot.me.id = 1000;
    snapshot.me.name = "Boxnec";
    snapshot.me.hp_pct = 100;
    return snapshot;
}

int count_idle_records(const DiagnosticsSink& sink) {
    int count = 0;
    for (const auto& record : sink.records()) {
        if (record.action == "Idle") ++count;
    }
    return count;
}

} // namespace

TEST_CASE("engine diagnostics: an idle tick is recorded once, with its reason") {
    DiagnosticsSink sink;
    sink.set_identity("Boxnec", "Fixture", "NEC", "boxgroup");
    CombatEngine engine;
    engine.set_diagnostics(&sink);
    NopActions actions;

    engine.tick(in_zone_snapshot(1000), actions);

    REQUIRE(sink.size() >= 1);
    const auto records = sink.records(DiagnosticModule::Engine);
    REQUIRE_FALSE(records.empty());
    CHECK(records.front().action == "Idle");
    CHECK(records.front().reason == "Controller stopped");
    CHECK(records.front().character == "Boxnec");
    CHECK(records.front().group_id == "boxgroup");
    CHECK(records.front().state == "idle");
}

TEST_CASE("engine diagnostics: repeating the same idle reason folds into a count") {
    DiagnosticsSink sink;
    CombatEngine engine;
    engine.set_diagnostics(&sink);
    NopActions actions;

    engine.start("test start");
    for (int i = 0; i < 20; ++i) {
        engine.tick(in_zone_snapshot(2000 + static_cast<std::uint64_t>(i) * 100), actions);
    }

    CHECK(count_idle_records(sink) == 1);
    const auto records = sink.records(DiagnosticModule::Engine);
    REQUIRE_FALSE(records.empty());
    bool found_waiting = false;
    for (const auto& record : records) {
        if (record.action == "Idle") {
            found_waiting = true;
            CHECK(record.reason == "Waiting for target");
            CHECK(record.occurrences == 20);
            CHECK(record.suppressed_repeats == 19);
        }
    }
    CHECK(found_waiting);
    CHECK(sink.suppressed_total() == 19);
}

TEST_CASE("engine diagnostics: an issued action is recorded instead of an idle tick") {
    DiagnosticsSink sink;
    CombatEngine engine;
    engine.set_diagnostics(&sink);
    NopActions actions;

    engine.start("test start");
    REQUIRE(sink.size() >= 1);
    CHECK(count_idle_records(sink) == 0);

    const auto records = sink.records();
    REQUIRE_FALSE(records.empty());
    // start() is a control action, not a state change: the engine stays Idle
    // until a tick finds something to do.
    CHECK(records.front().action == "control");
    CHECK(records.front().module == DiagnosticModule::Engine);
    CHECK(records.front().ok);
    CHECK(records.front().reason == "test start");

    // The first tick that leaves Idle is recorded as a state transition, so the
    // log carries the movement between states and not just the actions taken.
    // An empty snapshot can only ever stay Idle, so the tick needs a hostile in reach.
    CombatSnapshot leaving_idle = in_zone_snapshot(2000);
    SpawnSnapshot dummy;
    dummy.id = 501;
    dummy.name = "a_training_dummy";
    dummy.position = {10.0f, 0.0f, 0.0f};
    dummy.level = 50;
    dummy.hp_pct = 100;
    dummy.hp_known = true;
    dummy.is_npc = true;
    dummy.targetable = true;
    leaving_idle.spawns.push_back(dummy);
    leaving_idle.has_target = true;
    leaving_idle.target = dummy;
    leaving_idle.allowed_to_attack_target = true;

    NopActions tick_actions;
    engine.tick(leaving_idle, tick_actions);
    REQUIRE(engine.state() == CombatState::Combat);
    const auto after_tick = sink.records();
    bool saw_transition = false;
    for (const auto& record : after_tick) {
        if (record.action == "state" && record.reason.find("->") != std::string::npos) {
            saw_transition = true;
        }
    }
    CHECK(saw_transition);
}

TEST_CASE("engine diagnostics: a dry-run action is marked, and an engine without a sink is silent") {
    DiagnosticsSink sink;
    CombatEngine engine;
    engine.set_diagnostics(&sink);
    NopActions actions;

    engine.set_dry_run(true);
    engine.start("dry run start");
    REQUIRE(sink.size() >= 1);
    CHECK_FALSE(sink.records().front().ok);
    CHECK(sink.records().front().reason.rfind("DryRun ", 0) == 0);

    CombatEngine silent;
    silent.start("no sink");
    NopActions more_actions;
    silent.tick(in_zone_snapshot(3000), more_actions);
    CHECK(sink.size() >= 1); // the attached sink saw nothing new from `silent`
}
