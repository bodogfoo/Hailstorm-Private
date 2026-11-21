# Architecture Decision Record: Project Hailstone 24

## Context & Target Hardware
- **Objective**: Run **24 concurrent EverQuest clients** (RoF2 protocol, EZ Server) with full automation (`RG Mercs`, `Lua` scripts, `Detour/Recast` navigation, `/bca`/`/bcaa`/`/rc`/`actors` IPC), while rendering **only 1 active foreground viewport** at full Hailstone Vulkan fidelity and suppressing 3D/UI/audio overhead for the remaining 23 background sessions.
- **Workstation Budget**:
  - OS: Ubuntu Linux
  - GPU: Intel Arc Pro B70 (**32 GiB VRAM**)
  - System RAM: **48 GiB** (~2.0 GiB ceiling per process max, targeting ~0.7–1.2 GiB PSS per headless background process via OS copy-on-write and `shared_file_cache` mmap)
- **Hard Constraint**: Preserve upstream Hailstone behavior and visuals, avoid rewriting single-instance core globals across 100+ subsystems unless strictly necessary, and keep existing EverQuest/MacroQuest user directories completely untouched.

---

## Investigation of Upstream Hailstone (`display.background_fps = -1`)

Inspection of upstream `src/app/client_app.cpp`, `src/app/frame_pace.h`, and `src/render/vk_context.cpp` revealed four bottlenecks that break 24-box automation under upstream `-1`:
1. **CPU Simulation Coupled to Render Loop**: Even when `pace.draw == false` (`background_fps = -1`), upstream `ClientApp::run()` still executed full `ImGui::NewFrame()`, full Lua UI HUD frames (`lua_ui_->frame()`), spell/zone/actor particle emitters, audio spatial mixing, and `zone_settling()` waited for rendered `scene` frames that were suppressed!
2. **Stuck Zone Transitions**: In `ClientApp::update_zone_settle()`, zoning completion waited on `settle_frame_.ui` and `settle_frame_.scene`. While backgrounded with `draw == false`, headless clients could stall finishing zone settles.
3. **Swapchain & Per-Frame GPU Allocation Retention**: Background processes retained swapchains and transient render targets active (`vk_context.cpp`), wasting VRAM and driver command-queue tracking across 24 processes.
4. **Windows-Only `gpu_ipc` / `gpu_external_memory`**: Cross-process GPU buffer sharing existed for Windows (`OPAQUE_WIN32` / named pipes), leaving Linux (`AF_UNIX` + `SCM_RIGHTS` + `VK_KHR_external_memory_fd`) unimplemented.

---

## Evaluated Multibox Architectures

### Option A: 24 Independent Processes (Naive `background_fps = -1`)
- **Pros**: Zero risk of cross-account Lua/singleton state corruption; instant fault isolation (1 crash != raid wipe); trivial upstream rebasing.
- **Cons (unmodified)**: 24 full ImGui/particle loops saturate CPU; stuck zone settles; ~24× redundant GPU allocation without swapchain suspension.

### Option B: Central Coordinator + Hybrid Process Isolation (**CHOSEN**)
- **Design**:
  - **24 isolated `hailstone` OS processes**, managed by a lightweight local coordinator (`hailstone24_broker`) and interactive switcher (`hailstone24_manager`).
  - Each process executes an explicit **`ExecutionState`** machine (`Disconnected`, `BackgroundHeadless`, `Transitioning`, `ForegroundFull`) wired cleanly into `ClientApp`.
  - In `BackgroundHeadless`: window hidden, swapchain/transient scene passes suspended (`vk_.suspend_for_background()`), ImGui/Lua UI builds skipped, particle/audio updates skipped, while `GameConnection` networking, `StepClock` physics, `Detour` navigation, `Automation` / `RG Mercs` Lua coroutines, and `SessionBusClient` IPC execute at decoupled `bg_tick_hz` (default **25 Hz**, range 10–60 Hz).
  - Read-only `PFS`/zone assets are already deduplicated at page-cache level by Linux OS `mmap` (`shared_file_cache`), and Linux `VK_KHR_external_memory_fd` + `AF_UNIX`/`SCM_RIGHTS` enables `shared-broker` VRAM deduplication when paired with `hailstone_gpu_cache_broker`.
- **VRAM & RAM Estimate on Intel Arc Pro B70 + 48 GB RAM**:
  - `1 × ForegroundFull`: ~1.5–2.2 GiB RAM, ~1.2–2.5 GiB VRAM.
  - `23 × BackgroundHeadless (cached mode)`: ~450–750 MiB PSS each (~12–17 GiB total RAM), ~150–350 MiB minimal driver/geometry buffers each (~5–8 GiB VRAM) -> **Total ~16–20 GiB RAM (well under 48 GiB)** and **~7–11 GiB VRAM (well under 32 GiB)**, enabling **`< 250 ms` foreground switches**.
  - `deferred` mode drops background scene renderers further at the expense of slightly longer zone rebuild on foreground switch; `shared-broker` shares GPU asset memory via `OPAQUE_FD`.

### Option C: Single-Process 24-Client Monolith
- **Pros**: Theoretical lowest base overhead (~300 MiB saved from 1 binary image + 1 Vulkan device).
- **Cons (Disqualifying)**: Hailstone uses process-wide static registries (`mq::Types g`, Vulkan global volk dispatch table, ImGui single context, crash/plugin globals, Lua UI host). Merging 24 `ClientApp`s into one address space requires invasive global refactoring, breaks upstream patchability, exposes all 24 accounts to single-pointer Lua/plugin faults, and provides marginal RAM savings on a 48 GiB / 32 GiB VRAM workstation.

---

## Decision
**Adopted Option B (Hybrid Multi-Process with Explicit `ExecutionState` Headless Mode, Local `AF_UNIX` IPC Broker, and Linux `VK_KHR_external_memory_fd` Support).**
It achieves near-zero GPU load for 23 background clients, deterministic decoupled simulation (`25 Hz`), `< 250 ms` foreground switching, full process fault isolation, clean upstream `git diff` patchability, and seamless `RG Mercs` + `actors` IPC compatibility.
