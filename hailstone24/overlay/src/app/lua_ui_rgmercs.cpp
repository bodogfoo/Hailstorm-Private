// Hailstone-native RG Mercs UI adapter for the `hs` table (app/lua_ui_plugins.h, mq/rgmercs_bridge.h).
// Keeps RG Mercs gameplay/combat logic decoupled from presentation: the UI reads status, modules, settings,
// observable lifecycle transitions, and logs, and triggers Start/Stop/Pause/Resume/Reload, settings edits,
// persistence, and read-only DB import through this boundary.

#include "app/lua_ui_plugins.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include <lua.hpp>

#include "app/lua_ui.h"
#include "game/eq_names.h"
#include "game/slash_commands.h"
#include "game/world_state.h"
#include "mq/lua_mq.h"
#include "mq/rgmercs_bridge.h"
#include "net/game_connection.h"
#include "plugin/lua_keep.h"
#include "plugin/policy.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {

namespace {

constexpr auto kRescan = std::chrono::seconds(1);

struct RGMercsCache {
    std::vector<mq::ScriptHost::Found> found;
    std::chrono::steady_clock::time_point at{};
    bool valid = false;

    bool found_flag = false;
    bool allowed = false;
    bool server = false;
    bool kept = false;
    bool autorun = false;
    bool autorun_available = false;
    std::string status;
    std::string script_error;
    std::string why;
    std::string caps;
    std::string character;
    std::string server_name;
    std::string class_name;
    mq::RGMercsBridge::Snapshot snap;
};

UiHost& host_of(lua_State* L) { return *static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1))); }
RGMercsCache& cache_of(lua_State* L) { return *static_cast<RGMercsCache*>(lua_touserdata(L, lua_upvalueindex(2))); }
bool plugin_code() { return !ui::lua::LuaUi::caller_tag().empty(); }

int cache_gc(lua_State* L) {
    static_cast<RGMercsCache*>(lua_touserdata(L, 1))->~RGMercsCache();
    return 0;
}

std::string status_word(std::string_view s) {
    if (s == "RUNNING") return "running";
    if (s == "PAUSED") return "paused";
    if (s == "EXITED") return "stopped";
    if (s == "ERROR") return "error";
    if (s == "STOPPED" || s == "stopped") return "stopped";
    if (s == "STARTING" || s == "starting") return "starting";
    if (s == "STOPPING" || s == "stopping") return "stopping";
    if (s == "RELOADING" || s == "reloading") return "reloading";
    return {};
}

void set_string(lua_State* L, const char* key, std::string_view v) {
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, -2, key);
}

void set_bool(lua_State* L, const char* key, bool v) {
    lua_pushboolean(L, v);
    lua_setfield(L, -2, key);
}

void set_int(lua_State* L, const char* key, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, key);
}

void set_number(lua_State* L, const char* key, double v) {
    lua_pushnumber(L, v);
    lua_setfield(L, -2, key);
}

std::vector<std::string> split_args(const char* a) {
    std::vector<std::string> args;
    if (!a) return args;
    std::string_view rest = a;
    while (true) {
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
        if (rest.empty()) break;
        args.push_back(game::next_cmd_arg(rest));
    }
    return args;
}

// st = hs.RGMercsState([rescan])
int RGMercsState(lua_State* L) {
    UiHost& h = host_of(L);
    RGMercsCache& c = cache_of(L);
    const bool rescan = lua_toboolean(L, 1) != 0;
    const bool restricted = plugin_code();

    plugin::cxx_guard(L, [&] {
        c.found_flag = false;
        c.allowed = false;
        c.server = false;
        c.kept = false;
        c.autorun = false;
        c.autorun_available = false;
        c.status.clear();
        c.script_error.clear();
        c.why.clear();
        c.caps.clear();
        c.character.clear();
        c.server_name.clear();
        c.class_name.clear();
        c.snap = {};

        if (h.scripts) {
            const auto now = std::chrono::steady_clock::now();
            if (!c.valid || rescan || now - c.at >= kRescan) {
                c.found = h.scripts->found();
                c.at = now;
                c.valid = true;
            }
            for (const auto& f : c.found) {
                if (f.name == "rgmercs") {
                    c.found_flag = true;
                    c.allowed = f.caps != 0;
                    c.why = f.why;
                    c.server = f.server;
                    if (f.caps) c.caps = plugin::caps_text(f.caps);
                    break;
                }
            }
            bool in_live_list = false;
            for (const auto& i : h.scripts->list()) {
                if (i.name == "rgmercs") {
                    in_live_list = true;
                    c.status = status_word(i.status);
                    c.script_error = i.error;
                    c.kept = i.kept;
                    if (i.caps) c.caps = plugin::caps_text(i.caps);
                    break;
                }
            }
            if (!in_live_list) {
                if (auto lo = h.scripts->last_outcome("rgmercs")) {
                    c.status = status_word(lo->status);
                    if (!lo->error.empty()) c.script_error = lo->error;
                }
            }
            c.autorun_available = h.scripts->autorun_available();
            c.autorun = h.scripts->autorun("rgmercs");
        }

        if (h.rgmercs && !restricted) {
            c.snap = h.rgmercs->snapshot();
            // Never overwrite an error state from bridge with a generic stopped status.
            if (c.snap.script_status == "error" && c.status != "running" && c.status != "paused") {
                c.status = "error";
            } else if (c.status.empty() && !c.snap.script_status.empty()) {
                c.status = c.snap.script_status;
            }
            if (c.script_error.empty() && !c.snap.last_error.empty()) {
                c.script_error = c.snap.last_error;
            }
            c.character = c.snap.character;
            c.server_name = c.snap.server;
            c.class_name = c.snap.class_name;
        }
        if (c.status.empty()) c.status = "stopped";

        if (c.character.empty() && h.conn) {
            if (const game::Spawn* me = h.conn->world().player()) {
                c.character = game::display_name(me->name);
                if (c.class_name.empty()) {
                    if (me->class_id != 0) c.class_name = game::class_short_name(me->class_id);
                    else if (h.conn->player_class_mask() != 0) c.class_name = game::class_mask_name(h.conn->player_class_mask());
                }
            }
        }
        if (c.server_name.empty() && !h.rgmercs_server.empty()) c.server_name = h.rgmercs_server;
    });

    lua_createtable(L, 0, 22);
    set_string(L, "status", c.status);
    set_string(L, "runtime_state", c.snap.runtime_state);
    set_string(L, "last_error", restricted ? std::string_view() : std::string_view(c.script_error));
    set_int(L, "error_count", restricted ? 0 : c.snap.error_count);
    set_bool(L, "found", c.found_flag);
    set_bool(L, "allowed", c.allowed);
    set_string(L, "why", restricted ? std::string_view() : std::string_view(c.why));
    set_bool(L, "server_script", c.server);
    set_bool(L, "kept", c.kept);
    set_bool(L, "autorun", c.autorun);
    set_bool(L, "autorun_available", c.autorun_available);
    set_string(L, "caps", c.caps);

    set_string(L, "character", c.character);
    set_string(L, "server", c.server_name);
    set_string(L, "class_name", c.class_name);
    set_string(L, "settings_path", restricted ? std::string_view() : std::string_view(c.snap.settings_path));
    set_string(L, "imported_from", restricted ? std::string_view() : std::string_view(c.snap.imported_from));
    set_int(L, "imported_count", restricted ? 0 : c.snap.imported_count);
    set_bool(L, "suppress_script_ui", c.snap.suppress_script_ui);
    set_bool(L, "dirty", c.snap.dirty);

    // available_dbs
    lua_newtable(L);
    if (!restricted) {
        lua_Integer idx = 0;
        for (const auto& db : c.snap.available_dbs) {
            lua_pushlstring(L, db.data(), db.size());
            lua_rawseti(L, -2, ++idx);
        }
    }
    lua_setfield(L, -2, "available_dbs");

    // modules
    lua_newtable(L);
    if (!restricted) {
        lua_Integer idx = 0;
        for (const auto& m : c.snap.modules) {
            lua_createtable(L, 0, 4);
            set_string(L, "name", m.name);
            set_bool(L, "enabled", m.enabled);
            set_string(L, "status", m.status);
            set_string(L, "detail", m.detail);
            lua_rawseti(L, -2, ++idx);
        }
    }
    lua_setfield(L, -2, "modules");

    // settings
    lua_newtable(L);
    if (!restricted) {
        lua_Integer idx = 0;
        for (const auto& s : c.snap.settings) {
            lua_createtable(L, 0, 10);
            set_string(L, "key", s.key);
            set_string(L, "category", s.category);
            set_string(L, "type", s.type);
            set_string(L, "value", s.value);
            set_string(L, "default_value", s.default_value);
            set_string(L, "description", s.description);
            set_number(L, "min_val", s.min_val);
            set_number(L, "max_val", s.max_val);
            set_bool(L, "imported", s.imported);
            lua_newtable(L);
            lua_Integer ci = 0;
            for (const auto& ch : s.choices) {
                lua_pushlstring(L, ch.data(), ch.size());
                lua_rawseti(L, -2, ++ci);
            }
            lua_setfield(L, -2, "choices");
            lua_rawseti(L, -2, ++idx);
        }
    }
    lua_setfield(L, -2, "settings");

    // logs
    lua_newtable(L);
    if (!restricted) {
        lua_Integer idx = 0;
        for (const auto& l : c.snap.logs) {
            lua_createtable(L, 0, 3);
            set_string(L, "timestamp", l.timestamp);
            set_string(L, "level", l.level);
            set_string(L, "message", l.message);
            lua_rawseti(L, -2, ++idx);
        }
    }
    lua_setfield(L, -2, "logs");

    // lifecycle transitions
    lua_newtable(L);
    if (!restricted) {
        lua_Integer idx = 0;
        for (const auto& t : c.snap.transitions) {
            lua_createtable(L, 0, 4);
            set_string(L, "timestamp", t.timestamp);
            set_string(L, "from", t.from_state);
            set_string(L, "to", t.to_state);
            set_string(L, "reason", t.reason);
            lua_rawseti(L, -2, ++idx);
        }
    }
    lua_setfield(L, -2, "transitions");

    return 1;
}

// ok, msg = hs.RGMercsControl(action [, arg])
int RGMercsControl(lua_State* L) {
    UiHost& h = host_of(L);
    const char* action_c = luaL_checkstring(L, 1);
    const char* arg_c = luaL_optstring(L, 2, nullptr);
    if (plugin_code()) {
        lua_pushboolean(L, 0);
        lua_pushliteral(L, "RGMercs controls are reserved for the player");
        return 2;
    }

    std::string& why = plugin::lua_keep<std::string>(L);
    why.clear();
    bool ok = false;
    plugin::cxx_guard(L, [&] {
        const std::string_view action(action_c);
        if (action == "start") {
            if (!h.scripts) {
                why = "scripts are off";
                return;
            }
            if (h.rgmercs) h.rgmercs->clear_error();
            ok = h.scripts->run("rgmercs", split_args(arg_c), &why);
        } else if (action == "stop") {
            ok = h.scripts && h.scripts->stop("rgmercs");
            if (!ok) why = "rgmercs is not running";
        } else if (action == "pause" || action == "resume") {
            if (!h.scripts) {
                why = "scripts are off";
                return;
            }
            bool is_paused = false;
            bool is_running = false;
            for (const auto& s : h.scripts->list()) {
                if (s.name == "rgmercs") {
                    is_paused = (s.status == "PAUSED");
                    is_running = (s.status == "RUNNING");
                    break;
                }
            }
            if (action == "resume" && !is_paused) {
                ok = is_running;
                if (!ok) why = "rgmercs is not paused";
                return;
            }
            ok = h.scripts->pause("rgmercs");
            if (!ok) why = "rgmercs is not running";
        } else if (action == "reload") {
            if (!h.scripts) {
                why = "scripts are off";
                return;
            }
            ok = h.scripts->reload("rgmercs", split_args(arg_c), &why);
        } else if (action == "autorun_on" || action == "autorun_off") {
            ok = h.scripts && h.scripts->set_autorun("rgmercs", action == "autorun_on");
            if (!ok) why = "no character attached";
        } else if (action == "suppress_ui_on" || action == "suppress_ui_off") {
            if (h.rgmercs) {
                h.rgmercs->set_suppress_script_ui(action == "suppress_ui_on");
                ok = true;
            }
        } else if (action == "clear_error") {
            if (h.rgmercs) {
                h.rgmercs->clear_error();
                ok = true;
            }
        } else {
            why = "unknown action";
        }
    });

    lua_pushboolean(L, ok);
    if (ok) return 1;
    lua_pushlstring(L, why.data(), why.size());
    return 2;
}

// ok = hs.RGMercsSetSetting(key, val [, category])
int RGMercsSetSetting(lua_State* L) {
    UiHost& h = host_of(L);
    const char* key = luaL_checkstring(L, 1);
    const char* val = luaL_checkstring(L, 2);
    const char* cat = luaL_optstring(L, 3, "");
    if (plugin_code() || !h.rgmercs) {
        lua_pushboolean(L, 0);
        return 1;
    }
    bool ok = false;
    plugin::cxx_guard(L, [&] { ok = h.rgmercs->set_setting(key, val, cat, true); });
    lua_pushboolean(L, ok);
    return 1;
}

// ok = hs.RGMercsSaveSettings()
int RGMercsSaveSettings(lua_State* L) {
    UiHost& h = host_of(L);
    if (plugin_code() || !h.rgmercs) {
        lua_pushboolean(L, 0);
        return 1;
    }
    bool ok = false;
    plugin::cxx_guard(L, [&] { ok = h.rgmercs->save_settings(); });
    lua_pushboolean(L, ok);
    return 1;
}

// ok = hs.RGMercsResetDefaults()
int RGMercsResetDefaults(lua_State* L) {
    UiHost& h = host_of(L);
    if (plugin_code() || !h.rgmercs) {
        lua_pushboolean(L, 0);
        return 1;
    }
    plugin::cxx_guard(L, [&] { h.rgmercs->reset_to_defaults(); });
    lua_pushboolean(L, 1);
    return 1;
}

// ok, count, detail, warnings = hs.RGMercsImportDb([path])
int RGMercsImportDb(lua_State* L) {
    UiHost& h = host_of(L);
    const char* path_c = luaL_optstring(L, 1, "");
    if (plugin_code() || !h.rgmercs) {
        lua_pushboolean(L, 0);
        lua_pushinteger(L, 0);
        lua_pushliteral(L, "unavailable");
        lua_newtable(L);
        return 4;
    }
    auto& res = plugin::lua_keep<mq::RGMercsBridge::ImportResult>(L);
    plugin::cxx_guard(L, [&] {
        const std::string_view p(path_c);
        res = p.empty() ? h.rgmercs->import_best_source()
                        : h.rgmercs->import_from_path(std::filesystem::path(p));
    });
    lua_pushboolean(L, res.ok);
    lua_pushinteger(L, res.imported_count);
    const std::string& detail = res.ok ? res.source : res.error;
    lua_pushlstring(L, detail.data(), detail.size());
    lua_createtable(L, static_cast<int>(res.warnings.size()), 0);
    for (std::size_t i = 0; i < res.warnings.size(); ++i) {
        lua_pushlstring(L, res.warnings[i].data(), res.warnings[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 4;
}

// ok = hs.RGMercsSetModule(name, enabled)
int RGMercsSetModule(lua_State* L) {
    UiHost& h = host_of(L);
    const char* name = luaL_checkstring(L, 1);
    const bool enabled = lua_toboolean(L, 2) != 0;
    if (plugin_code() || !h.rgmercs) {
        lua_pushboolean(L, 0);
        return 1;
    }
    plugin::cxx_guard(L, [&] {
        h.rgmercs->set_module(name, enabled, enabled ? "Idle" : "Disabled", {}, true);
    });
    lua_pushboolean(L, 1);
    return 1;
}

// hs.RGMercsClearLogs()
int RGMercsClearLogs(lua_State* L) {
    UiHost& h = host_of(L);
    if (!plugin_code() && h.rgmercs) {
        plugin::cxx_guard(L, [&] { h.rgmercs->clear_logs(); });
    }
    return 0;
}

// ok = hs.RGMercsCommand(cmd)
int RGMercsCommand(lua_State* L) {
    UiHost& h = host_of(L);
    const char* cmd_c = luaL_checkstring(L, 1);
    if (plugin_code() || !h.run_rgmercs_command) {
        lua_pushboolean(L, 0);
        return 1;
    }
    bool ok = false;
    plugin::cxx_guard(L, [&] {
        std::string_view v(cmd_c);
        while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
        while (!v.empty() && v.back() == ' ') v.remove_suffix(1);
        if (v.empty()) return;
        std::string line(v);
        if (line.front() != '/') line = "/rgl " + line;
        if (h.rgmercs) h.rgmercs->append_log("info", "cmd: " + line);
        ok = h.run_rgmercs_command(line);
    });
    lua_pushboolean(L, ok);
    return 1;
}

} // namespace

void register_rgmercs_api(lua_State* L, UiHost& host) {
    void* mem = lua_newuserdatauv(L, sizeof(RGMercsCache), 0);
    new (mem) RGMercsCache();
    lua_newtable(L);
    lua_pushcfunction(L, cache_gc);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    const int cache = lua_gettop(L);

    for (auto [name, fn] : std::initializer_list<std::pair<const char*, lua_CFunction>>{
             {"RGMercsState", RGMercsState},
             {"RGMercsControl", RGMercsControl},
             {"RGMercsSetSetting", RGMercsSetSetting},
             {"RGMercsSaveSettings", RGMercsSaveSettings},
             {"RGMercsResetDefaults", RGMercsResetDefaults},
             {"RGMercsImportDb", RGMercsImportDb},
             {"RGMercsSetModule", RGMercsSetModule},
             {"RGMercsClearLogs", RGMercsClearLogs},
             {"RGMercsCommand", RGMercsCommand}}) {
        lua_pushlightuserdata(L, &host);
        lua_pushvalue(L, cache);
        lua_pushcclosure(L, fn, 2);
        lua_setfield(L, cache - 1, name);
    }
    lua_pop(L, 1);
}

} // namespace hailstone::app
