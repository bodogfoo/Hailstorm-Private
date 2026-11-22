// The Commander status path for Cortex: a read-only view of the application's
// shared supervisor (health, mode, per-character state) plus the assisted
// proposals awaiting a human, exposed through hs.CommanderStatus().
//
// Nothing in this slice - the view, the service or the Lua table - executes,
// confirms or dispatches a proposal. The pack displays it; a human acts on it.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <lua.hpp>

#include "app/lua_ui.h"
#include "cortex/commander_view.h"
#include "cortex/inference_provider.h"
#include "cortex/runtime.h"
#include "cortex/service.h"
#include "cortex/supervisor.h"
#include "cortex/types.h"
#include "ipc/session_bus.h"

using namespace hailstone;
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

// Decisions arrive on the supervisor's worker threads; poll instead of sleeping blind.
template <typename Pred>
bool wait_until(Pred pred, int attempts = 300) {
    for (int i = 0; i < attempts; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

std::shared_ptr<MockInferenceProvider> make_provider() {
    auto prov = std::make_shared<MockInferenceProvider>();
    prov->set_forced_response(R"({"action":"hold_pull","reason":"healer_mana_low"})");
    return prov;
}

SupervisorConfig tight_config() {
    SupervisorConfig cfg;
    cfg.worker_threads = 1;
    cfg.per_char_cooldown_ms = 0;
    return cfg;
}

const CortexCharacterView* find_char(const CortexStatusView& v, const std::string& key) {
    auto it = std::find_if(v.characters.begin(), v.characters.end(),
                           [&](const CortexCharacterView& c) { return c.key == key; });
    return it == v.characters.end() ? nullptr : &(*it);
}

std::string run_lua(lua_State* L, const std::string& code) {
    if (luaL_dostring(L, code.c_str()) != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        lua_settop(L, 0);
        return "error: " + err;
    }
    std::string out = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
    lua_settop(L, 0);
    return out;
}

} // namespace

// Compile-time, the whole surface: neither the service nor the view has any
// way to run, confirm or dispatch anything, and a proposal carries no command.
// Proposals are data the human acts on somewhere else, so this path cannot
// become an execution path by adding a field to the view alone. (Each check is
// its own template: a requirement that fails on a dependent expression is a
// substitution failure, not a hard error, and one satisfied member must not
// hide another that is still missing.)
template <typename Service>
constexpr bool service_has_execute =
    requires(const Service& s, const cortex::CortexProposalView& p) { s.execute(p); };
template <typename Service>
constexpr bool service_has_confirm =
    requires(const Service& s, const cortex::CortexProposalView& p) { s.confirm(p); };
template <typename Service>
constexpr bool service_has_dispatch =
    requires(const Service& s, const cortex::CortexProposalView& p) { s.dispatch(p); };
template <typename View>
constexpr bool view_carries_command = requires(const View& v) { v.command; };

static_assert(!service_has_execute<CortexService>);
static_assert(!service_has_confirm<CortexService>);
static_assert(!service_has_dispatch<CortexService>);
static_assert(!view_carries_command<CortexStatusView>);
static_assert(!view_carries_command<CortexCharacterView>);
static_assert(!view_carries_command<CortexProposalView>);

TEST_CASE("Cortex commander view: a runtime report maps to Commander display data") {
    auto prov = make_provider();
    SupervisorConfig cfg = tight_config();
    auto sup = std::make_shared<CortexSupervisor>(prov, nullptr, cfg);
    sup->set_mode(OperatingMode::Assisted);
    sup->start();

    std::vector<SessionSnapshot> roster;
    roster.push_back(make_snap("Tankone", "WAR", 90, 60, "Tankone"));
    roster.push_back(make_snap("Healerone", "CLR", 80, 10, "Tankone"));
    auto offline = make_snap("Offliner", "WIZ", 70, 50, "Tankone");
    offline.online = false;
    roster.push_back(offline);

    CortexRuntime rt(sup);
    rt.tick(roster, 5000);
    REQUIRE(wait_until([&] { return sup->snapshot().total_decisions >= 2; }));

    const RuntimeReport rep = rt.report();
    REQUIRE(rep.proposals.size() == 2);

    const CortexStatusView v = to_commander_view(rep, true);

    // Supervisor health and mode, as they are now.
    CHECK(v.available);
    CHECK(v.running);
    CHECK(v.mode == OperatingMode::Assisted);
    CHECK(v.inference_healthy);
    CHECK(v.model_name == prov->model_name());
    CHECK(v.endpoint == "mock://local");
    CHECK_FALSE(v.emergency_stop);
    CHECK(v.total_requests == rep.supervisor.total_requests);
    CHECK(v.total_decisions == rep.supervisor.total_decisions);
    CHECK(v.total_errors == rep.supervisor.total_errors);
    CHECK(v.pending_queue_size == rep.supervisor.pending_queue_size);

    // The tick it describes: the roster whole, the online named characters in it.
    CHECK(v.observed_ms == rep.observed_ms);
    CHECK(v.observed_ms == 5000);
    CHECK(v.roster_size == rep.roster_size);
    CHECK(v.roster_size == 3);
    CHECK(v.online_count == rep.online_count);
    CHECK(v.online_count == 2);

    // Per-character state, straight from the supervisor's agents.
    REQUIRE(v.characters.size() == 2);
    const CortexCharacterView* tank = find_char(v, "ez-server:Tankone");
    REQUIRE(tank != nullptr);
    const auto snap = sup->snapshot();
    REQUIRE(snap.agents.size() == 2);
    const auto agent = std::find_if(snap.agents.begin(), snap.agents.end(),
                                    [](const AgentSnapshot& a) { return a.key == "ez-server:Tankone"; });
    REQUIRE(agent != snap.agents.end());
    CHECK(tank->character_name == "Tankone");
    CHECK(tank->server_name == "ez-server");
    CHECK(tank->enabled);
    CHECK(tank->mode == OperatingMode::Assisted);
    CHECK(tank->status == agent->status);
    CHECK(std::string(to_string(tank->status)) != "");
    CHECK(tank->objective == agent->current_objective);
    CHECK(tank->intention == agent->current_intention);
    CHECK(tank->has_decision == agent->latest_decision.valid);
    CHECK(tank->proposal_pending); // a proposal awaits a human for this character
    const CortexCharacterView* healer = find_char(v, "ez-server:Healerone");
    REQUIRE(healer != nullptr);
    CHECK(healer->proposal_pending);
    CHECK(find_char(v, "ez-server:Offliner") == nullptr); // offline: not an agent

    // The proposals the pack shows: allowlisted, awaiting confirmation, and
    // never carrying anything executable.
    REQUIRE(v.proposals.size() == 2);
    for (const auto& p : v.proposals) {
        CHECK_FALSE(p.executed);
        CHECK(p.awaiting_confirmation);
        CHECK(parse_intention(p.action) == p.intention);
        CHECK(p.action == to_string(p.intention));
        CHECK(p.reason_code == "healer_mana_low");
        CHECK_FALSE(p.character_name.empty());
        CHECK(p.key == "ez-server:" + p.character_name);
        // every proposal belongs to a character shown next to it
        const CortexCharacterView* owner = find_char(v, p.key);
        REQUIRE(owner != nullptr);
        CHECK(owner->proposal_pending);
    }

    sup->stop();
}

TEST_CASE("CortexService: the application-owned runtime reports state and executes nothing") {
    auto prov = make_provider();
    CortexService svc(prov, nullptr, tight_config());

    // Before the first tick the service is wired but has observed nothing.
    {
        const CortexStatusView v0 = svc.view();
        CHECK(v0.available);
        CHECK_FALSE(v0.running);
        CHECK(v0.mode == OperatingMode::Observe); // nothing may act on its own
        CHECK(v0.characters.empty());
        CHECK(v0.proposals.empty());
    }

    svc.start();
    CHECK(svc.view().running);

    std::vector<SessionSnapshot> roster;
    roster.push_back(make_snap("Tankone", "WAR", 90, 60, "Tankone"));

    // Observe mode: the per-character state reaches the view, nothing is proposed.
    {
        const CortexStatusView v1 = svc.tick(roster, 5000);
        CHECK(v1.running);
        CHECK(v1.mode == OperatingMode::Observe);
        CHECK(v1.observed_ms == 5000);
        CHECK(v1.roster_size == 1);
        REQUIRE(v1.characters.size() == 1);
        CHECK(v1.characters[0].key == "ez-server:Tankone");
        CHECK(v1.characters[0].enabled);
        CHECK(v1.proposals.empty());
        CHECK_FALSE(v1.characters[0].proposal_pending);
    }
    REQUIRE(wait_until([&] { return svc.supervisor().snapshot().total_decisions >= 1; }));

    // Assisted mode: the same observation becomes a proposal held for a human.
    svc.supervisor().set_mode(OperatingMode::Assisted);
    {
        const CortexStatusView v2 = svc.tick(roster, 6000);
        CHECK(v2.mode == OperatingMode::Assisted);
        REQUIRE(v2.proposals.size() == 1);
        CHECK_FALSE(v2.proposals[0].executed);
        CHECK(v2.proposals[0].awaiting_confirmation);
        REQUIRE(v2.characters.size() == 1);
        CHECK(v2.characters[0].proposal_pending);
        CHECK(v2.inference_healthy);
        CHECK(v2.endpoint == "mock://local");
    }

    // An emergency stop is visible in the view and hides every proposal.
    svc.supervisor().trigger_emergency_stop();
    {
        const CortexStatusView v3 = svc.tick(roster, 7000);
        CHECK(v3.emergency_stop);
        CHECK(v3.proposals.empty());
    }
    svc.supervisor().clear_emergency_stop();
    {
        const CortexStatusView v4 = svc.tick(roster, 8000);
        CHECK_FALSE(v4.emergency_stop);
        REQUIRE(v4.proposals.size() == 1);
        CHECK_FALSE(v4.proposals[0].executed);
    }

    svc.stop();
    CHECK_FALSE(svc.view().running);
    CHECK(svc.view().available); // still wired: the pack can show it as stopped
}

TEST_CASE("CommanderStatus: the UI receives the cortex view with the schema it had") {
    auto prov = make_provider();
    CortexService svc(prov, nullptr, tight_config());
    svc.start();
    std::vector<SessionSnapshot> roster;
    roster.push_back(make_snap("Tankone", "WAR", 90, 60, "Tankone"));
    svc.tick(roster, 5000);
    REQUIRE(wait_until([&] { return svc.supervisor().snapshot().total_decisions >= 1; }));
    svc.supervisor().set_mode(OperatingMode::Assisted);
    const CortexStatusView view = svc.tick(roster, 6000);
    REQUIRE(view.proposals.size() == 1);
    const std::string expected_status = to_string(svc.supervisor().snapshot().agents[0].status);
    svc.stop();

    app::UiHost host;
    host.in_game = true;
    host.ui_ready = true;
    host.commander.status = [&]() {
        app::UiHost::CommanderStatusSnapshot snap;
        snap.bus_connected = true;
        snap.session_id = "slot01";
        snap.character_name = "Tankone";
        snap.execution_state = "ForegroundFull";
        snap.cortex = view;
        SessionSnapshot p1;
        p1.session_id = "slot01";
        p1.character_name = "Tankone";
        p1.class_short = "WAR";
        p1.online = true;
        snap.peers.push_back(p1);
        return snap;
    };

    app::UiApi api(host);
    std::unique_ptr<lua_State, decltype(&lua_close)> lua_guard(luaL_newstate(), &lua_close);
    lua_State* L = lua_guard.get();
    REQUIRE(L != nullptr);
    luaL_openlibs(L);
    lua_newtable(L);
    api.register_functions(L);
    lua_setglobal(L, "hs");

    // The schema the pack already reads is untouched.
    CHECK(run_lua(L, "local c = hs.CommanderStatus() "
                     "return c.characterName .. ':' .. tostring(c.busConnected) .. ':' .. #c.peers .. ':' .. c.executionState")
          == "Tankone:true:1:ForegroundFull");
    CHECK(run_lua(L, "local c = hs.CommanderStatus() return c.sessionId .. '|' .. c.activeProfile") == "slot01|");

    // Supervisor health and mode.
    CHECK(run_lua(L, "local cx = hs.CommanderStatus().cortex "
                     "return tostring(cx.available) .. '|' .. tostring(cx.running) .. '|' .. cx.mode")
          == "true|true|CORTEX_ASSISTED");
    CHECK(run_lua(L, "local cx = hs.CommanderStatus().cortex "
                     "return cx.endpoint .. '|' .. tostring(cx.inferenceHealthy) .. '|' .. cx.modelName")
          == "mock://local|true|mock-cortex-1.0");
    CHECK(run_lua(L, "local cx = hs.CommanderStatus().cortex "
                     "return tostring(cx.emergencyStop) .. '|' .. tostring(cx.pendingQueue) .. '|' .. "
                     "tostring(cx.totalDecisions > 0)")
          == "false|0|true");

    // Per-character state.
    CHECK(run_lua(L, "local ch = hs.CommanderStatus().cortex.characters[1] "
                     "return ch.key .. '|' .. ch.characterName .. '|' .. tostring(ch.enabled) .. '|' .. "
                     "tostring(ch.proposalPending) .. '|' .. ch.mode")
          == "ez-server:Tankone|Tankone|true|true|CORTEX_ASSISTED");
    CHECK(run_lua(L, "return hs.CommanderStatus().cortex.characters[1].brainStatus") == expected_status);

    // The proposal as display data: no command, nothing executed.
    CHECK(run_lua(L, "local p = hs.CommanderStatus().cortex.proposals[1] "
                     "return p.action .. '|' .. tostring(p.executed) .. '|' .. tostring(p.awaitingConfirmation)")
          == "hold_pull|false|true");
    CHECK(run_lua(L, "local p = hs.CommanderStatus().cortex.proposals[1] return tostring(p.command)") == "nil");
    CHECK(run_lua(L, "return #hs.CommanderStatus().cortex.proposals") == "1");

    // Without a status callback the pack still gets the same schema, empty.
    host.commander.status = nullptr;
    CHECK(run_lua(L, "local c = hs.CommanderStatus() "
                     "return tostring(c.available) .. '|' .. tostring(c.cortex.available) .. '|' .. "
                     "#c.cortex.characters .. '|' .. #c.cortex.proposals .. '|' .. c.cortex.mode")
          == "false|false|0|0|CORTEX_OBSERVE");
}
