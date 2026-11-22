#include "doctest.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cortex/inference_provider.h"
#include "cortex/observation.h"
#include "cortex/runtime.h"
#include "cortex/supervisor.h"
#include "ipc/session_bus.h"

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

const CharacterObservation* find_obs(const RuntimeReport& rep, const std::string& key) {
    auto it = std::find_if(rep.observations.begin(), rep.observations.end(),
                           [&](const CharacterObservation& o) {
                               return o.server_name + ":" + o.character_name == key;
                           });
    return it == rep.observations.end() ? nullptr : &(*it);
}

bool has_agent(const SupervisorSnapshot& snap, const std::string& key) {
    return std::any_of(snap.agents.begin(), snap.agents.end(),
                       [&](const AgentSnapshot& a) { return a.key == key; });
}

// The supervisor's decisions arrive on worker threads; poll instead of sleeping blind.
template <typename Pred>
bool wait_until(Pred pred, int attempts = 300) {
    for (int i = 0; i < attempts; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

std::shared_ptr<CortexSupervisor> make_supervisor(std::shared_ptr<MockInferenceProvider> prov,
                                                  OperatingMode mode) {
    SupervisorConfig cfg;
    cfg.worker_threads = 2;
    cfg.per_char_cooldown_ms = 0;
    auto sup = std::make_shared<CortexSupervisor>(prov, nullptr, cfg);
    sup->set_mode(mode);
    sup->start();
    return sup;
}

} // namespace

TEST_CASE("Cortex runtime: online session-bus telemetry reaches the shared supervisor") {
    auto prov = std::make_shared<MockInferenceProvider>();
    auto sup = make_supervisor(prov, OperatingMode::Observe);

    std::vector<SessionSnapshot> roster;
    roster.push_back(make_snap("Tankone", "WAR", 90, 60, "Tankone"));    // the leader
    roster.push_back(make_snap("Healerone", "CLR", 80, 10, "Tankone"));  // follower, low mana
    auto offline = make_snap("Offliner", "WIZ", 70, 50, "Tankone");
    offline.online = false;
    roster.push_back(offline);
    roster.push_back(make_snap("", "ROG", 70, 50, "Tankone")); // online but unnamed

    CortexRuntime rt(sup);
    auto rep = rt.tick(roster, 5000);

    // Only named, online characters are observed; the roster itself is counted whole.
    CHECK(rep.roster_size == 4);
    CHECK(rep.online_count == 2);
    REQUIRE(rep.observations.size() == 2);

    // The observation is the roster's telemetry, not something the runtime invented.
    auto tank = find_obs(rep, "ez-server:Tankone");
    REQUIRE(tank != nullptr);
    CHECK(tank->observed_ms == 5000);
    CHECK(tank->zone_short == "qrg");
    CHECK(tank->hp_pct == 90);
    CHECK(tank->combat_state == "Combat");
    CHECK(tank->self_is_leader);
    REQUIRE(tank->group.size() == 1);
    CHECK(tank->group[0].character_name == "Healerone");
    CHECK(tank->group[0].mana_pct == 10);

    auto healer = find_obs(rep, "ez-server:Healerone");
    REQUIRE(healer != nullptr);
    CHECK(healer->group.size() == 1);
    CHECK(healer->group[0].character_name == "Tankone");

    // Observe mode proposes nothing.
    CHECK(rep.proposals.empty());

    // Both online characters became enabled agents of the one shared supervisor;
    // the offline and the unnamed entries did not.
    auto snap = sup->snapshot();
    REQUIRE(snap.agents.size() == 2);
    CHECK(has_agent(snap, "ez-server:Tankone"));
    CHECK(has_agent(snap, "ez-server:Healerone"));
    CHECK_FALSE(has_agent(snap, "ez-server:Offliner"));
    for (const auto& agent : snap.agents) {
        CHECK(agent.enabled);
        CHECK(agent.status != BrainStatus::Disabled);
        CHECK(agent.mode == OperatingMode::Observe);
        CHECK(agent.group_leader == "Tankone");
        CHECK_FALSE(agent.identity.character_name.empty());
        CHECK(agent.identity.server_name == "ez-server");
    }

    // The submitted observations reached inference: decisions came back.
    REQUIRE(wait_until([&] { return sup->snapshot().total_decisions >= 2; }));
    auto rep2 = rt.report();
    CHECK(rep2.supervisor.total_decisions >= 2);
    CHECK(rep2.proposals.empty());

    sup->stop();
}

TEST_CASE("Cortex runtime: assisted mode proposes allowlisted intentions and never executes them") {
    auto prov = std::make_shared<MockInferenceProvider>();
    prov->set_forced_response(R"({"action":"hold_pull","reason":"healer_mana_low"})");
    auto sup = make_supervisor(prov, OperatingMode::Assisted);

    std::vector<SessionSnapshot> roster;
    roster.push_back(make_snap("Tankone", "WAR", 90, 60, "Tankone"));
    roster.push_back(make_snap("Healerone", "CLR", 80, 10, "Tankone"));

    CortexRuntime rt(sup);
    rt.tick(roster, 5000);
    REQUIRE(wait_until([&] { return sup->snapshot().total_decisions >= 2; }));

    auto rep = rt.report();
    REQUIRE(rep.proposals.size() == 2);

    // One proposal per online character, sorted by key, each a closed-allowlist
    // intention held for confirmation.
    CHECK(rep.proposals[0].key <= rep.proposals[1].key);
    for (const auto& p : rep.proposals) {
        CHECK((p.key == "ez-server:Tankone" || p.key == "ez-server:Healerone"));
        CHECK(p.intention == Intention::HoldPull);
        CHECK(p.action == "hold_pull");
        CHECK(p.action == to_string(p.intention));
        CHECK(parse_intention(p.action) == p.intention);
        CHECK(p.reason_code == "healer_mana_low");
        CHECK(p.character_name == p.key.substr(p.key.find(':') + 1));
        CHECK(p.awaiting_confirmation);
        CHECK_FALSE(p.executed);
        CHECK(p.arbitration_owner == "rgmercs");
        CHECK_FALSE(p.arbitration_reason.empty());
    }

    // Observe mode shows no proposals even with a decision ready.
    sup->set_mode(OperatingMode::Observe);
    CHECK(rt.report().proposals.empty());

    // Assisted proposals come back; an emergency stop hides every one of them.
    sup->set_mode(OperatingMode::Assisted);
    REQUIRE(rt.report().proposals.size() == 2);
    sup->trigger_emergency_stop();
    auto stopped = rt.report();
    CHECK(stopped.proposals.empty());
    CHECK(stopped.supervisor.emergency_stop);
    sup->clear_emergency_stop();
    REQUIRE(rt.report().proposals.size() == 2);

    sup->stop();
}

TEST_CASE("Cortex runtime: a non-allowlisted model action is never proposed") {
    auto prov = std::make_shared<MockInferenceProvider>();
    prov->set_forced_response(R"({"action":"rm_rf_root","reason":"bad"})");
    auto sup = make_supervisor(prov, OperatingMode::Assisted);

    std::vector<SessionSnapshot> roster;
    roster.push_back(make_snap("Tankone", "WAR", 90, 60, "Tankone"));

    CortexRuntime rt(sup);
    rt.tick(roster, 5000);
    REQUIRE(wait_until([&] { return sup->snapshot().total_errors >= 1; }));

    auto rep = rt.report();
    CHECK(rep.proposals.empty());
    REQUIRE(rep.supervisor.agents.size() == 1);
    CHECK(rep.supervisor.agents[0].status == BrainStatus::Error);
    CHECK(rep.supervisor.agents[0].last_error.rfind("unsupported_action:", 0) == 0);

    sup->stop();
}

TEST_CASE("Cortex runtime: a character that leaves the roster is no longer proposed") {
    auto prov = std::make_shared<MockInferenceProvider>();
    prov->set_forced_response(R"({"action":"follow_group","reason":"leader_moved"})");
    auto sup = make_supervisor(prov, OperatingMode::Assisted);

    std::vector<SessionSnapshot> roster;
    roster.push_back(make_snap("Tankone", "WAR", 90, 60, "Tankone"));

    CortexRuntime rt(sup);
    rt.tick(roster, 5000);
    REQUIRE(wait_until([&] { return sup->snapshot().total_decisions >= 1; }));
    REQUIRE(rt.report().proposals.size() == 1);

    // The bus no longer reports the character: nothing of it is proposed, even
    // though the supervisor still holds its last valid decision.
    auto rep = rt.tick(std::vector<SessionSnapshot>{}, 6000);
    CHECK(rep.online_count == 0);
    CHECK(rep.observations.empty());
    CHECK(rep.proposals.empty());
    REQUIRE(rep.supervisor.agents.size() == 1);
    CHECK(rep.supervisor.agents[0].latest_decision.valid);

    sup->stop();
}
