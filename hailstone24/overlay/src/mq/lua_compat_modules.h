#pragma once

// Hailstone 24 MacroQuest / RG Mercs Lua Compatibility Modules:
//   - lsqlite3 (native SQLite3 binding incl. update_hook with userdata arg)
//   - lfs (LuaFileSystem: attributes, dir, mkdir, rmdir, currentdir, chdir, touch)
//   - actors (bridged to hailstone::ipc::SessionBusClient + local loopback)
//   - bit32 (Lua 5.1/5.2 compat bitwise library used by RG Mercs config_db.lua)
//   - mq.PackageMan, mq.ICONS, mq.set / mq.Set
//   - ImPlot, ImAnim, Zep lightweight compatible shims

#include <filesystem>
#include <functional>
#include <string>

#include <lua.hpp>

#include "ipc/session_bus.h"

namespace hailstone::mq {

struct CompatEnv {
    ipc::SessionBusClient* ipc_bus = nullptr;
    std::function<std::string()> character_name;
    std::function<std::string()> server_name;
    std::function<void(const ipc::ActorPacket&)> loopback_push;
    std::string script_name;
    std::filesystem::path config_dir;
};

// Registers built-in C/Lua modules into Lua's `hailstone.preloaded` registry table and globals (`bit32`).
// Must be called when opening a script's Lua state before running kPrelude or the script chunk.
void register_compat_modules(lua_State* L, const CompatEnv& env);

// Delivers a single inbound ActorPacket to all matching `actors.register(...)` handlers in `L`.
// Must be called inside ScriptRun / protected frame during ScriptHost::pulse().
void deliver_compat_actor(lua_State* L, const ipc::ActorPacket& pkt,
                          const std::function<void(const std::string& err)>& on_error = {});

// Pure JSON <-> Lua conversion helpers (also unit-tested directly).
std::string lua_value_to_json(lua_State* L, int idx, int depth = 0);
bool push_json_to_lua(lua_State* L, std::string_view json);

} // namespace hailstone::mq
