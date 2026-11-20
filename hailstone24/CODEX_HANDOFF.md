# Project Hailstone 24 — Ubuntu Workstation Build & Validation Handoff

## Sync Status & Branch Topology (`bodogfoo/Hailstorm-Private`)
- **Upstream C++ Baseline**: `61e5135f78c6b492ad88ac029a469fee114f60cb`
- **Native C++ Source of Truth**: `codex/native-hailstone-sync` (`14ccd108`) — full `hailstone-client` C++ checkout containing the built native combat engine, multibox/headless execution states, `SessionBus`, group profiles, auto-login, UI contract fixes (`ServerSelectSet`, `tests/lua_ui_api_mock.lua`), `Automation::run_command`, `ack.seq`, `config_.set`, and the `set_follow_target` follow-mode fix.
- **Workbench `main` (`3ce8b577`) vs `codex/native-hailstone-sync` (`14ccd108`)**:
  - `main` (`3ce8b577`) is the AI Studio Workbench layout (`package.json`, `server.ts`, React `src/`, `hailstone24/`), while `codex/native-hailstone-sync` (`14ccd108`) is the full `hailstone-client` C++ layout (`CMakeLists.txt`, C++ `src/`, `tests/`, `tools/`, `ui/`). They have disjoint Git histories (`git merge-base` returns no common ancestor).
  - Do **not** run a raw `git merge codex/native-hailstone-sync` onto the workbench `main` (`3ce8b577`), as that mixes React `src/` with C++ `src/` or overwrites files. Instead:
    - `hailstone24/overlay/` and `hailstone24/hailstone24.patch` in this workbench are now synced to `14ccd108` (`61e5135f` baseline) plus regenerated `tests/lua_ui_api_mock.lua` (`CombatDo`/`CombatStatus`) and `docs/ui-api-reference.md` and a unit test for `set_follow_target` in `tests/hailstone24/combat_controller_test.cpp`.
    - Legacy RGMercs/MQ compatibility work remains preserved under `hailstone24/reference/rgmercs_mq_compat/` and is excluded from `hailstone24.patch` and `hailstone24/overlay/`.

---

## Container Verification Results (Verified on Synced `61e5135f` + `hailstone24.patch` Tree)
- **Patch & Overlay Integrity**: `git apply --check` passed on a clean `61e5135f78c6b492ad88ac029a469fee114f60cb` worktree for the regenerated 82-file `hailstone24/hailstone24.patch` (preserving all 77 native Hailstone 24 files plus 5 test/platform fix files; `hailstone24/reference/` untouched).
- **Failure #1 (`animation_frames_test.cpp` + `shared_file_test.cpp` under Ubuntu `umask 0002`) — Reproduced & Fixed**:
  - **Baseline reproduction**: Under `umask 0002`, `animation_frames_test.cpp` failed 5 test cases (`owner.mapped() == false`, `clip.tracks[i].mapped() == false`, `decoded_capacity() == 2 != 0`, `cleanup.trim(0) == false`, `bytes.mapped() == false`) and `shared_file_test.cpp` failed 5 test cases because `ScratchDir` inherited `0775` and `shared_file.cpp` rejected all `0775` ancestors.
  - **Fix**:
    - `tests/scratch_dir.h`: Explicitly sets `0700` (`perms::owner_all`) on newly created `ScratchDir` roots on POSIX after `create_directories`, isolating tests from permissive process umasks without modifying `/tmp`.
    - `src/platform/shared_file.{h,cpp}`: Preserves strict `0700` enforcement on cache leaves and rejects foreign-owned, world-writable (`0777`), and genuinely shared writable group (`0775`) ancestors, while allowing user-owned `0775` ancestors belonging to the current user's verified single-user User Private Group (`getpwuid_r` + `getgrgid_r` where `gr_gid == pw_gid`, `gr_name == pw_name`, and `gr_mem` is empty or contains only `pw_name`).
    - `tests/shared_file_test.cpp`: Added regression tests for `ScratchDir` under `umask(0002)` and for the UPG vs genuinely shared writable group / world-writable / non-private leaf policy (both deterministic policy checks and real filesystem checks).
- **Failure #2 (`appearance_test.cpp` `"EQG texture variants come from the model's .lay layers"`, `SIGSEGV`) — ASan+UBSan & Release `-O3` Findings**:
  - **Directly reproduced SIGSEGV**: When `EQ_GAME_DIR` is unset and `doctest` runs with `--no-skip`, `tests/appearance_test.cpp:258` (`const auto& game = *hailstone::test::game_dir();`) dereferences a null pointer (`UBSan: reference binding to null pointer of type 'const struct GameDir'` -> `src/core/game_dir.cpp:164` `GameDir::index()` -> `SIGSEGV` in Release `-O3`). Combined with the 5 `umask 0002` failures in `animation_frames_test.cpp`, this accounts for 6 failures before aborting the suite.
  - **Synthetic RoF2 `GUS` `.eqg`/`.lay` check**: Tested `CharacterLibrary::load`, `lib.model("GUS")`, and `lib.dress(*gus, a)` against a synthetic `gus.eqg` (EQGS v1 `gus.mds`, EQGL v4 `gus.lay`, EQGA v1 `stnd_ba_1_gus.ani`, `Resources/OnDemandResources.txt`) under both ASan+UBSan and Release `-O3` (`umask 0022` and `umask 0002`): 6/6 assertions passed with 0 sanitizer errors.
  - **Fix & coverage**: Added defensive null guards (`const auto* game_ptr = hailstone::test::game_dir(); if (!game_ptr) return;`) to the 4 `game_dir()` tests in `tests/appearance_test.cpp` without skipping tests or weakening any assertions, and added a self-contained synthetic `GUS` `.lay` dressing test (`"EQG texture variants come from synthetic .lay layers without EQ_GAME_DIR"`) that always runs without `EQ_GAME_DIR`.
  - **Workstation note**: Real RoF2 game files (`EQ_GAME_DIR`) are not present in this container. If `appearance_test.cpp` still crashes on your Ubuntu workstation when `EQ_GAME_DIR` points to your real RoF2 directory, capture a stack trace with:
    `EQ_GAME_DIR=/path/to/EZ-RoF2-clean-20260828 gdb -batch -ex run -ex bt --args ./build/hailstone_tests -tc="EQG texture variants come from the model's .lay layers"`
- **C++23 ASan+UBSan & Release `-O3` Verification Results**:
  - `animation_frames_test` + `shared_file_test` + `appearance_test` (ASan+UBSan, `umask 0022` and `umask 0002`, unset `EQ_GAME_DIR`): **34/34 passed, 4523/4523 assertions**; with `--no-skip`: **38/38 passed, 0 skipped, 4523/4523 assertions**; with synthetic `EQ_GAME_DIR`: **2/2 EQG `.lay` test cases passed, 25/25 assertions** (both ASan+UBSan and Release `-O3`).
  - `combat_controller_test` + `execution_state_test` + `session_bus_test` (ASan+UBSan, `umask 0002`): **11/11 passed, 179/179 assertions**.
  - `lua_ui_contract_test`: `tests/lua_ui_contract_test.cpp`, `src/app/lua_ui_api.cpp`, `src/app/lua_ui_caps.cpp`, and `src/app/lua_ui_combat.cpp` compiled cleanly with `g++ -std=c++23 -fsanitize=address,undefined` against vendored Lua 5.4 + ImGui `v1.91.9`, and the ASan+UBSan contract table harness verified **349 entries (343 functions, 6 constants), 0 duplicates, all capability masks matching**. (Full `hailstone_tests -sf="*lua_ui_contract_test.cpp"` requires the CMake/SDL3/Vulkan workstation build.)
- **UI-API & Python CI Checks (`docs/ci.md`)**:
  - `python3 tools/check_core_names.py`: **PASS**.
  - `python3 tools/migrate_ui_api.py --check`: **0 pending Lua files**.
  - `python3 tools/migrate_ui_api.py --mock-adapter --check`: **PASS**.
  - `python3 tools/gen_ui_api_docs.py --check`: **PASS**.
  - `python3 tools/sync_wiki.py --check`: **PASS**.
  - `python3 -m unittest discover -s tests -p 'test_*.py'`: **15/15 tests OK**.

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

## 2. Build & Verify `codex/native-hailstone-sync` (Ubuntu 24.04)
```bash
git clone https://github.com/bodogfoo/Hailstorm-Private.git hailstorm-client
cd hailstorm-client
git checkout codex/native-hailstone-sync

# Optional: apply the UI-API mock/docs CI sync + follow-mode unit test on top of 14ccd108:
# (Or start from upstream 61e5135f and `git apply /path/to/hailstone24/hailstone24.patch`)

cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DHAILSTONE_BROWSER=OFF \
  -DHAILSTONE_BUILD_TESTS=ON

cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure -R "Combat|ExecutionController|FramePace|SessionBus|MQ AutoLogin|GroupProfile|Login auto-select|Server select|EQ menu|GPU IPC|UI contract"
./build/hailstone_tests
```

---

## 3. Recommended Git Branch Layout on `Hailstorm-Private`
Because `main` (`3ce8b577`) is the Workbench tree and `codex/native-hailstone-sync` (`14ccd108`) is the C++ client tree:
- **If keeping `main` as the Workbench repository**: keep C++ client builds on `codex/native-hailstone-sync` (or a dedicated `hailstone-client` branch) and sync this updated `hailstone24/` directory into `main` without deleting `src/`, `server.ts`, or `hailstone24/reference/`.
- **If promoting `codex/native-hailstone-sync` to be the C++ `main` branch**: first preserve the current workbench `main` safely on a dedicated branch before moving `main`:
```bash
git branch workbench-archive origin/main
git push origin workbench-archive
git checkout -B main codex/native-hailstone-sync
git push origin main
```
