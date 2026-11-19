#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ipc/session_bus.h"
#include "mq/commands.h"
#include "mq/lua_mq.h"
#include "mq/lua_script.h"
#include "mq/parse.h"
#include "mq/tlo.h"
#include "mq_test_world.h"
#include "plugin/hooks.h"
#include "plugin/policy.h"

#ifndef HAILSTONE_SOURCE_DIR
#define HAILSTONE_SOURCE_DIR "."
#endif

namespace fs = std::filesystem;
using namespace hailstone;

TEST_CASE("RG Mercs compatibility: TLOs, Lua compat modules (lsqlite3/lfs/actors/bit32), and IPC commands") {
    mq_test::World world{"hailstone_rgmercs_test"};
    const fs::path lua_dir = world.dir / "lua";
    const fs::path cfg_dir = world.dir / "config";
    fs::create_directories(lua_dir);
    fs::create_directories(cfg_dir);

    const fs::path src_script = fs::path(HAILSTONE_SOURCE_DIR) / "tests/hailstone24/rgmercs_validation.lua";
    REQUIRE(fs::exists(src_script));
    fs::copy_file(src_script, lua_dir / "rgmercs_validation.lua", fs::copy_options::overwrite_existing);

#if !defined(_WIN32)
    ipc::SessionBusServer bus_server;
    const std::string sock = (world.dir / "rg_bus.sock").string();
    REQUIRE(bus_server.start(sock));

    ipc::SessionBusClient client_self, client_peer;
    REQUIRE(client_self.connect(sock, "slot01"));
    REQUIRE(client_peer.connect(sock, "slot02"));
#endif

    plugin::Hooks hooks;
    plugin::Policy policy;
    plugin::Rules rules;
    rules.configured = true;
    rules.player_script_caps = plugin::kCapAll;
    rules.social_mode = plugin::ButtonMode::Off;
    policy.set(rules);

    mq::Registry reg;
    mq::register_game_tlos(reg);

    mq::Context ctx;
    ctx.registry = &reg;
    ctx.conn = world.conn.get();
    ctx.server_name = "TestServer";
    ctx.lua_dir = lua_dir.string();
    ctx.active_script = "rgmercs_validation";
    ctx.foreground = true;
#if !defined(_WIN32)
    ctx.ipc_bus = &client_self;
#endif

    std::vector<std::string> printed;
    std::vector<std::string> dispatched_peer_cmds;
#if !defined(_WIN32)
    client_peer.set_on_command([&](const ipc::CommandPacket& cmd) {
        dispatched_peer_cmds.push_back(cmd.command);
        return ipc::CommandAckPacket{cmd.seq, "slot02", true, "ok"};
    });
    ipc::SessionSnapshot snap1;
    snap1.session_id = "slot01";
    snap1.character_name = "Tester";
    snap1.server_name = "TestServer";
    snap1.class_short = "CLR";
    snap1.zone_short = "qeynos2";
    snap1.group_leader = "Groupie";
    snap1.exec_state = app::ExecutionState::ForegroundFull;
    client_self.publish_telemetry(snap1);

    ipc::SessionSnapshot snap2;
    snap2.session_id = "slot02";
    snap2.character_name = "Groupie";
    snap2.server_name = "TestServer";
    snap2.class_short = "WAR";
    snap2.zone_short = "qeynos2";
    snap2.group_leader = "Groupie";
    snap2.exec_state = app::ExecutionState::BackgroundHeadless;
    client_peer.publish_telemetry(snap2);
#endif

    mq::CommandEnv ce;
    ce.hooks = &hooks;
    ce.registry = &reg;
    ce.context = [&]() -> const mq::Context& { return ctx; };
    ce.echo = [&](const std::string& s) { printed.push_back(s); };
    ce.run = [&](const std::string& line) {
        std::string l = line;
        (void)hooks.command(l);
    };
#if !defined(_WIN32)
    ce.ipc_bus = &client_self;
#endif
    mq::Commands commands(ce);

    std::int64_t now_ms = 0;
    mq::ScriptEnv se;
    se.hooks = &hooks;
    se.policy = &policy;
    se.registry = &reg;
    se.context = ce.context;
    se.print = ce.echo;
    se.run = ce.run;
    se.dirs = {lua_dir};
    se.config_dir = cfg_dir;
    se.now_ms = [&] { return now_ms; };
#if !defined(_WIN32)
    se.ipc_bus = &client_self;
#endif

    mq::ScriptHost host(se);

    // TLO assertions prior to script run
    CHECK(mq::parse_macro_data(ctx, "${Me.AAPoints}") == "7");
    CHECK(mq::parse_macro_data(ctx, "${Me.AmIGroupLeader}") == "FALSE");
    CHECK(mq::parse_macro_data(ctx, "${Group.Leader}") == "Groupie");
    CHECK(mq::parse_macro_data(ctx, "${EverQuest.Foreground}") == "TRUE");
    CHECK(mq::parse_macro_data(ctx, "${Zone.ShortName}") == "qeynos2");

    std::string why;
    REQUIRE(host.run("rgmercs_validation", {}, &why));
    for (int i = 0; i < 60; ++i) {
        now_ms += 10;
#if !defined(_WIN32)
        bus_server.poll_once(2);
        client_self.poll();
        client_peer.poll();
#endif
        host.pulse();
        if (!host.running("rgmercs_validation")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto list = host.list();
    REQUIRE_FALSE(list.empty());
    CHECK(list[0].status == "EXITED");
    CHECK(list[0].error.empty());

    bool saw_pass = false;
    for (const auto& line : printed) {
        if (line.find("RGMERCS_SMOKE_PASS") != std::string::npos) saw_pass = true;
    }
    CHECK(saw_pass);

#if !defined(_WIN32)
    // Verify /bca and /rc IPC dispatch through Commands.
    std::string bca_cmd = "/bca //sit";
    hooks.command(bca_cmd);
    std::string rc_cmd = "/rc Groupie /attack on";
    hooks.command(rc_cmd);
    for (int i = 0; i < 15; ++i) {
        bus_server.poll_once(2);
        client_self.poll();
        client_peer.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(dispatched_peer_cmds.size() == 2);
    CHECK(dispatched_peer_cmds[0] == "/sit");
    CHECK(dispatched_peer_cmds[1] == "/attack on");

    CHECK(mq::parse_macro_data(ctx, "${DanNet.Peers}") != "");
    client_self.disconnect();
    client_peer.disconnect();
    bus_server.stop();
#endif
}
