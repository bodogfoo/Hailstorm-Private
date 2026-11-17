# RG Mercs Compatibility Matrix (Project Hailstone 24)

Target baseline inspected: `DerpleDude/rgmercs`, `peonMQ/MQRemote`, `macroquest/macroquest`, and `projecthailstone/hailstone-client`.

| Category | Feature / API | Status | Implementation | Notes |
| --- | --- | --- | --- | --- |
| **Lua Modules** | `mq.PackageMan.Require('lsqlite3')` (`exec`, `nrows`, `update_hook`) | **Supported** | `src/mq/lua_compat_modules.cpp` | Native C SQLite3 binding; fires 5-arg `update_hook(ud, op, dbName, tableName, rowId)`. |
| **Lua Modules** | `mq.PackageMan.Require('luafilesystem', 'lfs')` | **Supported** | `src/mq/lua_compat_modules.cpp` | `std::filesystem`-backed `mkdir`, `rmdir`, `dir`, `attributes`. |
| **Lua Modules** | `require('actors')` (`register`, `send`, dropbox mailbox) | **Supported** | `src/mq/lua_compat_modules.cpp`, `src/ipc/session_bus.cpp` | Cross-session `AF_UNIX` JSON transport + same-client loopback. |
| **Lua Modules** | `bit32`, `mq.ICONS`, `mq.Set` / `mq.set` | **Supported** | `src/mq/lua_compat_modules.cpp` | Lua 5.4 bitwise implementation + preloaded table registry. |
| **Lua Runtime** | `mq.DataType.new()`, `mq.AddTopLevelObject('RGMercs', ...)` | **Supported** | `src/mq/lua_mq.cpp` | Custom TLO metatable chain wired directly into `mq.TLO`. |
| **IPC Commands** | `/bca`, `/bcaa`, `/bcz`, `/bcaaz`, `/bcg`, `/bcaag`, `/bct`, `/bc`, `/rc` | **Supported** | `src/mq/commands.cpp` | Routes over local `AF_UNIX` `SessionBusServer`, normalizes `//cmd -> /cmd`. |
| **IPC Commands** | `/dgt`, `/dge`, `/dga`, `/dgae`, `/dex`, `/dquery`, `/dobserve`, `/switch` | **Supported** | `src/mq/commands.cpp`, `src/app/client_app_internal.h` | Live peer queries via cached telemetry + explicit foreground switching. |
| **TLOs** | `Me.AAPoints*`, `AltAbility`, `Song`, `MaxBuffSlots`, debuff booleans, `XTargetSlots`, `PctAggro` | **Supported** | `src/mq/tlo_game.cpp` | Uses Hailstone `alt_adv()`, `eq_strings`, `aggro()`, and buff slots. |
| **TLOs** | `Spawn.Aggressive/Beneficial/FeetWet/PctAggro/AggroHolder`, `Raid.*`, `Group.Injured/LowMana` | **Supported** | `src/mq/tlo_game.cpp` | Full `Raid` & `raidmember` spawn conversion + injured/low-mana counters. |
| **TLOs** | `DanNet.*`, `MacroQuest.*`, `Lua.Script().Status()`, `InvSlot`, `Mercenary`, `Nav.*`, `MoveTo.*` | **Supported** | `src/mq/tlo_game.cpp` | Includes `EverQuest.Foreground` tied to Hailstone `ExecutionState`. |
| **Logins** | `--import-mq-logins` (`login.db` w/ XOR decryption, `MQ2AutoLogin.ini`, caret lists) | **Supported** | `src/core/mq_login_import.cpp` | Reads MQ files read-only; writes Hailstone isolated `servers/` + keyring only. |
| **UI Shims** | `ImPlot`, `ImAnim`, `Zep`, `mq.CreateTexture`, `mq.FindTextureAnimation` | **Partial** | `src/mq/lua_compat_modules.cpp`, `src/mq/lua_mq.cpp` | No-crash UI table/function stubs; heavy ImGui rendering skipped when `BACKGROUND_HEADLESS`. |
| **Legacy** | `.mac` MacroQuest script engine | **Unsupported** | Upstream Hailstone | Hailstone targets modern MQ2Lua (`RG Mercs`) rather than `.mac` macro files. |
