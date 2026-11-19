# Project Hailstone 24 — Ubuntu Workstation Build & Validation Handoff

## Clean Baseline & Architecture
- **Upstream Baseline Commit**: `61e5135f78c6b492ad88ac029a469fee114f60cb`
- **Legacy Archive**: Earlier MacroQuest/RGMercs Lua compatibility artifacts are archived under `hailstone24/reference/rgmercs_mq_compat/` and excluded from `hailstone24.patch` and `hailstone24/overlay/`.
- **Native Hailstone Combat Controller (`src/combat/*`, `src/app/hailstone_combat_adapter.{h,cpp}`, `src/app/lua_ui_combat.{h,cpp}`, `ui/modern/combat.lua`)**:
  - **Zero MacroQuest / RGMercs Dependencies**: No RGMercs Lua scripts, no MQ plugins, no MQ TLO evaluations, and no `/stick` commands.
  - **9-State FSM (`CombatEngine`)**: `Idle`, `Assist`, `Engage`, `Combat`, `Disengage`, `Retreat`, `Dead`, `Paused`, `EmergencyStop`. Every state transition logs timestamp, `from -> to`, and reason; `Idle` computes an explicit diagnostic `idleReason`.
  - **Hard Safety Rules & Emergency Stop**: Immediate halt of movement, spell casting, auto-attack, and pet attack on `Stop`, target death/disappearance/invalidity, range exit, or `EmergencyStop` (`EMERGENCY STOP` button in UI, `/estop`, `/combat estop`, or IPC bus `/estop`). `EmergencyStop` latches until explicit `Reset E-Stop` (`/combat reset_estop`).
  - **Native `NavControl` Movement (`MovementController`)**: Supports `None`, `Follow`, `AssistRange`, `MeleeRange` (including `melee_behind` positioning for ROG), `RangedRange`, and `DisengageDistance` via `NavControl`. Manual player movement or backing away triggers a `Paused` manual-override cooldown (`manual_override_cooldown_ms`) without snapping back.
  - **Native Spell & Ability Management (`SpellManager`)**: Reads `GameConnection::spell_state()`, `spell_table()`, and `use_ability_named()`. Supports pipe-separated fallback spell lines, cooldown/mana/HP/range/combat-state rule conditions, and gem slot auditing. **Never overwrites manually changed gems unless `auto_loadout=true`**; safely rememorizes missing profile spells only into empty slots when `rememorize_missing=true`. Built-in class profiles prioritize **NEC**, followed by **ROG**, **CLR**, **WAR**, and fallback EQ classes.
  - **Native INI Profile Format & Security (`CombatProfile`)**: Stores character profiles at `<character_dir>/combat_profile.ini` and optional group defaults at `${HAILSTONE_DATA_DIR}/profiles/<group_id>.combat.ini` (examples in `hailstone24/configs/combat/{nec,rog,clr,group_defaults}_example.ini`). Enforces secret-key filtering (`is_combat_secret_key`), canonical root confinement (`is_path_within_root`), and read-only migration from `<character_dir>/hailstone.ini`.
  - **Native UI (`CombatWnd`) & Slash Commands**: Accessible via EQ cascade menu `Combat Controller`, `/combat`, or `/estop`. Displays live state, target/assist telemetry, `Idle`/transition reason banner, `Dry-Run Sim` toggle, manual-gem protection status, rules table, gem loadout audit table, and transition/action logs.
- **Preserved Hailstone Systems**:
  - MacroQuest login/password import (`--import-mq-logins`, `SQLITE_OPEN_READONLY`) into system keyring (`libsecret`) + passwordless `hailstone24_roster.ini`.
  - Server/world selection, auto-login (`--auto-login`, `--auto-kick`), group profiles (`profiles/<name>.ini` / `.json`), and per-character isolation.
  - Multi-session `ExecutionState` (`ForegroundFull` / `BackgroundHeadless`), `SessionBus` `AF_UNIX` IPC, Linux `GPU IPC`, `hailstone24_broker`, and `hailstone24_manager`.

---

## Container Verification Results (Verified Before Handoff)
- **Patch & Overlay Integrity**: `git apply --check` and `git apply` applied `hailstone24/hailstone24.patch` cleanly onto upstream `61e5135f78c6b492ad88ac029a469fee114f60cb` with `0` `rgmercs` hits across `src/`, `ui/`, and `CMakeLists.txt`.
- **C++20 Unit Tests (`g++ -std=c++20 -Wall -Wextra -Werror`)**:
  - `combat_controller_test`: **7/7 test cases, 123/123 assertions passed**.
  - `execution_state_test`: **2/2 test cases, 30/30 assertions passed**.
  - `session_bus_test`: **1/1 test case, 21/21 assertions passed**.
  - Example INI Loader Smoke Test (`nec_example.ini`, `rog_example.ini`, `clr_example.ini`, `group_defaults_example.ini`): **4/4 loaded and validated**.
- **UI Contract & Lua Checks**:
  - `lua_ui_api.cpp` `static_assert(valid_catalog())` compiled cleanly with `g++ -std=c++20`.
  - Lua 5.4 syntax checks passed on `ui/modern/combat.lua`, `ui/modern/lib/escape.lua`, `ui/modern/lib/menuicons.lua`, and all `ui/modern/*.lua` files.
*(Note: Full Vulkan/SDL3/CEF client linking and live EverQuest server testing must be run on Ubuntu 24.04 workstation as detailed below.)*

---

## 1. Install Ubuntu 24.04 Dependencies
```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build git pkg-config python3 \
  libsqlite3-dev libsecret-1-dev libglib2.0-dev \
  libwayland-dev wayland-protocols libegl-dev libxkbcommon-dev libdecor-0-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
  vulkan-tools libvulkan-dev glslang-tools mesa-vulkan-drivers
```
> Verify GPU Vulkan visibility: `vulkaninfo --summary`

---

## 2. Apply Patch & Build
```bash
git clone https://github.com/projecthailstone/hailstone-client.git hailstone24-src
cd hailstone24-src
git checkout 61e5135f78c6b492ad88ac029a469fee114f60cb
git apply /path/to/hailstone24/hailstone24.patch

cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DHAILSTONE_BROWSER=OFF \
  -DHAILSTONE_BUILD_TESTS=ON

cmake --build build -j"$(nproc)"
```

---

## 3. Run Unit & Integration Tests
```bash
ctest --test-dir build --output-on-failure -R "Combat|ExecutionController|FramePace|SessionBus|MQ AutoLogin|GroupProfile|Login auto-select|Server select|EQ menu|GPU IPC"
# Or run full test binary:
./build/hailstone_tests
```

---

## 4. Import Logins & Configure Isolated Data Directory
```bash
export HAILSTONE_DATA_DIR="${HOME}/.local/share/hailstone24"
mkdir -p "${HAILSTONE_DATA_DIR}/profiles"

read -s -p "MQ Master Password (Enter if none): " MQ_MASTER_PASSWORD && echo
export MQ_MASTER_PASSWORD

./build/hailstone/hailstone \
  --data-dir "${HAILSTONE_DATA_DIR}" \
  --import-mq-logins "/path/to/macroquest/config/login.db" \
  --roster-out "${HAILSTONE_DATA_DIR}/hailstone24_roster.ini"

unset MQ_MASTER_PASSWORD
```
Copy `hailstone24/configs/hailstone24.ini` to `"${HAILSTONE_DATA_DIR}/hailstone.ini"` and configure `[assets] path=/path/to/everquest_rof2`.
Optionally copy `hailstone24/configs/combat/*.ini` templates into `"${HAILSTONE_DATA_DIR}/profiles/<group>.combat.ini"` or character folders as `combat_profile.ini`.

---

## 5. Launch & Smoke Test Native Combat Controller
```bash
./build/hailstone/hailstone \
  --data-dir "${HAILSTONE_DATA_DIR}" \
  --profile ez_6box_example \
  --slot Stabbinyah \
  --auto-login
```
Verify in-game (NEC first, then ROG / CLR / WAR):
1. Open `Combat Controller` from the EQ menu or run `/combat`.
2. Toggle `Dry-Run` ON, click `Start`, target/assist an NPC, and verify `Transitions & Log` records `[DRY-RUN]` decisions and explicit `Idle` reasons without casting or moving.
3. Verify manual back-away (`S` / `BACK`) pauses controller without snapping back.
4. Inspect `Spell Loadout` tab: verify manual gem changes show `ManualChange` and are not overwritten when `Auto Loadout` is OFF.
5. Disable `Dry-Run`, test normal engage/cast/melee, then press `EMERGENCY STOP` or type `/estop` and verify casting, movement, auto-attack, and pet attack halt immediately until `Reset E-Stop`.
