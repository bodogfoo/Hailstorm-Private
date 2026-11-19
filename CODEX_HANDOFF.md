# Project Hailstone 24 — Ubuntu Workstation Build & Validation Handoff

See `hailstone24/CODEX_HANDOFF.md` for complete architecture notes, verification results, and build/test commands.

## Summary
- **Baseline**: Clean upstream Hailstone commit `61e5135f78c6b492ad88ac029a469fee114f60cb`.
- **Native Combat Controller**: Implemented in C++20 (`src/combat/*`, `src/app/hailstone_combat_adapter.{h,cpp}`, `src/app/lua_ui_combat.{h,cpp}`) and native Modern Lua UI (`ui/modern/combat.lua`), with zero MacroQuest/RGMercs/MQ-plugin/`/stick` dependencies.
- **Preserved Systems**: MQ login/keyring import (`--import-mq-logins`), server/world auto-login, group profiles (`profiles/*.ini`, `.json`), per-character isolation, `ExecutionState`, `SessionBus` IPC, and Linux `GPU IPC`.
- **Legacy Archive**: Previous RGMercs/MQ compatibility work is preserved under `hailstone24/reference/rgmercs_mq_compat/`.

## Quick Build & Test on Ubuntu 24.04
```bash
git clone https://github.com/projecthailstone/hailstone-client.git hailstone24-src
cd hailstone24-src
git checkout 61e5135f78c6b492ad88ac029a469fee114f60cb
git apply /path/to/hailstone24/hailstone24.patch

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHAILSTONE_BROWSER=OFF -DHAILSTONE_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure -R "Combat|ExecutionController|FramePace|SessionBus|MQ AutoLogin|GroupProfile|Login auto-select|Server select|EQ menu|GPU IPC"
```
