#include "doctest.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>

#include "cortex/inference_provider.h"
#include "cortex/memory.h"
#include "cortex/observation.h"
#include "cortex/service.h"
#include "cortex/supervisor.h"
#include "cortex/validator.h"

// The service tick takes a clock reading in milliseconds; there is no
// platform helper for one, so this test reads its own steady clock.
std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

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

namespace {

// The client's wait for a decision the mock provider answers immediately: the queue is drained by
// the supervisor's own worker thread.
template <typename Done>
void wait_for_decision(Done done) {
    for (int i = 0; i < 300; ++i) {
        if (done()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

TEST_CASE("Cortex memory: open_memory_store makes the folders above the file and refuses a path that is not a database") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "hailstone_cortex_open_store";
    std::filesystem::remove_all(dir);

    // The client's seam: a nested path under the data folder is created on the way (paths.h leaves
    // folder creation to each writer).
    auto store = open_memory_store(dir / "cortex" / "memory.db");
    REQUIRE(store != nullptr);
    CHECK(store->is_open());
    CHECK(std::filesystem::is_directory(dir / "cortex"));
    store->close();

    // A folder is not a database: open_memory_store() refuses it, so the client can fall back to
    // running without persistence instead of failing to start.
    CHECK(open_memory_store(dir) == nullptr);

    std::filesystem::remove_all(dir);
}

TEST_CASE("Cortex supervisor: a completed validated decision persists a bounded memory entry visible via recent_memories()") {
    auto db_path = std::filesystem::temp_directory_path() / "hailstone_cortex_mem_slice_test.db";
    std::filesystem::remove(db_path);

    auto prov = std::make_shared<MockInferenceProvider>();
    auto mem = std::make_shared<MemoryStore>();
    REQUIRE(mem->open(db_path.string()));

    SupervisorConfig cfg;
    cfg.worker_threads = 1;
    cfg.per_char_cooldown_ms = 0;
    CortexSupervisor sup(prov, mem, cfg);
    sup.set_mode(OperatingMode::Observe);
    sup.set_agent_enabled("ez-server:Tankone", true);
    sup.start();

    SessionSnapshot s = make_snap("Tankone", "WAR", 90, 90);
    std::vector<SessionSnapshot> roster{s};
    sup.submit_observation(observe(s, roster, 5000), true);

    for (int i = 0; i < 100; ++i) {
        if (sup.snapshot().total_decisions >= 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(sup.snapshot().total_decisions == 1);

    // The validated decision must be readable back through the supervisor, with the bounded
    // context it was made on behind it (newest first).
    auto memories = sup.recent_memories("ez-server:Tankone", 8);
    REQUIRE(memories.size() == 2);
    CHECK(memories[0].server_name == "ez-server");
    CHECK(memories[0].character_name == "Tankone");
    CHECK(memories[0].kind == "decision");
    // Bounded decision context only: the validated action and reason code.
    CHECK(memories[0].content == "continue_current_task:nominal");
    CHECK(memories[1].kind == "observation");
    CHECK(memories[1].content.find("Tankone") != std::string::npos);

    // It is a real SQLite row, not just in-process state: a fresh store on the
    // same file sees the same entries.
    sup.stop();
    MemoryStore reopened;
    REQUIRE(reopened.open(db_path.string()));
    auto persisted = reopened.recent_memories("ez-server", "Tankone", 8);
    REQUIRE(persisted.size() == 2);
    CHECK(persisted[0].kind == "decision");
    CHECK(persisted[0].content == "continue_current_task:nominal");

    std::filesystem::remove(db_path);
}

TEST_CASE("Cortex service: a roster tick persists the decision, its observation context and the profile") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "hailstone_cortex_svc_mem";
    std::filesystem::remove_all(dir);

    auto prov = std::make_shared<MockInferenceProvider>();
    // The store and path the client owns (client_app_internal.h), opened through the same seam.
    auto mem = open_memory_store(dir / "cortex" / "memory.db");
    REQUIRE(mem != nullptr);
    REQUIRE(mem->is_open());

    SupervisorConfig cfg;
    cfg.worker_threads = 1;
    cfg.per_char_cooldown_ms = 0;
    CortexService svc(prov, mem, cfg);
    svc.start();

    // The production path: a roster tick, not a forced think.
    SessionSnapshot s = make_snap("Tankone", "WAR", 90, 90);
    svc.tick(std::vector<SessionSnapshot>{s}, now_ms());

    wait_for_decision([&] { return svc.supervisor().snapshot().total_decisions >= 1; });
    REQUIRE(svc.supervisor().snapshot().total_decisions == 1);

    // The bounded decision context, newest first.
    auto memories = mem->recent_memories("ez-server", "Tankone", 8);
    REQUIRE(memories.size() >= 2);
    REQUIRE(memories[0].kind == "decision");
    CHECK(memories[0].content == "continue_current_task:nominal");

    // The observation the decision was made on: one bounded line, what the prompt saw.
    bool saw_observation = false;
    for (const auto& m : memories) {
        if (m.kind != "observation") continue;
        saw_observation = true;
        CHECK(m.content.find("Tankone") != std::string::npos);
        CHECK(m.content.find("qrg") != std::string::npos);
    }
    CHECK(saw_observation);

    // The profile, written the first time this character's identity was seen.
    StoredProfile prof;
    REQUIRE(mem->load_profile("ez-server", "Tankone", prof));
    CHECK(prof.class_short == "WAR");

    svc.stop();
    std::filesystem::remove_all(dir);
}

TEST_CASE("Cortex service: a profile an earlier run left behind keeps its objective") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "hailstone_cortex_profile_keep";
    std::filesystem::remove_all(dir);

    auto mem = open_memory_store(dir / "cortex" / "memory.db");
    REQUIRE(mem != nullptr);
    CharacterIdentity id{"ez-server", "Tankone", "WAR", 70, "tank", "steady"};
    REQUIRE(mem->upsert_profile(id, "farm gnolls"));

    CortexService svc(std::make_shared<MockInferenceProvider>(), mem, SupervisorConfig{});
    svc.start();
    svc.tick(std::vector<SessionSnapshot>{make_snap("Tankone", "WAR", 90, 90)}, now_ms());
    wait_for_decision([&] { return svc.supervisor().snapshot().total_decisions >= 1; });

    StoredProfile prof;
    REQUIRE(mem->load_profile("ez-server", "Tankone", prof));
    // This run's objective is empty (set_objective() is never called), so the new observation must
    // load the stored one back instead of wiping it.
    CHECK(prof.current_objective == "farm gnolls");

    svc.stop();
    std::filesystem::remove_all(dir);
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
