#pragma once

struct lua_State;

namespace hailstone::app {

struct UiHost;

// Registers hs.EpicStatus, hs.EpicItems, hs.EpicAudit, hs.EpicDo, hs.EpicExport,
// and hs.EpicImport on the `hs` table at the top of the Lua stack.
void register_epic_api(lua_State* L, UiHost& host);

} // namespace hailstone::app
