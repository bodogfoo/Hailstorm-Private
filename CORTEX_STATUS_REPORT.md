HAILSTONE CORTEX — STATUS REPORT

Audit date: 2026-11-21 (America/Toronto). Auditor: orchestrator session, read-only
except for the one committed change described in §8.
Repository: /home/john/src/hailstone-native

Overall completion: 21%
Implementation completion: 35%
Testing completion: 21%
Production readiness: 0%

How those figures are computed (nothing here is a guess, and nothing is rounded up):

- Overall 21% = the seven pipeline stages the assignment names, equally weighted,
  scored by how far each is actually built end to end:
  character game state 100%, agent observation 50%, shared local miniature model 0%,
  independent decision 0%, persistent memory 0%, Commander visibility 0%,
  validated RGMercs intention 0% → (1.0 + 0.5) / 7 = 21.4%.
- Implementation 35% = the 23 components of §2, scored COMPLETE 1.0 / PARTIAL 0.5 /
  NOT STARTED 0.0: 3 complete and 10 partial of 23 → 8.0 / 23 = 34.8%. The partial
  scores are all *substrate* (session bus, group profiles, combat controller,
  RGMercs bridge), not agent functionality.
- Testing 21% = the same seven stages scored by automated coverage of the behaviour
  that exists today: game state 100%, observation 50% (8 new cases), the rest 0%.
  No test anywhere exercises an agent, because there is no agent.
- Production readiness 0% = no client code path reaches the observation layer and
  no model, memory or mode exists, so there is nothing Cortex-shaped to deploy. The
  *client* remains usable; that is a different product.

Current phase: audit complete; pipeline stage 2 (agent observation) started, stages
3-7 unbuilt. Autonomous live actions stay disabled.
Current Git commit: cc805811b1983c81fe482617c3a923495ea64d65 on
codex/hailstone-unified-review (parent ef765c228016305dd833dd2d59275d2d975544ad,
the dispatch point). Draft PR #1 into main, unchanged by this work.
Working deployment: /home/john/src/hailstone-native/build/hailstone/ (client,
broker, manager), rebuilt from this source at 20:37 EST.
Backup location: /home/john/backups/hailstone-cortex-20261121/

--------------------------------------------------------------------------------
§1 SOURCE SYNCHRONIZATION

Repository, branch, commit
- /home/john/src/hailstone-native, branch codex/hailstone-unified-review.
- HEAD at dispatch ef765c228016305dd833dd2d59275d2d975544ad, working tree clean.
- HEAD now cc805811b1983c81fe482617c3a923495ea64d65 (one commit, §8), tree clean.
- main untouched: local main 16d4a24d, origin/main 809b9ef8; no push, no force-push,
  no merge performed by this work.

Uncommitted changes
- None at dispatch; none now (git status --porcelain is empty).

The two named archives — what they actually are
- /home/john/.hermes/attachments/web-11fb7581ac1db7bc-hailstone-native-latest-2026-11-21.zip
- /home/john/.hermes/attachments/web-d580acbd58810c02-hailstone-native-latest-2026-11-21.zip
  SHA-256 1a94e7ae39daa0d84e01e6684ae720d76420260b2f486daacda885a86ac18e06, 9,456,475 bytes
  — identical for both, and identical to /home/john/Downloads/hailstone-native-latest-2026-11-21.zip
  and to the two copies under ~/.hermes/uploads/ and ~/Documents/Codex/.
  Verified by sha256sum on all five copies; `diff -rq` of the two extracted trees reports
  no differences.
- No file named "hailstone-native-latest-2026-11-21(1).zip" exists anywhere on this
  machine (searched /home/john, /tmp, /var/tmp, the trash, /mnt, /media to depth 6). The
  "(1)" copy is the browser's duplicate of the same download: both named archives are the
  same bytes, so there are not two different archives to reconcile.
- What they contain: 255 entries (219 files, 36 directories), all under scripts/ —
  scripts/rgmercs/** plus scripts/porter_go.lua. They are the **RGMercs runtime script
  snapshot**, not the C++ source tree.
- Reconciliation against the installed runtime (/home/john/.local/share/hailstone24/scripts),
  done with `diff -rq`: the only difference is scripts/rgmercs/.git/index (git bookkeeping);
  every script, config and test file is byte-identical, and the installed tree is a superset
  (it additionally holds scripts/class_configs/ and scripts/config/). So no customized
  RGMercs modification was lost or overwritten, and the archive adds nothing the install
  lacks.

The C++ source snapshot that came with them
- /home/john/Downloads/hailstone-native-latest-2026-11-21/ (no .git, no hailstone24/),
  2,364 files, dated 2026-11-20..21. This is the source-side snapshot the assignment calls
  the source archive; its content was compared programmatically with HEAD:
  - 71 files differ, 24 paths exist only in HEAD, **0 paths exist only in the snapshot**.
  - Of the 71 differing files, 68 have snapshot content that is *exactly* the blob of a
    commit that is an ancestor of HEAD (checked per file with `git hash-object` +
    `git log --find-object`), i.e. the repository has simply moved past them.
  - 3 files carry content that was never committed: HERMES_HANDOFF.md (the repository holds
    a longer, newer version), src/app/client_app_login.cpp and ui/modern/combat.lua. Their
    diffs were read in full: HEAD adds class-name handling, the two-step force-stop with
    process_identity verification, and the roster-height parameter. HEAD is strictly newer
    in all three; nothing from the snapshot is missing.
  Conclusion: the working project is a strict superset of both the source snapshot and the
  script archives. No customization was overwritten and no newer improvement was lost.

Backup and rollback
- /home/john/backups/hailstone-cortex-20261121/
  - codex-hailstone-unified-review-ef765c22.bundle (23,039,078 B) — a git bundle of the
    whole branch at the dispatch commit; `git bundle verify` reports the ref and
    "records a complete history".
  - rgmercs-installed-scripts-20261121.tar.gz (10,071,742 B) — the installed RGMercs
    scripts plus porter_go.lua as they stand (329 entries).
  - SHA256SUMS for both.
  Rollback: `git clone /home/john/backups/hailstone-cortex-20261121/codex-hailstone-unified-review-ef765c22.bundle`
  (or `git fetch` from it) and check out the branch; restore scripts with
  `tar xzf rgmercs-installed-scripts-20261121.tar.gz -C /home/john/.local/share/hailstone24`.
  Code rollback inside the working repository is `git reset --keep ef765c22` (keeps
  the tree's uncommitted state out of the way) or `git checkout ef765c22 -- .`.

Do the deployed binaries match the current source?
- Yes, as of 20:37 EST: `cmake --build build -j8` relinked all 28 targets with exit 0 and
  no warnings, and no file under src/, ui/, tests/ or CMakeLists.txt is newer than
  build/hailstone/hailstone (checked with `find … -newer`). The desktop launcher
  (/home/john/Desktop/Hailstone Native - EZ Server.desktop →
  ~/Desktop/scripts/eq/launch-hailstone-native.sh) runs exactly this build tree.
- No installed/duplicate copy of the binaries exists elsewhere (nothing in ~/.local/bin,
  /opt or /usr/local/bin).

--------------------------------------------------------------------------------
§2 CORTEX IMPLEMENTATION STATUS

Finding first, because it governs the rest: **before this work the word "Cortex" did not
occur anywhere in this repository.** `git grep -i cortex` on every other branch tip
(local and remote, including main), `git log --all --grep=cortex` and every file name in
history return nothing: no branch, no file, no directory, no test. The only occurrences
today are the eight files this audit touched (two sources, one test, three docs, the
CMake list and the wiki page list). There is no agent runtime, no model client, no memory
store, no objective parser, no mode switch and no Cortex UI. What exists is the substrate
an agent layer would sit on.

Statuses: COMPLETE = built and tested (operational noted separately); PARTIAL = a real
substrate exists but not the Cortex component; NOT STARTED = nothing.

Component | Status | Evidence | Outstanding
1 Structured game-state observations | PARTIAL | `src/ipc/session_bus.h:58` SessionSnapshot (identity, zone, target, HP/MP/EN %, group leader, combat state, script state, RSS/VRAM); published at 4 Hz by `src/app/client_app.cpp:1330 publish_bus_telemetry()` (throttle at `:1333`); `src/cortex/observation.{h,cpp}` (new) turns it into a per-character record. Tested: 8 cases. | No spawns, buffs, gems, inventory, positions or quest state in the snapshot; those are needed before a decision can be tactical.
2 Native C++ agent runtime | NOT STARTED | No scheduler, tick loop, agent class or context object in `src/`. | The runtime itself.
3 RGMercs user-module integration | COMPLETE | `src/mq/rgmercs_bridge.h` (typed settings, modules, bounded logs, SQLite import), `src/mq/lua_hailstone.cpp` (`rgmercs.*`), `ui/modern/rgmercs.lua`; tests `tests/hailstone24/rgmercs_trusted_loader_test.cpp`, `rgmercs_native_ui_test.cpp`. Operational: used live by this installation (scripts installed separately at ~/.local/share/hailstone24/scripts/rgmercs). | Nothing for Cortex; it is the interface an intention must end in.
4 Shared miniature LLM inference | NOT STARTED | No endpoint, client, prompt builder or token accounting in `src/`. The only local endpoint (`:8096`, Qwen3-4B) belongs to Hindsight, not this project, and is saturated (§3). | Model choice, a dedicated endpoint or in-process backend, timeouts, and a fallback that returns control to RGMercs.
5 Individual character identities | PARTIAL | Per-process identity published (`session_id`, character, server, class, level); one process per character. | An agent identity distinct from the game name, surviving zone/relog.
6 Independent character contexts | PARTIAL | Process isolation plus per-character data folders; `cortex::observe()` builds one observation per character. | A per-agent context (observation + history + memory handle) with no shared mutable state.
7 Persistent character memory | NOT STARTED | Per-character settings/UI/autorun persistence exists; agent memory does not. | A store keyed by character identity, retention rules, and a decision on what is remembered.
8 Event-driven inference scheduling | NOT STARTED | No trigger, queue or budget code. | Which events wake an agent, coalescing, and a global budget.
9 Inter-character communication | COMPLETE (transport) | `MsgType::ActorSend`/`ActorPacket` mailbox routing (`src/ipc/session_bus.h:90`), command scopes `/bca`, `/bcaa`, `/rc`, telemetry roster; test `tests/hailstone24/session_bus_test.cpp`. | Agent-to-agent semantics (ask for help, claim a target, report an outcome) and guarantees above a mailbox.
10 Group-level intelligence | PARTIAL | Group profiles with named leader and leader-follow defaults (`src/core/group_profile.*`, `tests/hailstone24/group_profile_test.cpp`), protocol group state (`src/game/group.h:30`), combat controller. Scripted policy only. | Any group reasoning: role assignment, target arbitration, coordinated assist/heal driven by an agent.
11 Raid-level intelligence | PARTIAL | Raid state parsed (`src/game/social.h:216 RaidMember`, `:230 RaidState`), raid leadership rules (`src/game/group_rules.*`), raid windows in both UI packs. | Raid-scale observation (to 72 members) and any raid decision-making.
12 Tactical action validation | PARTIAL | The client validates its own commands (target existence, nav reachability, melee range, combat preconditions in `src/combat/`). | A validator for *agent intentions*, with an explicit accept/refuse list.
13 Action ownership and arbitration | PARTIAL | Manual movement takes the decision from the controller for a cooldown (`manual_override_active`/`_remaining_ms`, `src/combat/combat_types.h:398`, `src/combat/movement_controller.cpp`); `/estop` latches the engine. No agent/RGMercs arbitration. | An owner per action class with a stated precedence order and a test per pair.
14 Natural-language objectives | NOT STARTED | Slash commands are the only input. | Objective intake, decomposition, plan representation.
15 Commander Cortex tab | NOT STARTED | The Commander window has four tabs — Players / Clients, Overview & Rules, Spell Loadout, Transitions & Log (`ui/modern/combat.lua:708`). | The tab: per-agent state, last decision, cost, mode, kill switch.
16 Manual override | PARTIAL | Human commands and native controls work; the controller yields to manual movement for a cooldown. Nothing exists to override. | Explicit, visible, tested override against a live agent.
17 Global emergency stop | PARTIAL | `/estop` latches EmergencyStop per client (`src/app/client_app_commands.cpp:699`, `:944`), exposed in the Combat window (`ui/modern/combat.lua:606`), and the bus can dispatch to every session (`CommandScope::AllIncludingSelf`); combat-engine tests pass. | One fleet-wide stop that also halts agents, fails closed when the bus is down, and is tested as a fleet action.
18 Observation-only mode | NOT STARTED | — | Mode that runs observation and logs it, proposing nothing.
19 Assisted mode | NOT STARTED | — | Proposals in the Commander window, applied only on human approval.
20 Autonomous mode | NOT STARTED | — | Gated on arbitration, validation and safety tests.
21 Headless background client compatibility | COMPLETE | `ExecutionState::BackgroundHeadless` (`src/app/execution_state.h`), entered by `ClientApp::enter_background_headless()` (`src/app/client_app.cpp:1303`); telemetry keeps publishing while headless; test `tests/hailstone24/execution_state_test.cpp`. Operational in this installation's previous live runs. | An agent must not assume a rendered frame; nothing else.
22 Support for 24 simultaneous agents | PARTIAL | The bus is sized for 24 (peer table, roster, bounded 1 MiB frames) and a harness exists (`hailstone24/scripts/bench_24box.sh`); no 24-client run is recorded in this repository, and 24 *agents* cannot exist yet. | A recorded 1/6/12/24 run with latency, RSS and VRAM per client.
23 Autonomous quest planning | NOT STARTED | Quest state is read (`src/game/tasks.*`, quest journal in both packs). | Any planning.

Implemented / tested / operational, kept apart
- Implemented and tested: 3 (RGMercs integration), 9 (comms transport), 21 (headless
  state machine), plus the new observation layer (1).
- Operational in this installation (evidence: installed scripts, prior live handoffs):
  RGMercs integration and headless multiboxing. Not re-verified live by this audit — no
  client was launched.
- Operational for Cortex: nothing.

--------------------------------------------------------------------------------
§3 LOCAL LLM STATUS

Miniature models present on the machine (GGUF, by size)
- 2.49 GB — Qwen3-4B-Q4_K_M.gguf (/media/john/kubuntu_2510/AI-Models/gguf/Qwen3-4B-GGUF/),
  4,022,468,096 parameters, Q4_K_M, n_ctx_train 40,960.
- 8.13 GB — Gemma-4-E4B-Uncensored Q8_K_P (3 copies on different volumes).
- 10.36 GB — DeepSeek-Coder-V2-Lite-Instruct Q4_K_M.
- 14.4 GB — LFM2-24B-A2B Q4_K_M.
- 17-22 GB range — Gemma-4-26B-A4B, Qwen3.8-27B (4 variants incl. heretic), Qwen3.6-35B-A3B
  (4 variants), Qwen3.5-35B-A3B Q3_K_M.
- 54.7 GB — Qwopus3.8-27B-Flash-MTP BF16 (two copies).
No model has been downloaded, converted or quantised *for Cortex*; none is wired to this
project.

Inference engine and endpoint actually running
- llama.cpp (`/home/john/llama-build-native/bin/llama-server`, pid 1520426), listening on
  127.0.0.1:8096, started 2026-11-19 21:38, up 1 d 23 h.
- Served model: alias `qwen3-4b`, Q4_K - Medium, n_ctx 32768, total_slots 1
  (from GET /props and GET /v1/models; /health returns {"status":"ok"}).
- CPU or GPU: **CPU**. The command line carries no `-ngl`, and the process holds no
  /dev/dri file descriptor (checked /proc/1520426/fd). A 2026-11-19 audit of this host
  recorded partial offload on this port, so the observed state is CPU-bound regardless.
- Resource use, measured: 3,735,340 KiB RSS (≈3.56 GiB), 84% CPU sustained, 23 GiB of
  46 GiB system RAM available, no VRAM attributable to it.
- This server is Hindsight's backend (Hermes memory), not a Cortex endpoint. There is no
  Cortex endpoint, port or supervisor unit.

Measured inference speed for Cortex: **none — outstanding work, not estimated.**
What was measured is that the Hindsight endpoint could not serve a Cortex-style request at
all while it is busy: a 24-token chat completion returned nothing within 120 s, and an
8-token completion returned HTTP 000 after a 20 s timeout, twice. Any latency, prompt-
processing, decision-latency, structured-output or concurrency number for a Cortex model
would therefore be invented, and none is given.

Existing installations that must be preserved
- Lima (`:8095`, vLLM-XPU for the B70): `lima-vllm.service` is present, **disabled and
  inactive**; the Docker container `b70-heretic-vllm` does not exist; port 8095 is not
  listening; the model directory on /media/john/Playbook (mounted) is present. So Lima is
  installed but not running, and "operational" cannot be asserted without starting it —
  which this audit did not do.
- XE Strata (xestrata, `:8099`): present at /home/john/xestrata (source, build-xe, venv,
  configs); **not running**; port 8099 free. Unchanged since the 2026-11-19 audit that
  recorded the same state.
- Neither installation was started, stopped, reconfigured or written to by this work. No
  GPU driver, model installation or production deployment was touched.

--------------------------------------------------------------------------------
§4 AUTOMATED TESTING

Build (Release, Ninja, tests on, GCC 13.3.0, tests linked but no client started)
- `cmake --build build -j8` → 28/28 targets, exit 0, **no warnings or errors** in the log
  (scratch/cortex-build2.log). Build 1 (before the change) was also exit 0.

Whole suite, run with a short TMPDIR (`TMPDIR=/tmp/hst`):
- Baseline at dispatch commit ef765c22: 4,001 cases | 3,999 passed | **2 failed** | 74 skipped;
  1,729,603 assertions | 1,729,601 passed | 2 failed. The two failures are
  tests/hailstone24/session_bus_test.cpp:34 and :136, `REQUIRE( srv.start(sock) )` — the bus
  socket path exceeded the 108-byte sun_path limit because TMPDIR was long. Environmental,
  reproducible, and they pass with `TMPDIR=/tmp/hst` (2/2).
- Final at cc805811: **4,009 cases | 4,009 passed | 0 failed | 74 skipped; 1,729,737
  assertions | 1,729,737 passed | 0 failed; Status: SUCCESS; exit 0.**
  Delta: +8 cases, +134 assertions, all new; the 2 baseline failures are gone because the
  rerun used the short TMPDIR.
- New regressions: none. Existing baseline failures: 2, both environmental (above), both
  passing under the documented TMPDIR.
- The 74 skips are the suite's own guards (`--skip`/`gt` and unset EQ_GAME_DIR /
  HAILSTONE_VECTORS_DIR). No game folder or recorded vectors were used, so asset and
  recorded-vector cases prove nothing here; say so rather than counting them as green.
- CI checks: `python3 tools/check_core_names.py` exit 0; `python3 tools/sync_wiki.py --check`
  exit 0 (the new page is registered). The UI-api checks were not run: no UI contract file
  changed.

The nine specific verifications the assignment asks for
- RGMercs remains operational — **code and tests yes; live no.** The installed scripts are
  byte-identical to the archive (diff), the bridge and trusted-loader/native-UI tests pass,
  but no client was launched, so "operational" rests on the file state and earlier live
  handoffs, not on a run today.
- Character configurations intact — 47 per-character hailstone.ini files present under
  /home/john/.local/share/hailstone24/servers/..., roster hailstone24_roster.ini unchanged
  since 2026-11-16 18:36, rgmercs_config.db last written 2026-11-21 12:34 (before this
  session). Nothing wrote to them.
- Commander functional — the Commander window, session bus and group-profile tests pass
  (bus: 2/2, profiles: 4/4 by name; whole suite green). No live window was opened.
- Foreground graphics functional — **not verified**: no client was launched (prohibited by
  the brief). The render target is unchanged by this work.
- Background headless execution functional — the state machine is unit-tested; the
  rendered-frame suspension path is not exercised by tests and no live run was done.
- Inference does not block game simulation — **not applicable**: the client contains no
  inference. No test can exist for it yet.
- Manual controls override Cortex — **not applicable**: no Cortex. Manual override of the
  combat controller (movement cooldown) is implemented and tested.
- Agent identity and memory isolation — **not applicable**: no agents, no memory.
- Inference failure returns control to RGMercs — **not applicable**: no inference.
- Cortex and RGMercs do not compete for character control — nothing competes *today*
  because there is no Cortex; the risk is unmanaged at the protocol level (no owner token),
  and it is the first thing §13/§16 has to settle.

--------------------------------------------------------------------------------
§5 LIVE READINESS (answers, not estimates)

Is Cortex built? NO — there is no Cortex binary, library entry point or wire protocol.
Is Cortex deployed? NO.
Is there a desktop launcher? A launcher for the *client* exists
  (/home/john/Desktop/Hailstone Native - EZ Server.desktop → ~/Desktop/scripts/eq/
  launch-hailstone-native.sh → build/hailstone/hailstone, one standalone client). There is
  no Cortex launcher.
Is the inference server running? Not for Cortex. Hindsight's Qwen3-4B on :8096 is up but
  CPU-saturated and did not answer 8-token and 24-token probes. :8095 (Lima) is down.
Is the Cortex tab visible in Commander? NO — the window has four tabs, none of them Cortex.
Can one character think independently? NO — no model, no decision loop.
Can six characters think independently? NO.
Can all 24 characters think independently? NO.
Can characters communicate with one another? YES at the transport level: actor mailboxes and
  scoped commands over the session bus (tested). There is no agent protocol above them.
Is character memory persistent across restarts? Character *settings* are. Agent memory does
  not exist.
Can I issue natural-language objectives? NO — slash commands only.
Can agents execute tactical actions? NO. The client can execute validated commands when a
  human or RGMercs issues them.
What is actually operational today: the Hailstone client itself (login, zones, graphics,
  groups/raids, combat controller, RGMercs running as an add-on, headless multiboxing over
  the session bus, Commander window, /estop). That is the substrate. None of it is Cortex.

Can the user test it now: **NO** — there is nothing Cortex-shaped to launch. What can be
tested is the substrate and the new observation layer:
  1. Substrate, read-only: /home/john/src/hailstone-native/build/hailstone/hailstone24_manager
     --socket /run/user/1000/hailstone24/bus.sock --data-dir /home/john/.local/share/hailstone24 --status
  2. Observation layer: `cd /home/john/src/hailstone-native && TMPDIR=/tmp/hst
     ./build/hailstone_tests -tc="cortex*"` → 8 cases, 73 assertions, all pass.
  3. Full suite: `TMPDIR=/tmp/hst ./build/hailstone_tests` → 4,009/4,009, exit 0.
  Launching the client is safe and unchanged, but it demonstrates the client, not Cortex.

--------------------------------------------------------------------------------
§6 PERFORMANCE ASSESSMENT

1 / 6 / 12 / 24 agent benchmarks: **not run, not runnable** — there are no agents. Reporting
latency or throughput here would be fabrication. What exists to measure is the substrate,
and these numbers are real:
- Client telemetry cadence: 250 ms (code, `client_app.cpp:1333`); no measured delivery
  latency for a 24-client roster in this audit.
- The one inference endpoint: 3.56 GiB RSS, 84% CPU, 1 concurrent slot, and it did not
  answer a 8-token request in 20 s or a 24-token request in 120 s while busy (§3).
- RAM headroom on the host: 46 GiB total, 23 GiB available; swap 10 GiB used of 71 GiB.
- GPU: Intel Arc Pro B70 (PCI 0b:00.0, device e223). No VRAM counters are exposed by the
  `xe` driver on this kernel; per-client VRAM is whatever the client reports in its own
  telemetry (`vram_mib`), which is an estimate, not driver telemetry.

Bottlenecks, in the order they will bite
1. No dedicated inference capacity. A single CPU slot shared with Hindsight cannot serve one
   agent, let alone 24. Before any agent work: a dedicated endpoint (the idle B70 via
   Vulkan/SYCL, or a small CPU model on its own core budget) with a measured p50/p95.
2. Single-slot concurrency. 24 agents at even one decision per 10 s is 2.4 req/s; a
   `--parallel 1` CPU server will serialise and then time out.
3. Observation breadth. Deciding well needs spawns/buffs/gems/positions; the bus carries
   only aggregates today.
4. No arbitration primitive. Without an action owner, two agents (or an agent and RGMercs)
   will fight over movement, as the native combat controller and RGMercs already can.
Concrete recommendations: put the model behind one supervisor with a fixed token budget and
a hard timeout; make the observation a delta (send only changes, with a full snapshot on
demand) so 24 agents do not re-send 24 copies of the roster; measure per-client frame time
with 6 and 24 headless clients *before* adding inference, so the inference cost is
attributable; and gate autonomous mode on an arbitration test that fails if two owners act
inside one tick.

--------------------------------------------------------------------------------
§7 VERIFIED WORKING FUNCTIONALITY / REMAINING / RISKS

Verified working (client substrate, evidence = passing tests + installed state + prior live
handoffs; not re-run live today)
- Session bus: roster, /bca dispatch, actor mailboxes, foreground switch, ACK/timeout
  tracking (tests 2/2).
- Group profiles: load/save INI and JSON, leader-follow defaults, secrets refused (tests).
- Combat controller: melee hold, movement modes, manual-override cooldown, /estop latch.
- RGMercs: trusted loader, capability ceiling, native UI, typed settings (tests); scripts
  installed and byte-identical to the archive.
- Headless/foreground execution state machine.
- The new observation layer: 8 cases.

Remaining functionality: everything from stage 3 onward — model wiring, prompts, decision
loop, memory, scheduling, arbitration, validation of intentions, objective parsing, the
Commander tab, the three modes, and any 24-agent run.

Critical blockers
1. No model endpoint that can serve a request (measured: timeouts, §3), and no budget to
   create one without competing with the user's existing inference.
2. No arbitration primitive between an agent and RGMercs — the one thing that makes
   autonomy unsafe even once a model works.
3. Observation is too thin for tactical decisions (no spawns, buffs, gems, inventory,
   positions).
4. No persistent memory design (what to store, where, retention) has been decided.

Performance results: none for Cortex (§6). Known bugs: none found in the substrate during
this audit; the only test anomalies were the environmental bus-socket failures, now
characterised. Regression risks: adding an agent that issues commands could race the combat
controller and RGMercs (§13/§17); the bus is a single-socket broker, so an agent that
blocks on it would stall a client's telemetry; and any per-agent model call on the :8096
endpoint would slow Hindsight, whose memory work is already saturating it.

--------------------------------------------------------------------------------
§8 WHAT WAS CHANGED, AND WHAT WAS NOT

Commit cc805811b1983c81fe482617c3a923495ea64d65 "cortex: add the agent observation layer
with tests" — on codex/hailstone-unified-review only, not pushed.
- new src/cortex/observation.h, src/cortex/observation.cpp — `observe()`, `to_json()`,
  `to_prompt_context()`; read-only, no command, no I/O, no clock of its own.
- new tests/cortex_observation_test.cpp — 8 cases, 73 assertions.
- new docs/cortex-status.md — the durable status document (component table with evidence).
- CMakeLists.txt — engine source + test list (2 lines).
- docs/todo.md — a "Hailstone Cortex (agent layer)" section: what is done, what is open.
- tools/sync_wiki.py — PAGES entry for the new document.

Deliberately not done: no client code calls the observation layer yet (so no live behaviour
change and no way for it to act); no merge, no push, no PR edit; main untouched; nothing
started, stopped or reconfigured; no client, broker or model launched; no credentials,
accounts, runtime logs or game data written into the tree (checked: the only files added
are source, tests and docs).

--------------------------------------------------------------------------------
§9 NEXT FIVE DEVELOPMENT PRIORITIES

1. Decide and stand up one inference endpoint for the box, isolated from Hindsight, and
   record measured p50/p95 latency and tokens/s for a 200-token prompt and a 32-token
   decision — on the idle B70 if its driver path is healthy, else a small CPU model with a
   dedicated core budget. Nothing else can be measured until this exists.
2. Widen the observation: spawns (id, name, level, distance, target), buffs, spell gems,
   inventory summary, and position — as a delta on the bus so 24 clients do not re-send a
   full roster each tick. Extend tests/cortex_observation_test.cpp per field.
3. Write the arbitration contract before any agent can act: one owner per action class
   (movement, targeting, casting, looting), a stated precedence (human > agent > RGMercs >
   native controller, or the reverse where justified), a token that carries the owner, and
   a test for each pair. Fail closed: if the owner is ambiguous, nobody acts.
4. Build the intention path end to end behind observation-only mode: model proposes a
   structured intention, the validator accepts or refuses it with a reason, the Commander
   window shows it, and nothing executes. Then assisted mode (approve to apply), then
   autonomous, each gated on its own regression tests.
5. Persistent memory with an explicit design: keyed by character identity, event/outcome
   records with retention, a size cap, and a test that two characters' memories cannot
   see each other — before the first autonomous run.

Suggested acceptance criteria for the advising AI to arbitrate: an agent may take a
character's movement only through the arbitration token; a model timeout must leave RGMercs
running and in control; and no autonomous path may exist without a passing fleet-wide stop
test.

--------------------------------------------------------------------------------
Method and limits of this audit

Read: git history of every branch for the named feature; both archives extracted and
diffed byte-for-byte; the installed runtime diffed against the archive; source read for
each component (session bus, RGMercs bridge, group profiles, combat controller, execution
state, Commander Lua, commands); tests built and run in full; CI checks run; process,
socket, filesystem and model state inspected with ps/ss/proc/docker/systemctl.

Not done, by design: no client, broker, manager, game window, model server, Lima or Xe
Strata start/stop; no GPU driver, model or deployment change; no destructive operation; no
push, merge or PR edit. Consequently "operational" claims above rest on file state, unit
tests and earlier live handoffs, never on a session run today — and every place that
matters says so.
