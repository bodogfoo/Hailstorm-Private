#include <doctest/doctest.h>

#if !defined(_WIN32)
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ipc/session_bus.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::ipc;

namespace {

// sockaddr_un::sun_path holds about 107 bytes, so a socket path built from a deep
// temporary folder (a profile dir under a long TMPDIR) cannot be bound at all and
// SessionBusServer::start() just returns false. The bus therefore gets a short name
// of its own; the scratch dir keeps the ordinary files.
std::string short_socket_path() {
    static std::atomic<unsigned> counter{0};
    static const std::uint32_t seed = [] {
        std::random_device rd;
        return static_cast<std::uint32_t>(rd());
    }();
    char name[32];
    std::snprintf(name, sizeof(name), "hsb_%08x_%u.sock", seed, counter++);
    return (std::filesystem::temp_directory_path() / name).string();
}

// Removes the socket file the scratch dir no longer owns.
struct SocketFile {
    explicit SocketFile(std::string p) : path(std::move(p)) {}
    ~SocketFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    SocketFile(const SocketFile&) = delete;
    SocketFile& operator=(const SocketFile&) = delete;
    std::string path;
};

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
    // Braces, not parentheses: `SocketFile f(short_socket_path())` is a function declaration.
    const SocketFile sock_file{short_socket_path()};
    const std::string& sock = sock_file.path;

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

TEST_CASE("SessionBus Commander: tab switch without reconnect, non-exclusive Live/Headless, ACK/failure/timeout tracking, and extended telemetry") {
    test::ScratchDir dir("hs24_bus_commander");
    // Braces, not parentheses: `SocketFile f(short_socket_path())` is a function declaration.
    const SocketFile sock_file{short_socket_path()};
    const std::string& sock = sock_file.path;

    SessionBusServer srv;
    REQUIRE(srv.start(sock));

    SessionBusClient c1, c2, mgr;
    REQUIRE(c1.connect(sock, "slot01"));
    REQUIRE(c2.connect(sock, "slot02"));
    REQUIRE(mgr.connect(sock, "__manager__"));

    app::ExecutionState c1_state = app::ExecutionState::ForegroundFull;
    app::ExecutionState c2_state = app::ExecutionState::BackgroundHeadless;
    c1.set_on_state_control([&](const StateControlPacket& s) { c1_state = s.desired_state; });
    c2.set_on_state_control([&](const StateControlPacket& s) { c2_state = s.desired_state; });

    bool drop_c2_ack = false;
    c1.set_on_command([&](const CommandPacket& cmd) {
        if (cmd.command == "/failme") {
            return CommandAckPacket{cmd.seq, "slot01", false, "target out of range"};
        }
        return CommandAckPacket{cmd.seq, "slot01", true, "executed:" + cmd.command};
    });
    c2.set_on_command([&](const CommandPacket& cmd) {
        if (drop_c2_ack) {
            return CommandAckPacket{0, "", false, ""}; // unused; handled below
        }
        return CommandAckPacket{cmd.seq, "slot02", true, "ok:" + cmd.command};
    });

    SessionSnapshot s1;
    s1.session_id = "slot01";
    s1.character_name = "Stabbinyah";
    s1.server_name = "EZ Server";
    s1.class_short = "ROG";
    s1.level = 75;
    s1.zone_short = "surefall";
    s1.hp_pct = 100;
    s1.exec_state = app::ExecutionState::ForegroundFull;
    s1.combat_state = "Idle";
    s1.script_name = "rgmercs";
    s1.script_state = "running";
    s1.active_profile = "Melee Group";
    s1.follow_target = "Yavaha";
    s1.connection_state = "in_game";
    s1.combat_engine = "NativeHailstone";

    SessionSnapshot s2;
    s2.session_id = "slot02";
    s2.character_name = "Yavaha";
    s2.server_name = "EZ Server";
    s2.class_short = "WAR";
    s2.level = 75;
    s2.zone_short = "surefall";
    s2.hp_pct = 100;
    s2.exec_state = app::ExecutionState::BackgroundHeadless;
    s2.combat_state = "Combat";
    s2.script_name = "rgmercs";
    s2.script_state = "paused";
    s2.active_profile = "Main Tank";
    s2.connection_state = "in_game";
    s2.combat_engine = "LegacyRGMercs";

    REQUIRE(c1.publish_telemetry(s1));
    REQUIRE(c2.publish_telemetry(s2));
    pump_all(srv, c1, c2, mgr);

    // Extended telemetry fields round-trip cleanly into roster snapshots.
    const auto peer1 = mgr.find_peer("Stabbinyah");
    REQUIRE(peer1.has_value());
    CHECK(peer1->script_name == "rgmercs");
    CHECK(peer1->script_state == "running");
    CHECK(peer1->active_profile == "Melee Group");
    CHECK(peer1->follow_target == "Yavaha");
    CHECK(peer1->connection_state == "in_game");
    CHECK(peer1->combat_engine == "NativeHailstone");
    const auto peer2 = mgr.find_peer("Yavaha");
    REQUIRE(peer2.has_value());
    CHECK(peer2->combat_engine == "LegacyRGMercs");

    // 1. Clicking a tab (exclusive foreground) switches live client without disconnecting/reconnecting either client.
    REQUIRE(mgr.request_state_control("Yavaha", app::ExecutionState::ForegroundFull, true));
    pump_all(srv, c1, c2, mgr);
    CHECK(c1.connected());
    CHECK(c2.connected());
    CHECK(c1_state == app::ExecutionState::BackgroundHeadless);
    CHECK(c2_state == app::ExecutionState::ForegroundFull);

    // 2. Non-exclusive Make Live leaves existing live peer live; explicit Make Headless demotes only target.
    REQUIRE(mgr.request_state_control("Stabbinyah", app::ExecutionState::ForegroundFull, false));
    pump_all(srv, c1, c2, mgr);
    CHECK(c1_state == app::ExecutionState::ForegroundFull);
    CHECK(c2_state == app::ExecutionState::ForegroundFull);

    REQUIRE(mgr.request_state_control("Yavaha", app::ExecutionState::BackgroundHeadless, false));
    pump_all(srv, c1, c2, mgr);
    CHECK(c1_state == app::ExecutionState::ForegroundFull);
    CHECK(c2_state == app::ExecutionState::BackgroundHeadless);

    // 3. Command ACK tracking: success ACK, peer failure reason, and 0-peer miss synthetic failure.
    CommandPacket ok_cmd;
    ok_cmd.scope = CommandScope::TargetPeer;
    ok_cmd.target = "Stabbinyah";
    ok_cmd.command = "/combat follow Yavaha";
    const std::uint64_t ok_seq = mgr.send_command(ok_cmd);
    REQUIRE(ok_seq > 0);
    pump_all(srv, c1, c2, mgr);
    const auto ok_tr = mgr.find_tracked_command(ok_seq);
    REQUIRE(ok_tr.has_value());
    CHECK(ok_tr->status == CommandTrackStatus::Acked);
    REQUIRE(ok_tr->acks.size() == 1);
    CHECK(ok_tr->acks[0].ok);
    CHECK(ok_tr->acks[0].message == "executed:/combat follow Yavaha");

    CommandPacket fail_cmd;
    fail_cmd.scope = CommandScope::TargetPeer;
    fail_cmd.target = "Stabbinyah";
    fail_cmd.command = "/failme";
    const std::uint64_t fail_seq = mgr.send_command(fail_cmd);
    REQUIRE(fail_seq > 0);
    pump_all(srv, c1, c2, mgr);
    const auto fail_tr = mgr.find_tracked_command(fail_seq);
    REQUIRE(fail_tr.has_value());
    CHECK(fail_tr->status == CommandTrackStatus::Failed);
    REQUIRE(fail_tr->acks.size() == 1);
    CHECK_FALSE(fail_tr->acks[0].ok);
    CHECK(fail_tr->acks[0].message.find("out of range") != std::string::npos);

    CommandPacket miss_cmd;
    miss_cmd.scope = CommandScope::TargetPeer;
    miss_cmd.target = "NonExistentPeer";
    miss_cmd.command = "/combat stop";
    const std::uint64_t miss_seq = mgr.send_command(miss_cmd);
    REQUIRE(miss_seq > 0);
    pump_all(srv, c1, c2, mgr);
    const auto miss_tr = mgr.find_tracked_command(miss_seq);
    REQUIRE(miss_tr.has_value());
    CHECK(miss_tr->status == CommandTrackStatus::Failed);

    // 4. Timeout tracking when a peer fails to respond within timeout window.
    SessionBusClient silent;
    REQUIRE(silent.connect(sock, "slot03"));
    SessionSnapshot s3;
    s3.session_id = "slot03";
    s3.character_name = "Norosun";
    s3.class_short = "MAG";
    s3.zone_short = "surefall";
    REQUIRE(silent.publish_telemetry(s3));
    for (int i = 0; i < 8; ++i) {
        srv.poll_once(2);
        silent.poll();
        mgr.poll();
    }
    // Deliberately do not poll `silent` so it never sends back an ACK.
    mgr.set_command_timeout_ms(60);
    CommandPacket to_cmd;
    to_cmd.scope = CommandScope::TargetPeer;
    to_cmd.target = "Norosun";
    to_cmd.command = "/combat reloadspells";
    const std::uint64_t to_seq = mgr.send_command(to_cmd);
    REQUIRE(to_seq > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(85));
    srv.poll_once(2);
    mgr.poll();
    const auto to_tr = mgr.find_tracked_command(to_seq);
    REQUIRE(to_tr.has_value());
    CHECK(to_tr->status == CommandTrackStatus::TimedOut);

    silent.disconnect();
    c1.disconnect();
    c2.disconnect();
    mgr.disconnect();
    srv.stop();
}
TEST_CASE("SessionBus IPC: snapshot serialization carries combat_engine and tolerates an older payload") {
    // The commander table shows which engine owns each character, so the field has
    // to survive the wire. It is appended last, which also means a client from
    // before it must still decode: the older blob simply ends before the field.
    SessionSnapshot s;
    s.session_id = "slot02";
    s.character_name = "Yavaha";
    s.server_name = "EZ Server";
    s.class_short = "WAR";
    s.level = 70;
    s.zone_short = "surefall";
    s.online = true;
    s.combat_state = "Combat";
    s.script_name = "rgmercs";
    s.script_state = "paused";
    s.active_profile = "Main Tank";
    s.follow_target = "Yavaha";
    s.connection_state = "in_game";
    s.combat_engine = "NativeHailstone";
    s.updated_ms = 123456;

    const auto blob = encode_snapshot(s);
    SessionSnapshot back;
    REQUIRE(decode_snapshot(blob.data(), blob.size(), back));
    CHECK(back.character_name == "Yavaha");
    CHECK(back.connection_state == "in_game");
    CHECK(back.script_state == "paused");
    CHECK(back.combat_engine == "NativeHailstone");

    // An engine string that was never set writes a zero-length field; dropping
    // those four bytes is exactly the payload of a build without the field.
    SessionSnapshot older = s;
    older.combat_engine.clear();
    auto old_blob = encode_snapshot(older);
    REQUIRE(old_blob.size() > 4);
    old_blob.resize(old_blob.size() - 4);
    SessionSnapshot decoded;
    REQUIRE(decode_snapshot(old_blob.data(), old_blob.size(), decoded));
    CHECK(decoded.character_name == "Yavaha");
    CHECK(decoded.connection_state == "in_game");
    CHECK(decoded.combat_engine.empty());
}
#endif

