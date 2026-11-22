#include "doctest.h"

#include <chrono>
#include <filesystem>
#include <thread>

#include "cortex/inference_provider.h"
#include "cortex/memory.h"
#include "cortex/observation.h"
#include "cortex/supervisor.h"
#include "cortex/validator.h"

using namespace hailstone::cortex;
using namespace hailstone::ipc;

namespace {

SessionSnapshot make_snap(const std::string& name,
                          const std::string& cls,
                          std::uint8_t hp,
                          std::uint8_t mana,
                          const std::string& leader = "Tankone") {
    SessionSnapshot s;
    s.session_id = "sess-" + name;
    s.server_name = "ez-server";
    s.character_name = name;
    s.class_short = cls;
    s.level = 70;
    s.online = true;
    s.zone_short = "qrg";
    s.hp_pct = hp;
    s.mana_pct = mana;
    s.end_pct = 100;
    s.combat_state = "Combat";
    s.group_leader = leader;
    return s;
}

} // namespace

TEST_CASE("Cortex validator: parses valid JSON and rejects malformed/stale") {
    auto d1 = validate_decision_json(
        R"({"action":"hold_pull","reason":"healer_mana_low"})", 1000, 1050, 42);
    CHECK(d1.valid);
    CHECK(d1.intention == Intention::HoldPull);
    CHECK(d1.reason_code == "healer_mana_low");

    auto d2 = validate_decision_json("not json at all", 1000, 1050, 10);
    CHECK_FALSE(d2.valid);
    CHECK(d2.rejection_reason == "malformed_json");

    auto d3 = validate_decision_json(
        R"({"action":"rm_rf_root","reason":"bad"})", 1000, 1050, 10);
    CHECK_FALSE(d3.valid);

    auto d4 = validate_decision_json(
        R"({"action":"hold_pull","reason":"ok"})", 1000, 25000, 10);
    CHECK_FALSE(d4.valid);
    CHECK(d4.rejection_reason == "stale_observation");
}

TEST_CASE("Cortex arbitration: observe mode never executes; emergency stop wins") {
    Decision d;
    d.valid = true;
    d.intention = Intention::HoldPull;

    ArbitrationContext ctx;
    ctx.mode = OperatingMode::Observe;
    auto r1 = arbitrate(d, ctx);
    CHECK_FALSE(r1.execute_cortex);
    CHECK(r1.owner == "rgmercs");

    ctx.mode = OperatingMode::Autonomous;
    ctx.emergency_stop = true;
    auto r2 = arbitrate(d, ctx);
    CHECK_FALSE(r2.execute_cortex);
    CHECK(r2.owner == "emergency_stop");

    ctx.emergency_stop = false;
    auto r3 = arbitrate(d, ctx);
    CHECK(r3.execute_cortex);
    CHECK(r3.owner == "cortex");
}

TEST_CASE("Cortex memory: SQLite namespace isolation and restart persistence") {
    auto db_path = std::filesystem::temp_directory_path() / "hailstone_cortex_mem_test.db";
    std::filesystem::remove(db_path);

    {
        MemoryStore mem;
        REQUIRE(mem.open(db_path.string()));
        CharacterIdentity id1{"ez-server", "Tankone", "WAR", 70, "tank", "steady"};
        CharacterIdentity id2{"ez-server", "Healerone", "CLR", 70, "healer", "cautious"};
        CHECK(mem.upsert_profile(id1, "hold_camp"));
        CHECK(mem.upsert_profile(id2, "keep_tank_alive"));
        CHECK(mem.add_memory("ez-server", "Tankone", "outcome", "won_pull", 100));
        CHECK(mem.add_memory("ez-server", "Healerone", "outcome", "low_mana_warning", 101));
        CHECK(mem.memory_count("ez-server", "Tankone") == 1);
        CHECK(mem.memory_count("ez-server", "Healerone") == 1);
    }

    // Simulate restart: reopen DB and verify isolation & persistence.
    {
        MemoryStore mem2;
        REQUIRE(mem2.open(db_path.string()));
        StoredProfile p1;
        REQUIRE(mem2.load_profile("ez-server", "Tankone", p1));
        CHECK(p1.current_objective == "hold_camp");
        auto m1 = mem2.recent_memories("ez-server", "Tankone", 8);
        REQUIRE(m1.size() == 1);
        CHECK(m1[0].content == "won_pull");
        auto m2 = mem2.recent_memories("ez-server", "Healerone", 8);
        REQUIRE(m2.size() == 1);
        CHECK(m2[0].content == "low_mana_warning");
    }
    std::filesystem::remove(db_path);
}

TEST_CASE("Cortex supervisor: 24 independent agents, messaging, emergency stop, and fault tolerance") {
    auto db_path = std::filesystem::temp_directory_path() / "hailstone_cortex_sup_test.db";
    std::filesystem::remove(db_path);

    auto prov = std::make_shared<MockInferenceProvider>();
    auto mem = std::make_shared<MemoryStore>();
    REQUIRE(mem->open(db_path.string()));

    SupervisorConfig cfg;
    cfg.worker_threads = 2;
    cfg.per_char_cooldown_ms = 0;
    CortexSupervisor sup(prov, mem, cfg);
    sup.set_mode(OperatingMode::Observe);
    sup.start();

    // Register 24 characters across 4 groups of 6.
    std::vector<SessionSnapshot> roster;
    for (int g = 0; g < 4; ++g) {
        std::string leader = "Leader" + std::to_string(g);
        for (int m = 0; m < 6; ++m) {
            std::string name = "Char_g" + std::to_string(g) + "_m" + std::to_string(m);
            if (m == 0) name = leader;
            roster.push_back(make_snap(name, (m == 1 ? "CLR" : "WAR"), 90, (m == 1 ? 10 : 90), leader));
        }
    }

    for (const auto& s : roster) {
        std::string key = s.server_name + ":" + s.character_name;
        sup.set_agent_enabled(key, true);
        auto obs = observe(s, roster, 5000);
        sup.submit_observation(obs, true);
    }

    // Wait for all 24 decisions.
    for (int i = 0; i < 100; ++i) {
        auto snap = sup.snapshot();
        if (snap.total_decisions >= 24) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto snap = sup.snapshot();
    CHECK(snap.agents.size() == 24);
    CHECK(snap.total_decisions >= 24);

    // Inter-character message with TTL.
    AgentMessage msg;
    msg.type = AgentMessageType::RequestAssistance;
    msg.from_key = "ez-server:Leader0";
    msg.to_key = "ez-server:Char_g0_m1";
    msg.payload = "need_heal";
    sup.send_message(msg);
    auto inbox = sup.drain_messages("ez-server:Char_g0_m1");
    REQUIRE(inbox.size() == 1);
    CHECK(inbox[0].payload == "need_heal");

    // Emergency stop & provider disconnect resilience.
    sup.trigger_emergency_stop();
    CHECK(sup.snapshot().emergency_stop);
    sup.clear_emergency_stop();
    prov->set_healthy(false);
    sup.submit_observation(observe(roster[0], roster, 6000), true);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(sup.snapshot().total_errors >= 1);

    sup.stop();
    std::filesystem::remove(db_path);
}
