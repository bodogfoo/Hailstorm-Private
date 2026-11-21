// Native Hailstone RGMercs UI, Commander Lua API & bridge boundary tests:
// 1. Per-character settings persistence inside Hailstone data_dir + read-only SQLite/Lua import,
//    malformed numeric conversion safety & warnings, and apostrophe character name matching ("Noro's Son").
// 2. Sandbox enforcement (rejects imports or writes outside canonical data_dir).
// 3. ScriptHost + RGMercsBridge boundary: hailstone.rgmercs adapter, error banner preservation, mq.imgui suppression.
// 4. hs.RGMercs* & hs.Commander* Lua adapters, ui_call_caps gating, CascadeMenu row gating, and modern Lua UI validation.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>
#include <lua.hpp>
#include <sqlite3.h>

#include "app/lua_ui.h"
#include "app/lua_ui_caps.h"
#include "app/lua_ui_menu.h"
#include "app/lua_ui_plugins.h"
#include "mq/commands.h"
#include "mq/lua_mq.h"
#include "mq/rgmercs_bridge.h"
#include "mq/tlo.h"
#include "mq_test_world.h"
#include "plugin/hooks.h"
#include "plugin/policy.h"
#include "scratch_dir.h"

#ifndef HAILSTONE_SOURCE_DIR
#define HAILSTONE_SOURCE_DIR "."
#endif

namespace fs = std::filesystem;
using namespace hailstone;

namespace {

void create_sample_rgmercs_sqlite(const fs::path& db_path) {
    fs::create_directories(db_path.parent_path());
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(db_path.string().c_str(), &db) == SQLITE_OK);
    const char* ddl =
        "CREATE TABLE IF NOT EXISTS CoreConfig (section TEXT, key TEXT, value TEXT);"
        "INSERT INTO CoreConfig (section, key, value) VALUES "
        "('Combat', 'DoMelee', 'false'),"
        "('Targeting', 'AssistRange', '85.0'),"
        "('Healing', 'MaxHealPct', '72'),"
        "('Movement', 'PullRange', 'not_a_number'),"
        "('Buffs', 'BuffManaPct', 'NaN'),"
        "('Targeting', 'AutoAssistAt', '99999');";
    char* err = nullptr;
    const int rc = sqlite3_exec(db, ddl, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    sqlite3_close(db);
    REQUIRE(rc == SQLITE_OK);
}

std::string read_file_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return out;
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

TEST_CASE("RGMercsBridge: read-only SQLite import, malformed numeric safety, apostrophe names, and sandbox boundaries") {
    test::ScratchDir root("rgmercs_bridge");
    const fs::path data_dir = root.path() / "hailstone_data";
    const fs::path char_dir = data_dir / "characters" / "ez_server_stabbinyah";
    const fs::path external_dir = root.path() / "outside";
    fs::create_directories(char_dir);
    fs::create_directories(external_dir);

    const fs::path db_path = data_dir / "scripts" / "config" / "rgmercs" / "ez_server_stabbinyah.db";
    create_sample_rgmercs_sqlite(db_path);
    const std::string db_before = read_file_bytes(db_path);

    const fs::path outside_db = external_dir / "escape.db";
    create_sample_rgmercs_sqlite(outside_db);

    {
        mq::RGMercsBridge bridge;
        bridge.attach_character(data_dir, char_dir, "Stabbinyah", "EZ Server", "ROG");

        const auto snap = bridge.snapshot();
        CHECK(snap.character == "Stabbinyah");
        CHECK(snap.server == "EZ Server");
        CHECK(snap.class_name == "ROG");
        CHECK(snap.imported_count >= 3);
        REQUIRE_FALSE(snap.available_dbs.empty());

        const auto do_melee = bridge.get_setting("DoMelee");
        REQUIRE(do_melee.has_value());
        CHECK(do_melee->value == "false");
        CHECK(do_melee->imported);

        // Float-formatted integer "85.0" converts cleanly to "85".
        const auto assist_range = bridge.get_setting("AssistRange");
        REQUIRE(assist_range.has_value());
        CHECK(assist_range->value == "85");

        const auto heal_pct = bridge.get_setting("MaxHealPct");
        REQUIRE(heal_pct.has_value());
        CHECK(heal_pct->value == "72");

        // Malformed numeric values ("not_a_number", "NaN") fall back safely to default and emit warnings.
        const auto pull_range = bridge.get_setting("PullRange");
        REQUIRE(pull_range.has_value());
        CHECK(pull_range->value == pull_range->default_value);

        const auto buff_mana = bridge.get_setting("BuffManaPct");
        REQUIRE(buff_mana.has_value());
        CHECK(buff_mana->value == buff_mana->default_value);

        // Out-of-range numeric "99999" is safely clamped to max_val (100).
        const auto auto_assist = bridge.get_setting("AutoAssistAt");
        REQUIRE(auto_assist.has_value());
        CHECK(auto_assist->value == "100");

        // Re-running explicit import surfaces warnings for malformed numbers.
        const auto reimp = bridge.import_from_path(db_path);
        CHECK(reimp.ok);
        CHECK(reimp.warnings.size() >= 2);

        // SQLite source file must never be mutated.
        CHECK(read_file_bytes(db_path) == db_before);

        // Sandbox: refuse imports outside data_dir.
        const auto bad_import = bridge.import_from_path(outside_db);
        CHECK_FALSE(bad_import.ok);

        // Modify a setting and save to per-character INI inside char_dir.
        CHECK(bridge.set_setting("PullRange", "215", "Movement", true));
        REQUIRE(bridge.save_settings());
        CHECK(fs::exists(char_dir / "rgmercs_settings.ini"));
    }

    // Apostrophe character name matching ("Noro's Son" -> matches ez_server_noros_son.db).
    {
        const fs::path noro_db = data_dir / "scripts" / "config" / "rgmercs" / "ez_server_noros_son.db";
        create_sample_rgmercs_sqlite(noro_db);
        const fs::path noro_dir = data_dir / "characters" / "ez_server_noros_son";
        fs::create_directories(noro_dir);

        mq::RGMercsBridge noro_bridge;
        noro_bridge.attach_character(data_dir, noro_dir, "Noro's Son", "EZ Server", "WIZ");
        CHECK(noro_bridge.snapshot().imported_count >= 3);
        const auto ar = noro_bridge.get_setting("AssistRange");
        REQUIRE(ar.has_value());
        CHECK(ar->value == "85");
    }

    // Reload in a fresh bridge instance (simulates client restart).
    {
        mq::RGMercsBridge reloaded;
        reloaded.attach_character(data_dir, char_dir, "Stabbinyah", "EZ Server", "ROG");
        const auto pull_range = reloaded.get_setting("PullRange");
        REQUIRE(pull_range.has_value());
        CHECK(pull_range->value == "215");

        const auto assist_range = reloaded.get_setting("AssistRange");
        REQUIRE(assist_range.has_value());
        CHECK(assist_range->value == "85");

        // Re-attaching with a character_dir outside data_dir keeps settings_path inside data_dir.
        mq::RGMercsBridge sandboxed;
        sandboxed.attach_character(data_dir, external_dir / "rogue_outside", "Rogue", "EZ Server", "ROG");
        CHECK(sandboxed.set_setting("DoMelee", "true", "Combat", false));
        REQUIRE(sandboxed.save_settings());
        CHECK_FALSE(fs::exists(external_dir / "rogue_outside" / "rgmercs_settings.ini"));
        CHECK(fs::path(sandboxed.snapshot().settings_path).string().find(data_dir.string()) != std::string::npos);
    }
}

TEST_CASE("RGMercsBridge + ScriptHost: hailstone.rgmercs adapter, error banner, and mq.imgui suppression") {
    std::unique_ptr<ImGuiContext, void (*)(ImGuiContext*)> imgui_ctx(ImGui::CreateContext(), ImGui::DestroyContext);
    mq_test::World world{"hailstone_rgmercs_native_ui_script"};
    const fs::path data_dir = world.dir / "data";
    const fs::path char_dir = data_dir / "characters" / "ez_server_stabbinyah";
    const fs::path lua_dir = data_dir / "scripts";
    const fs::path cfg_dir = data_dir / "scripts" / "config";
    fs::create_directories(char_dir);
    fs::create_directories(lua_dir);
    fs::create_directories(cfg_dir);

    {
        std::ofstream out(lua_dir / "rgmercs.lua");
        out << "local mq = require('mq')\n"
               "local hailstone = require('hailstone')\n"
               "DRAW_COUNT = 0\n"
               "mq.imgui.init('RGMercsLegacy', function()\n"
               "  DRAW_COUNT = DRAW_COUNT + 1\n"
               "end)\n"
               "hailstone.rgmercs.set_status('Adapter Running')\n"
               "hailstone.rgmercs.log('info', 'Native adapter connected')\n"
               "local pending = hailstone.rgmercs.take_pending_changes()\n"
               "for k, v in pairs(pending) do\n"
               "  hailstone.rgmercs.log('info', 'applied:' .. tostring(k) .. '=' .. tostring(v))\n"
               "end\n"
               "hailstone.rgmercs.set_setting('Combat', 'DoMelee', false)\n"
               "hailstone.rgmercs.set_module('Healing', false, 'Disabled', 'Solo mode')\n"
               "mq.delay(25)\n"
               "if tonumber(hailstone.rgmercs.get_setting('TriggerErr')) == 1 then\n"
               "  error('simulated native ui test error')\n"
               "end\n";
    }

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
    ctx.server_name = "EZ Server";
    ctx.lua_dir = lua_dir.string();

    mq::RGMercsBridge bridge;
    bridge.attach_character(data_dir, char_dir, "Stabbinyah", "EZ Server", "ROG");
    CHECK(bridge.set_setting("PullRange", "175", "Movement", true));

    std::int64_t clock_ms = 0;
    mq::ScriptEnv se;
    se.hooks = &hooks;
    se.policy = &policy;
    se.registry = &reg;
    se.context = [&]() -> const mq::Context& { return ctx; };
    se.print = [](const std::string&) {};
    se.run = [](const std::string&) {};
    se.dirs = {lua_dir};
    se.config_dir = cfg_dir;
    se.now_ms = [&] { return clock_ms; };
    se.rgmercs = &bridge;

    mq::ScriptHost host(se);
    std::string why;
    REQUIRE(host.run("rgmercs", {}, &why));
    host.pulse();
    // With suppress_script_ui == true, mq.imgui draw callbacks for rgmercs are suppressed.
    host.imgui();

    const auto mid = bridge.snapshot();
    CHECK(mid.script_status == "running");
    CHECK(mid.runtime_state == "Adapter Running");
    CHECK(mid.suppress_script_ui);

    bool saw_adapter_log = false;
    bool saw_pending_log = false;
    for (const auto& l : mid.logs) {
        if (l.message.find("Native adapter connected") != std::string::npos) saw_adapter_log = true;
        if (l.message.find("applied:PullRange=175") != std::string::npos) saw_pending_log = true;
    }
    CHECK(saw_adapter_log);
    CHECK(saw_pending_log);

    const auto do_melee = bridge.get_setting("DoMelee");
    REQUIRE(do_melee.has_value());
    CHECK(do_melee->value == "false");

    bool healing_disabled = false;
    for (const auto& m : mid.modules) {
        if (m.name == "Healing" && !m.enabled && m.status == "Disabled") healing_disabled = true;
    }
    CHECK(healing_disabled);

    // Finish cleanly first.
    clock_ms = 50;
    host.pulse();
    CHECK(bridge.snapshot().last_error.empty());

    // Now run again with TriggerErr = 1 and confirm the error banner state captures and preserves the failure.
    CHECK(bridge.set_setting("TriggerErr", "1", "Class", false));
    REQUIRE(host.run("rgmercs", {}, &why));
    clock_ms = 60;
    host.pulse();
    clock_ms = 100;
    host.pulse();

    const auto after_err = bridge.snapshot();
    CHECK(after_err.script_status == "error");
    CHECK(after_err.error_count >= 1);
    CHECK(after_err.last_error.find("simulated native ui test error") != std::string::npos);
}

TEST_CASE("hs.RGMercs* & hs.Commander* Lua adapters, capability gating, CascadeMenu integration, and modern Lua UI validation") {
    std::unique_ptr<ImGuiContext, void (*)(ImGuiContext*)> imgui_ctx(ImGui::CreateContext(), ImGui::DestroyContext);
    test::ScratchDir scratch("rgmercs_hs");
    const fs::path data_dir = scratch.path() / "data";
    const fs::path char_dir = data_dir / "characters" / "ez_server_stabbinyah";
    fs::create_directories(char_dir);

    mq::RGMercsBridge bridge;
    bridge.attach_character(data_dir, char_dir, "Stabbinyah", "EZ Server", "ROG");

    std::vector<std::string> dispatched;
    std::vector<std::string> commander_calls;

    app::UiHost host;
    host.in_game = true;
    host.ui_ready = true;
    host.rgmercs = &bridge;
    host.rgmercs_server = "EZ Server";
    host.run_rgmercs_command = [&](const std::string& cmd) {
        dispatched.push_back(cmd);
        return true;
    };
    host.commander.status = []() {
        app::UiHost::CommanderStatusSnapshot snap;
        snap.bus_connected = true;
        snap.session_id = "slot01";
        snap.character_name = "Stabbinyah";
        snap.execution_state = "ForegroundFull";
        snap.auto_headless_others = true;
        snap.active_profile_id = "Melee Group";
        ipc::SessionSnapshot p1;
        p1.session_id = "slot01";
        p1.character_name = "Stabbinyah";
        p1.class_short = "ROG";
        p1.level = 75;
        p1.zone_short = "surefall";
        p1.combat_state = "Idle";
        p1.script_name = "rgmercs";
        p1.script_state = "running";
        p1.connection_state = "in_game";
        p1.exec_state = app::ExecutionState::ForegroundFull;
        p1.online = true;
        snap.peers.push_back(p1);
        return snap;
    };
    host.commander.command = [&](std::string_view action, std::string_view target, std::string_view arg,
                                 std::string& out_msg) {
        commander_calls.push_back(std::string(action) + ":" + std::string(target) + ":" + std::string(arg));
        out_msg = "dispatched:" + std::string(action);
        return true;
    };

    app::UiApi api(host);
    std::unique_ptr<lua_State, decltype(&lua_close)> lua_guard(luaL_newstate(), &lua_close);
    lua_State* L = lua_guard.get();
    REQUIRE(L != nullptr);
    luaL_openlibs(L);
    lua_newtable(L);
    api.register_functions(L);
    lua_setglobal(L, "hs");

    CHECK(run_lua(L, "local st = hs.RGMercsState(true) return st.character .. '@' .. st.server") == "Stabbinyah@EZ Server");
    CHECK(run_lua(L, "return hs.RGMercsSetSetting('AssistRange', '90', 'Targeting')") == "true");
    CHECK(run_lua(L, "return hs.RGMercsSaveSettings()") == "true");
    CHECK(fs::exists(char_dir / "rgmercs_settings.ini"));
    CHECK(run_lua(L, "return hs.RGMercsSetModule('Combat', false)") == "true");
    CHECK(run_lua(L, "return hs.RGMercsCommand('pause')") == "true");
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched[0] == "/rgl pause");

    CHECK(run_lua(L, "hs.RGMercsControl('suppress_ui_off') return hs.RGMercsState().suppress_script_ui") == "false");
    CHECK(run_lua(L, "hs.RGMercsControl('suppress_ui_on') return hs.RGMercsState().suppress_script_ui") == "true");
    CHECK(run_lua(L, "hs.RGMercsClearLogs() return #hs.RGMercsState().logs") == "0");

    // Verify all 9 sections exist in defaults exposed to UI.
    CHECK(run_lua(L, "local st = hs.RGMercsState() local seen = {} "
                     "for _, s in ipairs(st.settings) do seen[s.category] = true end "
                     "return tostring(seen.Combat and seen.Targeting and seen.Healing and seen.Buffs and "
                     "seen.Debuffs and seen.Movement and seen.Loot and seen.Class)") == "true");

    // Verify CommanderStatus and CommanderDo bindings.
    CHECK(run_lua(L, "local c = hs.CommanderStatus() return c.characterName .. ':' .. tostring(#c.peers) .. ':' .. c.peers[1].scriptState")
          == "Stabbinyah:1:running");
    CHECK(run_lua(L, "local ok, msg = hs.CommanderDo('follow', 'Yavaha', 'Stabbinyah') return tostring(ok) .. ':' .. tostring(msg)")
          == "true:dispatched:follow");
    REQUIRE(commander_calls.size() == 1);
    CHECK(commander_calls[0] == "follow:Yavaha:Stabbinyah");

    // Verify ui_call_caps classifies every RGMercs and Commander function in lua_ui_api.inc.
    CHECK(app::ui_call_caps("RGMercsState") == std::optional<std::uint32_t>{0u});
    CHECK(app::ui_call_caps("RGMercsControl") == std::optional<std::uint32_t>{0u});
    CHECK(app::ui_call_caps("RGMercsSetSetting") == std::optional<std::uint32_t>{0u});
    CHECK(app::ui_call_caps("RGMercsSaveSettings") == std::optional<std::uint32_t>{0u});
    CHECK(app::ui_call_caps("RGMercsResetDefaults") == std::optional<std::uint32_t>{0u});
    CHECK(app::ui_call_caps("RGMercsImportDb") == std::optional<std::uint32_t>{0u});
    CHECK(app::ui_call_caps("RGMercsSetModule") == std::optional<std::uint32_t>{0u});
    CHECK(app::ui_call_caps("RGMercsClearLogs") == std::optional<std::uint32_t>{0u});
    CHECK(app::ui_call_caps("RGMercsCommand") == std::optional<std::uint32_t>{plugin::kCapCommands});
    CHECK(app::ui_call_caps("CommanderStatus") == std::optional<std::uint32_t>{0u});
    CHECK(app::ui_call_caps("CommanderDo") ==
          std::optional<std::uint32_t>{plugin::kCapActions | plugin::kCapCommands | plugin::kCapMove});
    CHECK(app::ui_call_caps("ScriptReload") == std::optional<std::uint32_t>{0u});

    // CascadeMenu: "RGMercs" is offered only when the pack draws RGMercsWnd.
    auto with_rgmercs = [](std::string_view screen) { return screen == "RGMercsWnd" || screen == "OptionsWindow"; };
    app::CascadeMenu menu;
    menu.load_text(app::CascadeMenu::hailstone_layout(), 0xffffffffu, 1,
                   [&](int c) { return app::menu_command_offered(c, with_rgmercs); });
    const auto top = menu.row_texts(0);
    CHECK(std::find(top.begin(), top.end(), "RGMercs") != top.end());

    // Validate ui/modern/rgmercs.lua and ui/modern/combat.lua compile cleanly and use modern hs.UI APIs.
    for (const char* rel : {"ui/modern/rgmercs.lua", "ui/modern/combat.lua", "ui/modern/scripts.lua"}) {
        const fs::path ui_script = fs::path(HAILSTONE_SOURCE_DIR) / rel;
        REQUIRE(fs::exists(ui_script));
        CHECK(luaL_loadfile(L, ui_script.string().c_str()) == LUA_OK);
        lua_settop(L, 0);
    }

    const std::string rg_src = read_file_bytes(fs::path(HAILSTONE_SOURCE_DIR) / "ui/modern/rgmercs.lua");
    CHECK(rg_src.find("hs.UI.rgmercs") != std::string::npos);
    CHECK(rg_src.find("Zep") == std::string::npos);
    CHECK(rg_src.find("IM_COL32") == std::string::npos);
    CHECK(rg_src.find("ImHashStr") == std::string::npos);
    CHECK(rg_src.find("GetForegroundDrawList") == std::string::npos);
    CHECK(rg_src.find("ConsoleWidget") == std::string::npos);

    const std::string combat_src = read_file_bytes(fs::path(HAILSTONE_SOURCE_DIR) / "ui/modern/combat.lua");
    CHECK(combat_src.find("Players / Clients") != std::string::npos);
    CHECK(combat_src.find("API.commander") != std::string::npos);
}
