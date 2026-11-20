#include <doctest/doctest.h>

#if !defined(_WIN32)
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "ipc/session_bus.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::ipc;

namespace {

void pump_all(SessionBusServer& srv, SessionBusClient& a, SessionBusClient& b, SessionBusClient& mgr, int rounds = 12) {
    for (int i = 0; i < rounds; ++i) {
        srv.poll_once(2);
        a.poll();
        b.poll();
        mgr.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace

TEST_CASE("SessionBus IPC: roster, /bca command dispatch, actors mailbox, and foreground switch") {
    test::ScratchDir dir("hs24_bus_test");
    const std::string sock = (dir.path() / "bus.sock").string();

    SessionBusServer srv;
    REQUIRE(srv.start(sock));

    SessionBusClient c1, c2, mgr;
    REQUIRE(c1.connect(sock, "slot01"));
    REQUIRE(c2.connect(sock, "slot02"));
    REQUIRE(mgr.connect(sock, "__manager__"));

    std::vector<std::string> c1_cmds, c2_cmds, mgr_cmds;
    std::vector<ActorPacket> c2_actors;
    app::ExecutionState c1_state = app::ExecutionState::ForegroundFull;
    app::ExecutionState c2_state = app::ExecutionState::BackgroundHeadless;

    c1.set_on_command([&](const CommandPacket& cmd) {
        c1_cmds.push_back(cmd.command);
        return CommandAckPacket{cmd.seq, "slot01", true, "ok"};
    });
    c2.set_on_command([&](const CommandPacket& cmd) {
        c2_cmds.push_back(cmd.command);
        return CommandAckPacket{cmd.seq, "slot02", true, "ok"};
    });
    mgr.set_on_command([&](const CommandPacket& cmd) {
        mgr_cmds.push_back(cmd.command);
        return CommandAckPacket{cmd.seq, "__manager__", true, "ok"};
    });
    c2.set_on_actor([&](const ActorPacket& pkt) { c2_actors.push_back(pkt); });
    c1.set_on_state_control([&](const StateControlPacket& s) { c1_state = s.desired_state; });
    c2.set_on_state_control([&](const StateControlPacket& s) { c2_state = s.desired_state; });

    SessionSnapshot s1;
    s1.session_id = "slot01";
    s1.character_name = "TankOne";
    s1.server_name = "EZ";
    s1.class_short = "WAR";
    s1.level = 75;
    s1.zone_short = "potimeb";
    s1.hp_pct = 98;
    s1.exec_state = app::ExecutionState::ForegroundFull;

    SessionSnapshot s2;
    s2.session_id = "slot02";
    s2.character_name = "ClericTwo";
    s2.server_name = "EZ";
    s2.class_short = "CLR";
    s2.level = 75;
    s2.zone_short = "potimeb";
    s2.hp_pct = 100;
    s2.mana_pct = 85;
    s2.exec_state = app::ExecutionState::BackgroundHeadless;

    CHECK(c1.publish_telemetry(s1));
    CHECK(c2.publish_telemetry(s2));
    pump_all(srv, c1, c2, mgr);

    // Controller (__manager__) must not appear as a game slot in roster().
    const auto roster = srv.roster();
    REQUIRE(roster.size() == 2);
    CHECK(mgr.cached_roster().size() == 2);

    // /bca from c1 -> delivers only to c2, never c1 or __manager__.
    CommandPacket bca;
    bca.scope = CommandScope::AllExceptSelf;
    bca.sender_char = "TankOne";
    bca.sender_zone = "potimeb";
    bca.command = "/sit";
    CHECK(c1.send_command(bca) > 0);
    pump_all(srv, c1, c2, mgr);
    CHECK(c1_cmds.empty());
    REQUIRE(c2_cmds.size() == 1);
    CHECK(c2_cmds[0] == "/sit");
    CHECK(mgr_cmds.empty());
    CHECK_FALSE(srv.take_acks().empty());

    // Actor mailbox routing from c1 -> ClericTwo.
    ActorPacket ap;
    ap.sender_session = "slot01";
    ap.sender_char = "TankOne";
    ap.mailbox = "combat_mode";
    ap.target_char = "ClericTwo";
    ap.payload_json = R"({"heal":true,"hp":42})";
    CHECK(c1.send_actor(ap));
    pump_all(srv, c1, c2, mgr);
    REQUIRE(c2_actors.size() == 1);
    CHECK(c2_actors[0].mailbox == "combat_mode");
    CHECK(c2_actors[0].payload_json == ap.payload_json);

    // Exclusive foreground switch via manager client -> ClericTwo becomes FG, TankOne becomes BG_HEADLESS.
    CHECK(mgr.request_switch_foreground("ClericTwo"));
    pump_all(srv, c1, c2, mgr);
    CHECK(c1_state == app::ExecutionState::BackgroundHeadless);
    CHECK(c2_state == app::ExecutionState::ForegroundFull);

    c1.disconnect();
    c2.disconnect();
    mgr.disconnect();
    srv.stop();
}
#endif
