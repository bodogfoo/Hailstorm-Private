# Project Hailstone 24 — Ubuntu Workstation Build & Validation Handoff

## Baseline & Patch Summary (`bodogfoo/Hailstorm-Private`)
- **Upstream C++ Baseline**: `61e5135f78c6b492ad88ac029a469fee114f60cb`
- **Overlay & Unified Patch**:
  - `hailstone24/overlay/` contains the complete synced source-of-truth changes on top of `61e5135f`.
  - `hailstone24/hailstone24.patch` (99 changed/added files, regenerated cleanly via `git diff 61e5135f78c6b492ad88ac029a469fee114f60cb`) passes `git apply --check` on a clean `61e5135f` archive.
- **Workbench `main` vs C++ client repo**:
  - This repository holds the AI Studio Workbench (`package.json`, `server.ts`, React `src/`, and `hailstone24/`). Apply `hailstone24/hailstone24.patch` (or overlay `hailstone24/overlay/`) onto a clean checkout of `projecthailstone/hailstone-client` (`61e5135f`) on your Ubuntu workstation to build the native binaries and CMake/CTest suite.

---

## Implemented Capabilities (Preserved & Extended)
1. **MQ Commander-Style In-Game Control Layer (`src/ipc/session_bus.{h,cpp}`, `src/app/client_app*.{h,cpp}`, `src/app/lua_ui_combat.{h,cpp}`, `ui/modern/combat.lua`)**:
   - **Players / Clients tab**: Displays every connected Hailstone instance (`characterName`, `classShort`, `level`, `zoneShort`, `connectionState`, `combatState`, `scriptName`/`scriptState`, `Live`/`Headless` mode) with clickable client tabs that switch the active visible client without destroying or reconnecting instances, optional auto-headless demotion of the previously active client, and per-client controls (`Live`, `BG`, `Follow`, `Stop`, `Restart`).
   - **Group & Per-Client Controls**: Routes `follow`, `stop_follow`, `assist`, `attack_on`, `attack_off`, `pause_script`, `resume_script`, `reload_spells`, `apply_profile`, `camp`, `return_to_bind`, and server-authorized-only `tp_to_me`/`tp_here` over `SessionBus` with tracked sequence ACKs, failure reasons, and timeouts surfaced in `CombatWnd` (`API.commander.Status()` / `API.commander.Do(...)`).
2. **Hailstone-Native Group Profiles & MQ Import (`src/core/group_profile.{h,cpp}`, `src/core/mq_login_import.{h,cpp}`, `src/app/login_launch.{h,cpp}`, `src/ui/dev_screens.cpp`)**:
   - Named profiles (`Main Tank`, `Caster Group`, `Melee Group`, `Raid`, `Solo`, custom profiles) defining roster slots, roles, follow targets, combat profiles, script/module settings, spell loadouts, headless/live preferences, and startup order.
   - Read-only MacroQuest login/account/character/profile import with strict secret hygiene (passwords are stored only in OS keyring and never written to logs, exported profiles, or patches).
   - Trimmed whitespace handling in `is_favorite_world` (`src/ui/dev_screens.cpp`) so favorite servers pin properly at the top of `server_list_order`.
3. **Combat Engine & Spell Loadout Restoration (`src/combat/*`, `src/app/hailstone_combat_adapter.{h,cpp}`, `src/app/automation.{h,cpp}`)**:
   - Out-of-combat missing spell memorization (`rememorize_now` / `rememorize_missing`), manual gem protection (`auto_loadout` off by default, never overwriting manually changed gems in combat), and diagnostics for unavailable spells, missing spell data, level restrictions, and failed memorizations.
4. **RGMercs Integration & Security Hardening (`src/mq/lua_mq.cpp`, `src/mq/lua_compat_modules.{h,cpp}`, `src/mq/lua_hailstone.cpp`, `src/mq/rgmercs_bridge.{h,cpp}`, `ui/modern/rgmercs.lua`)**:
   - Observable script lifecycle transitions (`stopped`, `starting`, `running`, `paused`, `stopping`, `reloading`, `error`), `mq.PackageMan` / compatibility shims scoped strictly to trusted `rgmercs`, preserved `_G.io == nil` for general scripts, disk-first add-on capability-ceiling checks in `find_module` (`ModuleFile::refused` before preload lookup), and Lua stack-order fixes in `hailstone.rgmercs.*` bindings.
   - Typed `RGMercsBridge` defaults (`AutoAssistAt`, `MaxHealPct`, `BuffManaPct`, `PullRange`, etc.) for numeric parsing, range clamping, malformed-value fallback warnings, and read-only SQLite / data-only config import.
   - Strict `scripts/config` data-only loader and trusted `scripts/rgmercs/` root policy preserved.
5. **Multibox Execution States & Linux Cache/Test Isolation (`src/app/execution_state.h`, `src/platform/shared_file.{h,cpp}`, `tests/scratch_dir.h`, `tests/appearance_test.cpp`)**:
   - `ForegroundFull` vs `HeadlessBackground` transitions, `umask 0002` `ScratchDir` `0700` test isolation, verified single-user UPG `0775` ancestor allowance with strict `0700` leaf security and shared-group/world-writable rejection, and null-guarded `game_dir()` checks plus synthetic `.lay` test coverage.

---

## Files Changed / Added (`hailstone24.patch` — 99 files over `61e5135f`)
- **Build & Docs**: `CMakeLists.txt`, `cmake/Dependencies.cmake`, `docs/ui-api-reference.md`
- **Combat, Profiles & IPC**: `src/combat/{combat_engine,combat_profile,movement_controller,spell_manager}.{h,cpp}`, `src/combat/combat_types.h`, `src/core/{group_profile,mq_login_import}.{h,cpp}`, `src/ipc/session_bus.{h,cpp}`
- **Client App, UI & Execution State**: `src/app/{automation,client_app,hailstone_combat_adapter,login_launch,lua_ui,lua_ui_combat,lua_ui_menu}.{h,cpp}`, `src/app/client_app_{commands,draw,login,options,player,zone}.cpp`, `src/app/client_app_internal.h`, `src/app/{execution_state,frame_pace,lua_ui_plugins}.h`, `src/app/lua_ui_{misc,pregame,rgmercs,scripts}.cpp`, `src/app/lua_ui_api.inc`, `src/app/main.cpp`, `src/net/game_connection_social.cpp`, `src/ui/dev_screens.{h,cpp}`, `ui/modern/{combat,login,rgmercs,scripts,serverselect}.lua`, `ui/modern/lib/{escape,menuicons}.lua`
- **MQ / RGMercs Bridge**: `src/mq/{lua_compat_modules,lua_mq,rgmercs_bridge}.{h,cpp}`, `src/mq/{lua_hailstone,tlo_game}.cpp`, `src/mq/{lua_script,tlo}.h`
- **Platform, Render & Supervisor**: `src/platform/{child_processes,gpu_external_memory,gpu_ipc,paths,shared_file,window}.{h,cpp}`, `src/platform/{gpu_external_memory_export,gpu_ipc_server}.cpp`, `src/platform/gpu_ipc_internal.h`, `src/render/vk_context.{h,cpp}`, `tools/hailstone24_manager/{broker_main,manager_ui}.cpp`
- **Tests**: `tests/{appearance_test,gpu_ipc_test,shared_file_test}.cpp`, `tests/scratch_dir.h`, `tests/lua_ui_api_mock.lua`, `tests/hailstone24/{combat_controller_test,execution_state_test,group_profile_test,login_autoselect_test,mq_login_import_test,rgmercs_native_ui_test,rgmercs_trusted_loader_test,session_bus_test}.cpp`

---

## Ubuntu Workstation Build & Test Commands
### 1. Install Ubuntu 24.04 Dependencies
```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build git pkg-config python3 \
  libsqlite3-dev libsecret-1-dev libglib2.0-dev \
  libwayland-dev wayland-protocols libegl-dev libxkbcommon-dev libdecor-0-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
  vulkan-tools libvulkan-dev glslang-tools mesa-vulkan-drivers
```

### 2. Apply Patch & Build with CMake / CTest
```bash
git clone https://github.com/projecthailstone/hailstone-client.git hailstone-patched
cd hailstone-patched
git checkout 61e5135f78c6b492ad88ac029a469fee114f60cb
git apply /path/to/Hailstorm-Private/hailstone24/hailstone24.patch

cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DHAILSTONE_BROWSER=OFF \
  -DHAILSTONE_BUILD_TESTS=ON

cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure -R "Combat|ExecutionController|SessionBus|GroupProfile|MQ|RGMercs|Commander|Server select|Login auto-select|Lua|Animation|Appearance|Shared"
EQ_GAME_DIR=/path/to/EZ-RoF2 ./build/hailstone_tests
```

---

## Launching Two Clients (Runtime Steps)
1. **Start the Session Broker & Supervise 2 Roster Slots** (with `hailstone24_broker` + `hailstone` built in `./build`):
   ```bash
   ./build/hailstone24_broker \
     --data-dir "$HOME/.local/share/hailstone" \
     --game-dir /path/to/EZ-RoF2 \
     --hailstone-bin ./build/hailstone \
     --profile "Melee Group" \
     --launch-all
   ```
2. **Or launch two Hailstone client processes manually against a running broker socket**:
   - **Client 1 (Live foreground, e.g., Yavaha)**:
     ```bash
     ./build/hailstone \
       --game-dir /path/to/EZ-RoF2 \
       --data-dir "$HOME/.local/share/hailstone" \
       --session-id slot01 \
       --ipc-socket /tmp/hailstone24_bus.sock \
       --profile "Melee Group" --slot 1 \
       --auto-login
     ```
   - **Client 2 (Headless background, e.g., Stabbinyah)**:
     ```bash
     ./build/hailstone \
       --game-dir /path/to/EZ-RoF2 \
       --data-dir "$HOME/.local/share/hailstone" \
       --session-id slot02 \
       --ipc-socket /tmp/hailstone24_bus.sock \
       --profile "Melee Group" --slot 2 \
       --headless-bg --bg-tick-hz 20 --auto-login
     ```
3. In-game, open **CombatWnd** (`/combat`), select the **Players / Clients** tab to switch live focus between clients without reconnecting, issue group/per-client Commander actions (`Follow Me`, `Assist Me`, `Attack On/Off`, `Pause/Resume Script`, `Reload Spells`, `Apply Profile`, safe teleport commands), and monitor **RGMercsWnd** (`/rgl ui` or menu -> `RGMercs`) and ACK diagnostics.

---

## Verified Container Test Results & Known Limitations
### Directly Observed in Container (ASan + UBSan + LSan under `umask 0002`, Current Synced Overlay)
| Suite / Check | Result | Scope |
|---|---|---|
| `mq_lua_test` | **74/74 test cases, 621/621 assertions PASS** | Sandboxed MQ Lua host, preload/add-on capability ceilings, `_G.io == nil` |
| `rgmercs_trusted_loader_test` | **5/5 test cases, 86/86 assertions PASS** | Trusted `scripts/rgmercs/` root, config decoy rejection, `FailNow` numeric check |
| `rgmercs_native_ui_test` | **3/3 test cases, 102/102 assertions PASS** | `RGMercsBridge` SQLite/INI import, clamping, error banner, `hs.RGMercs*`/`hs.Commander*`, `combat.lua`/`rgmercs.lua` |
| `combat_controller_test` | **9/9 test cases, 144/144 assertions PASS** | State machine, rules, loadout gem protection, `set_follow_target`, E-Stop |
| `execution_state_test` | **2/2 test cases, 30/30 assertions PASS** | Foreground/Headless transitions, frame pacing |
| `group_profile_test` | **4/4 test cases, 175/175 assertions PASS** | Group profiles, save/load, MQ import compatibility |
| `login_autoselect_test` | **3/3 test cases, 20/20 assertions PASS** | World matching, keyword filter, whitespace-trimmed favorite world pinning |
| `mq_login_import_test` | **2/2 test cases, 43/43 assertions PASS** | MacroQuest INI import & credential hygiene |
| `session_bus_test` | **2/2 test cases, 64/64 assertions PASS** | `SessionBus` registration, command ACKs, timeouts |
| `shared_file_test` | **8/8 test cases, 126/126 assertions PASS** | `ScratchDir` `0700` isolation, UPG `0775` vs shared-group/world-writable security |
| `animation_frames_test` | **11/11 test cases, 4143/4143 assertions PASS** | Animation mmap ownership & trim under `umask 0002` |
| `appearance_test` | **15/15 ran, 254/254 assertions PASS (4 skipped)** | Synthetic EQG `.lay` variants & actor look (4 skipped require real `EQ_GAME_DIR`) |
| `lua_ui_contract_test` | **6/6 test cases, 1479/1479 assertions PASS** | All Lua UI API bindings, capability masks, doc/mock sync |
| Python CI (`tools/*.py` & `tests/test_*.py`) | **5/5 CI checks PASS + 15/15 unittest OK** | Core names, `migrate_ui_api`, `--mock-adapter`, `gen_ui_api_docs`, `sync_wiki` |
| `git apply --check hailstone24.patch` | **PASS** | Cleanly applies to upstream `61e5135f` |

### Container Limitations (To Verify on Workstation)
- **No CMake / SDL3 / Vulkan / Chromium CEF toolchain or `EQ_GAME_DIR` RoF2 assets in this container**: C++ tests were compiled and linked with `g++ -std=c++20 -fsanitize=address,undefined` directly against pinned FetchContent dependencies, and non-tested GUI/Vulkan helper symbols (`ActionsUi`, `Gamepad`, `UiTextures`, `SpellIconCache`, `dev_screens` widget helpers, `platform::open_folder`/`open_url`) were linked via a temporary in-`/tmp` headless stub (`headless_ui_stubs.cpp`, not included in `hailstone24/` or `hailstone24.patch`). Full `cmake --build`, multi-process `hailstone24_broker`, Vulkan rendering, `gpu_ipc_test`, and live EZ Server login/RGMercs runs against real RoF2 files must be executed on the Ubuntu workstation.
- **No `git push` access to `bodogfoo/Hailstorm-Private`**: `hailstone24/hailstone24.patch`, `hailstone24/overlay/`, and handoff docs are updated locally in the workbench workspace ready for commit/push.
