// Trusted script-root loader vs restricted data-only config loader tests for RG Mercs:
// 1. RG Mercs class configuration initialization for Stabbinyah (ROG on EZ Server) via trusted loadfile/dofile
//    and MacroQuest-compatible require/package with mq, ImGui, lfs, lsqlite3, actors, bit32, and hailstone.rgmercs.
// 2. Preserves restricted data-only loader for mq.configDir / mq.pickle, blocks config/ paths inside script roots,
//    enforces server policy/capability ceilings, and rejects binary chunks & directory traversal.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/sha256.h"
#include "mq/commands.h"
#include "mq/lua_mq.h"
#include "mq/rgmercs_bridge.h"
#include "mq/tlo.h"
#include "mq_test_world.h"
#include "plugin/hooks.h"
#include "plugin/policy.h"

namespace fs = std::filesystem;
using namespace hailstone;

namespace {

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << content;
}

struct LoaderHarness {
    mq_test::World world{"hailstone_trusted_loader"};
    fs::path lua_dir = world.dir / "scripts";
    fs::path cfg_dir = lua_dir / "config";
    fs::path char_dir = world.dir / "characters" / "ez_server_stabbinyah";
    plugin::Hooks hooks;
    plugin::Policy policy;
    mq::Registry reg;
    mq::Context ctx;
    mq::RGMercsBridge bridge;
    std::vector<std::string> printed;
    std::int64_t now_ms = 0;
    std::unique_ptr<mq::Commands> commands;
    std::unique_ptr<mq::ScriptHost> host;

    explicit LoaderHarness(bool allow_player_scripts = true) {
        fs::create_directories(lua_dir);
        fs::create_directories(cfg_dir);
        fs::create_directories(char_dir);

        using A = net::GameConnectionTestAccess;
        A::character(*world.conn) = "Stabbinyah";
        A::profile(*world.conn).class_ = 9; // Rogue (ROG)
        auto& w = A::world(*world.conn);
        w.spawns[1].name = "Stabbinyah";
        w.spawns[1].class_id = 9;
        w.zone_short_name = "surefall";
        w.zone_long_name = "The Surefall Glade";

        if (allow_player_scripts) {
            plugin::Rules rules;
            rules.configured = true;
            rules.player_script_caps = plugin::kCapAll;
            rules.social_mode = plugin::ButtonMode::Off;
            policy.set(rules);
        }

        mq::register_game_tlos(reg);
        ctx.registry = &reg;
        ctx.conn = world.conn.get();
        ctx.server_name = "EZ Server";
        ctx.lua_dir = lua_dir.string();
        ctx.foreground = true;

        bridge.attach_character(world.dir, char_dir, "Stabbinyah", "EZ Server", "ROG");

        mq::CommandEnv ce;
        ce.hooks = &hooks;
        ce.registry = &reg;
        ce.context = [this]() -> const mq::Context& { return ctx; };
        ce.echo = [this](const std::string& line) { printed.push_back(line); };
        ce.run = [this](const std::string& line) {
            std::string cmd = line;
            (void)hooks.command(cmd);
        };
        commands = std::make_unique<mq::Commands>(ce);

        mq::ScriptEnv se;
        se.hooks = &hooks;
        se.policy = &policy;
        se.registry = &reg;
        se.context = ce.context;
        se.print = ce.echo;
        se.run = ce.run;
        se.dirs = {lua_dir};
        se.config_dir = cfg_dir;
        se.now_ms = [this] { return now_ms; };
        se.rgmercs = &bridge;
        host = std::make_unique<mq::ScriptHost>(se);
    }

    ~LoaderHarness() {
        host.reset();
        commands.reset();
    }

    bool has_line(std::string_view needle) const {
        return std::any_of(printed.begin(), printed.end(),
                           [&](const std::string& line) { return line.find(needle) != std::string::npos; });
    }
};

} // namespace

TEST_CASE("TrustedScriptLoader: RG Mercs loads Stabbinyah ROG class configs with active environment and compat modules") {
    LoaderHarness h;

    // Shared helper required FROM inside dynamically loaded class config chunks.
    write_file(h.lua_dir / "rgmercs" / "utils" / "rotation_helper.lua", R"lua(
local mq = require('mq')
local M = {}
function M.describe()
    return string.format("%s/%s/%s", mq.TLO.Me.Name(), mq.TLO.Me.Class.ShortName(), mq.TLO.MacroQuest.Server())
end
return M
)lua");

    // RG Mercs class config in Live/rog_class_config.lua loaded via absolute loadfile(mq.luaDir .. '/...')
    write_file(h.lua_dir / "rgmercs" / "class_configs" / "Live" / "rog_class_config.lua", R"lua(
local mq = require('mq')
local ImGui = require('ImGui')
local Icons = require('mq.Icons')
local lsqlite3 = require('lsqlite3')
local lfs = require('lfs')
local actors = require('actors')
local helper = require('utils.rotation_helper')
local hailstone = require('hailstone')

assert(mq and mq.TLO and ImGui and Icons and lsqlite3 and lfs and actors and bit32)
assert(type(IM_COL32(255, 128, 0, 255)) == 'number')
assert(type(ImHashStr('rog')) == 'number')

local dt = mq.DataType.new('RogTestType', {
    Members = {
        Summary = function(_, obj) return 'string', obj.summary end,
    },
})
mq.AddTopLevelObject('RogInitCheck', function()
    return dt, { summary = helper.describe() }
end)

return {
    _version = 'Live-ROG',
    DefaultConfig = { DoBackstab = true, HidePct = 95 },
    RunRotation = function(self)
        local msg = mq.TLO.RogInitCheck.Summary()
        hailstone.rgmercs.set_setting('Class', 'DoBackstab', self.DefaultConfig.DoBackstab)
        hailstone.rgmercs.set_module('Class', true, 'Running', 'ROG class loaded')
        hailstone.rgmercs.set_status('Class initialized: ' .. tostring(msg))
        return msg
    end,
}
)lua");

    // Class config under folder with space loaded via relative dofile(...)
    write_file(h.lua_dir / "rgmercs" / "class_configs" / "EQ Might" / "rog_class_config.lua", R"lua(
local mq = require('mq')
return {
    _version = 'EQMight-' .. mq.TLO.Me.Class.ShortName(),
    Init = function() return 'eqmight-ok' end,
}
)lua");

    // Classloader module mimicking RG Mercs' utils/classloader.lua using lfs + loadfile/dofile + package.searchpath
    write_file(h.lua_dir / "rgmercs" / "utils" / "classloader.lua", R"lua(
local mq = require('mq')
local lfs = require('lfs')
local classloader = {}

function classloader.discover()
    local found = {}
    local base = mq.luaDir .. '/rgmercs/class_configs'
    assert(lfs.attributes(base, 'mode') == 'directory')
    for entry in lfs.dir(base) do
        if entry ~= '.' and entry ~= '..' then
            found[entry] = true
        end
    end
    return found
end

function classloader.load(class_short)
    local path = mq.luaDir .. '/rgmercs/class_configs/Live/' .. class_short:lower() .. '_class_config.lua'
    local fn, err = loadfile(path)
    assert(fn, 'trusted loadfile failed: ' .. tostring(err))
    local cfg = fn()
    assert(type(cfg) == 'table' and type(cfg.RunRotation) == 'function')

    local eqm = dofile('class_configs/EQ Might/' .. class_short:lower() .. '_class_config.lua')
    assert(type(eqm) == 'table' and eqm.Init() == 'eqmight-ok')

    local slash_req = require('class_configs/Live/rog_class_config.lua')
    assert(slash_req._version == 'Live-ROG')
    return cfg
end

return classloader
)lua");

    write_file(h.lua_dir / "rgmercs" / "init.lua", R"lua(
local mq = require('mq')
local loader = require('utils.classloader')
local dirs = loader.discover()
assert(dirs['Live'] and dirs['EQ Might'])

-- Verify package.searchpath & package.preload compatibility.
local sp, sp_err = package.searchpath('utils.rotation_helper', package.path)
assert(sp ~= nil, tostring(sp_err))
package.preload['rog.preloaded_mod'] = function() return { ready = true } end
assert(require('rog.preloaded_mod').ready == true)

-- Explicit custom env override stays supported on loadfile(path, 't', custom_env).
local isolated_env = { TAG = 'custom_env_ok' }
local env_fn, env_err = loadfile('utils/rotation_helper.lua', 't', isolated_env)
-- Helper calls require(), which is intentionally absent in isolated_env -> proves custom env was applied.
assert(type(env_fn) == 'function', tostring(env_err))
local ok_iso = pcall(env_fn)
assert(not ok_iso)

local cfg = loader.load(mq.TLO.Me.Class.ShortName())
local res = cfg:RunRotation()
print('CLASS_INIT_OK:' .. tostring(res))
)lua");

    std::string why;
    REQUIRE(h.host->run("rgmercs", {}, &why));
    for (int i = 0; i < 32 && h.host->running("rgmercs"); ++i) {
        h.now_ms += 10;
        h.host->pulse();
    }

    const auto list = h.host->list();
    REQUIRE_FALSE(list.empty());
    CHECK(list[0].status == "EXITED");
    CHECK(list[0].error.empty());
    CHECK(h.has_line("CLASS_INIT_OK:Stabbinyah/ROG/EZ Server"));

    const auto snap = h.bridge.snapshot();
    CHECK(snap.last_error.empty());
    CHECK(snap.runtime_state == "Class initialized: Stabbinyah/ROG/EZ Server");
}

TEST_CASE("TrustedScriptLoader: preserves data-only mq.configDir loader and enforces policy & sandbox rules") {
    LoaderHarness h;

    // Put a closure-bearing Lua file inside mq.configDir -> MUST remain restricted data-only!
    write_file(h.cfg_dir / "user_data.lua", "return { val = 42 }\n");
    write_file(h.cfg_dir / "user_code.lua", "return { fn = function() return 99 end }\n");

    // Put Lua files inside script's own config/ subdir -> MUST never be executable via loadfile or require!
    write_file(h.lua_dir / "rgmercs" / "config" / "sneaky.lua", "return { fn = function() return 1 end }\n");
    write_file(h.lua_dir / "rgmercs" / "class_configs" / "ok.lua", "return { tag = 'ok' }\n");

    write_file(h.lua_dir / "rgmercs" / "init.lua", R"lua(
local mq = require('mq')

-- mq.pickle roundtrip and existing data-only config dir read
mq.pickle('prefs.lua', { melee = true })
assert(dofile('prefs.lua').melee == true)
assert(dofile('user_data.lua').val == 42)

-- Code in mq.configDir must STILL fail as non-data!
local f_cfg, err_cfg = loadfile('user_code.lua')
assert(f_cfg == nil and tostring(err_cfg):find('not data', 1, true) ~= nil, tostring(err_cfg))

local f_abs_cfg, err_abs_cfg = loadfile(mq.configDir .. '/user_code.lua')
assert(f_abs_cfg == nil and tostring(err_abs_cfg):find('not data', 1, true) ~= nil, tostring(err_abs_cfg))

-- Files under <script>/config/ are never executable via loadfile, dofile, or require
local f_subcfg = loadfile('config/sneaky.lua')
assert(f_subcfg == nil)
local ok_req_subcfg = pcall(require, 'config.sneaky')
assert(not ok_req_subcfg)

-- Binary mode and traversal outside script root must fail cleanly
local f_bin, err_bin = loadfile('class_configs/ok.lua', 'b')
assert(f_bin == nil and tostring(err_bin):find('binary', 1, true) ~= nil)
local f_trav = loadfile('../../outside.lua')
assert(f_trav == nil)
local f_abs = loadfile('/etc/passwd')
assert(f_abs == nil)

print('SANDBOX_PRESERVED_OK')
)lua");

    std::string why;
    REQUIRE(h.host->run("rgmercs", {}, &why));
    for (int i = 0; i < 32 && h.host->running("rgmercs"); ++i) {
        h.now_ms += 10;
        h.host->pulse();
    }

    const auto list = h.host->list();
    REQUIRE_FALSE(list.empty());
    CHECK(list[0].status == "EXITED");
    CHECK(list[0].error.empty());
    CHECK(h.has_line("SANDBOX_PRESERVED_OK"));

    // Policy & capability ceiling enforcement when player scripts are disallowed / server add-ons enforced.
    LoaderHarness strict(false);
    const std::string init_src = R"lua(
local f_unapproved, err1 = loadfile('class_configs/unapproved.lua')
local f_lowcap, err2 = loadfile('class_configs/lowcap.lua')
print('UNAPPROVED', f_unapproved == nil, tostring(err1):find('may not run', 1, true) ~= nil)
print('LOWCAP', f_lowcap == nil, tostring(err2):find('may do only', 1, true) ~= nil)
)lua";
    const std::string lowcap_src = "return { low = true }\n";
    const std::string unapproved_src = "return { nope = true }\n";

    write_file(strict.lua_dir / "rgmercs" / "init.lua", init_src);
    write_file(strict.lua_dir / "rgmercs" / "class_configs" / "lowcap.lua", lowcap_src);
    write_file(strict.lua_dir / "rgmercs" / "class_configs" / "unapproved.lua", unapproved_src);

    strict.policy.set_ceilings({
        {plugin::FileKind::Script, sha256(init_src), plugin::kCapRead | plugin::kCapCommands, "rgmercs", "srv/rg"},
        {plugin::FileKind::Script, sha256(lowcap_src), plugin::kCapRead, "lowcap", "srv/low"},
    });
    strict.policy.set(plugin::parse_server_cfg(
        "[scripts]\nplayer = off\n[addons]\nenforce = on\ndefault = deny\nrequired = srv/rg, srv/low\n"));

    std::string strict_why;
    REQUIRE(strict.host->run("rgmercs", {}, &strict_why));
    for (int i = 0; i < 16 && strict.host->running("rgmercs"); ++i) {
        strict.now_ms += 10;
        strict.host->pulse();
    }

    CHECK(strict.has_line("UNAPPROVED true true"));
    CHECK(strict.has_line("LOWCAP true true"));
}
