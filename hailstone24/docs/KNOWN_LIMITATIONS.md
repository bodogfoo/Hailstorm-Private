# Known Limitations & Honest Verification Boundaries

1. **No C++20 / Vulkan Compiler in Workspace Container**:
   - This authoring container holds Node.js, Git, and TypeScript tooling without `g++`/`clang++`, `cmake`, `ninja`, or Vulkan SDK headers installed.
   - Therefore, while `hailstone24.patch` has been verified with `git apply --check` cleanly against the upstream `hailstone-client` tree and carefully audited for headers, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` warnings, and upstream type signatures, **C++ compilation, `ctest`, and 24-box runtime benchmarks must be executed on the target Ubuntu workstation** following `CODEX_HANDOFF.md`.

2. **MQ `login.db` Windows DPAPI Blobs**:
   - Plaintext `login.db` entries and XOR-encrypted entries (using MacroQuest Next's `master_pass` XOR key routine) are imported and decrypted natively on Linux.
   - Windows-specific DPAPI (`CryptProtectData`) blobs encrypted by a Windows machine cannot be decrypted natively on Ubuntu; the importer skips them safely, emits a clear warning naming the account, and avoids corrupting keyring entries. Workaround: export from MQ with `--decrypt` / caretaker INI or supply `--mq-master-pass`.

3. **RG Mercs `ImPlot` / `ImAnim` / `Zep` Rendering Fidelity**:
   - Lightweight Lua compatibility shims are registered so RG Mercs modules importing `ImPlot`, `ImAnim`, or `Zep` load and initialize cleanly without `module not found` or nil-call panics.
   - Because Hailstone 24 suppresses ImGui and 3D rendering across all 23 background clients (`skip_ui_build = true`), visual HUD charts are only relevant to the single foreground client (`FOREGROUND_FULL`) and use fallback tables unless full C++ `ImPlot` bindings are linked later.

4. **Shared GPU Broker (`gpu_mode=shared-broker`) Requires Validation on Mesa ANV / Intel Arc Pro B70**:
   - Linux `VK_KHR_external_memory_fd` (`OPAQUE_FD_BIT`) + `AF_UNIX`/`SCM_RIGHTS` FD passing has been implemented to unblock `hailstone_gpu_cache_broker` on Linux.
   - `gpu_mode=cached` (the default on 32 GiB VRAM Arc Pro B70) keeps Vulkan device allocations local and avoids cross-process synchronization risk while fitting 24 sessions comfortably within ~8–12 GiB VRAM. Test `gpu_mode=shared-broker` via `hailstone_gpu_share_probe` before promoting it for unattended raids.

5. **Legacy `.mac` Macros**:
   - Upstream Hailstone intentionally implements MacroQuest Lua (`MQ2Lua`) rather than the legacy `.mac` interpreter. Use Lua automation (`RG Mercs`, custom Lua scripts) and `/bca`/`/bcaa`/`/rc` commands.
