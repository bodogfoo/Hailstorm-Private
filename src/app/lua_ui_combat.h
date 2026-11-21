#pragma once

struct lua_State;

namespace hailstone::app {

struct UiHost;

void register_combat_api(lua_State* L, UiHost& host);

} // namespace hailstone::app
