# Project Hailstone 24 — Ubuntu Workstation Build & Validation Handoff

## What Was Built
- **Decoupled `ExecutionState` Engine**: `Disconnected`, `BackgroundHeadless`, `Transitioning`, `ForegroundFull`. Background clients (`--headless-bg`) hide their window, suspend Vulkan swapchains (`vk_.suspend_for_background()`), mute audio, and skip ImGui/particle/Lua UI builds while continuing network packets, `StepClock` physics, `Detour` navigation, and `RG Mercs` Lua coroutines at `--bg-tick-hz` (default `25 Hz`, range `10..60 Hz`).
- **Local `AF_UNIX` IPC Bus & Process Supervisor (`hailstone24_broker`, `hailstone24_manager`)**: Same-UID `SO_PEERCRED` socket for `< 250 ms` foreground switching (`/switch` or manager hotkeys/click), `/bca`/`/bcaa`/`/bc`/`/rc`/`/dgt`/`/dquery`/`/dobserve` dispatch with ACKs, and `require('actors')` mailbox routing.
- **Linux Shared GPU IPC (`VK_KHR_external_memory_fd` + `SCM_RIGHTS`)**: Unlocks `hailstone_gpu_cache_broker` and `--gpu-mode shared-broker` alongside `cached` (default for 32 GB Intel Arc Pro B70) and `deferred`.
- **MacroQuest AutoLogin Importer (`--import-mq-logins`) & End-to-End Auto-Login (`--auto-login`, `--world`, `--auto-kick`)**: Read-only import (`SQLITE_OPEN_READONLY`) of MQ `login.db` (plaintext & `master_pass` XOR-encrypted), `MQ2AutoLogin.ini`, and `server^acct^char^pass` files into Hailstone's isolated `--data-dir` + `libsecret` keyring, writing a passwordless `[global]`/`[slot01..slot24]` `hailstone24_roster.ini`. Supports full end-to-end auto-login (server -> world -> character -> enter world), interactive `"Auto-login"` toggle (`login.auto_login`), Server Select keyword filtering, case-insensitive favorite pinning at the top, and automatic kick-existing confirmation (`--auto-kick` / `login.auto_kick=1` / headless mode). **Existing EverQuest & MacroQuest files are never modified.**
- **RG Mercs Compatibility Layer**: Native C `lsqlite3` (with 5-arg `update_hook`) & `lfs`, IPC-bridged `actors`, `bit32`, `mq.DataType.new`, `mq.AddTopLevelObject`, `mq.PackageMan`, `mq.ICONS`, `mq.Set`, `ImPlot`/`ImAnim`/`Zep` shims, and `Raid`, `AltAbility`, `InvSlot`, `MacroQuest`, `Lua`, `DanNet`, `Mercenary`, `Nav` TLOs.

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
> Verify your Intel Arc Pro B70 is visible to Vulkan:
> `vulkaninfo --summary`

---

## 2. Apply Patch & Build
```bash
git clone https://github.com/projecthailstone/hailstone-client.git hailstone24-src
cd hailstone24-src
git apply /path/to/hailstone24/hailstone24.patch

cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DHAILSTONE_BROWSER=OFF \
  -DHAILSTONE_BUILD_TESTS=ON

cmake --build build -j"$(nproc)"
```
*(Set `-DHAILSTONE_BROWSER=ON` if you want CEF in-game browser support.)*

---

## 3. Run Unit & Integration Tests
```bash
ctest --test-dir build --output-on-failure -R "ExecutionController|FramePace|SessionBus|MQ AutoLogin|Login auto-select|Server select|RG Mercs|GPU IPC"
# Or run the full upstream + Hailstone 24 suite:
./build/hailstone_tests
```

---

## 4. Import All MacroQuest Logins Safely (Isolated Data Directory)
Never point `--data-dir` inside your EverQuest folder. Use an isolated directory such as `~/.local/share/hailstone24`:
```bash
export HAILSTONE_DATA_DIR="${HOME}/.local/share/hailstone24"
mkdir -p "${HAILSTONE_DATA_DIR}"

read -s -p "MQ Master Password (Enter if none): " MQ_MASTER_PASSWORD && echo
export MQ_MASTER_PASSWORD

./build/hailstone/hailstone \
  --data-dir "${HAILSTONE_DATA_DIR}" \
  --import-mq-logins "/path/to/macroquest/config/login.db" \
  --roster-out "${HAILSTONE_DATA_DIR}/hailstone24_roster.ini"

unset MQ_MASTER_PASSWORD
```
Copy `hailstone24/configs/hailstone24.ini` to `"${HAILSTONE_DATA_DIR}/hailstone.ini"` and set `[assets] path=/path/to/your/everquest_rof2`.

---

## 5. Install RG Mercs Into Hailstone Isolated Scripts Folder
```bash
git clone https://github.com/DerpleDude/rgmercs.git "${HAILSTONE_DATA_DIR}/scripts/rgmercs"
```

---

## 6. Launch & Manage 24 Sessions
Terminal 1 (Broker + 24 Staggered Clients — Slot 1 Foreground, Slots 2–24 Headless):
```bash
./build/hailstone/hailstone24_broker \
  --data-dir "${HAILSTONE_DATA_DIR}" \
  --game-dir "/path/to/your/everquest_rof2" \
  --roster "${HAILSTONE_DATA_DIR}/hailstone24_roster.ini" \
  --launch-all \
  --bg-tick-hz 25 \
  --gpu-mode cached \
  --stagger-ms 400
```

Terminal 2 (Interactive GUI or Terminal Switcher):
```bash
./build/hailstone/hailstone24_manager

./build/hailstone/hailstone24_manager --status
./build/hailstone/hailstone24_manager --switch slot05
./build/hailstone/hailstone24_manager --cmd "/bcaa /lua run rgmercs"
```

---

## 7. Run the 24-Box Benchmark & Hardware Validation
```bash
chmod +x hailstone24/scripts/bench_24box.sh
./hailstone24/scripts/bench_24box.sh ./build/hailstone "${HAILSTONE_DATA_DIR}/hailstone24_roster.ini"
```
