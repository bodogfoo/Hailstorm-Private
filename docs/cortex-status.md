# Hailstone Cortex: status

Hailstone Cortex is the agent layer planned on top of Hailstone 24: one agent per boxed
character, each observing its own game state, deciding through a shared local miniature
model, remembering what it learned, and acting only through intentions that
RGMercs and the client's own validators accept.

The pipeline it has to satisfy is:

```
character game state -> agent observation -> shared local miniature model
    -> independent character decision -> persistent memory
    -> Commander visibility -> validated RGMercs intention
```

Status as written: **2026-11-21**, branch `codex/hailstone-unified-review`,
commit `ef765c22`; updated on branch `feature/hailstone-cortex` with the shared
supervisor, the telemetry runtime, the application-owned service and the Commander
status surface below. This document is the durable record; the
audit that produced it is in `CORTEX_STATUS_REPORT.md` at the repository root.

## What exists, and what does not

The client already has the *substrate* an agent layer needs: every client publishes a
structured snapshot of itself, has a per-character identity, runs headless in the
background, and can receive validated commands over the local session bus. It has **no
agent runtime, no model client, no memory store, no objective parser, no mode switch and
no Cortex UI**. Nothing in this repository mentioned "Cortex" before this document: no
source file, no branch, no commit.

| Component | Status | Evidence | Outstanding |
|---|---|---|---|
| Structured game-state observations | PARTIAL | `src/ipc/session_bus.h:58` `SessionSnapshot` (identity, zone, target, HP/mana/endurance %, group leader, combat state, script state, RSS/VRAM); published every 250 ms by `src/app/client_app.cpp:1330` `publish_bus_telemetry()`. The observation record itself is `src/cortex/observation.h`, added with this document. | Observation is built from bus telemetry only; no spawn list, inventory, buffs, spell gems or positions. Needs a wider snapshot before decisions can be tactical. |
| Native C++ agent runtime | PARTIAL | One shared `CortexSupervisor` (`src/cortex/supervisor.{h,cpp}`): a worker pool over an inference queue with per-character cooldown, priority, backpressure, a circuit breaker, per-agent enable and a mode switch. `src/cortex/runtime.{h,cpp}` drives it from the session bus. Now owned by the application: `src/cortex/service.{h,cpp}` `CortexService` is created in the `ClientApp` constructor, started in Observe mode, ticked from `publish_bus_telemetry()` (`src/app/client_app.cpp:1330`) on the cached roster, and read by `commander_status()` (`src/app/client_app_commands.cpp`). Tests: `tests/cortex_supervisor_test.cpp`, `tests/cortex_runtime_test.cpp`, `tests/cortex_commander_view_test.cpp`. | No per-agent task cancellation when a character leaves the roster; the service is created with the default HTTP provider, so no endpoint/model is configured yet. |
| RGMercs user-module integration | COMPLETE | `src/mq/rgmercs_bridge.h` (typed settings, module toggles, bounded logs, SQLite/Lua import, adapter queue), `src/mq/lua_hailstone.cpp` (`rgmercs.*` bindings), `ui/modern/rgmercs.lua`. Tests: `tests/hailstone24/rgmercs_trusted_loader_test.cpp`, `tests/hailstone24/rgmercs_native_ui_test.cpp`. Runtime scripts are separate from this repository (`~/.local/share/hailstone24/scripts/rgmercs/`). | Nothing for Cortex; this is the interface an intention has to end in. |
| Shared miniature model inference | PARTIAL | `src/cortex/inference_provider.{h,cpp}`: the `InferenceProvider` interface, an HTTP JSON client with timeouts and a mock used by tests. The supervisor runs its queue against any provider. | No default endpoint in the client, no prompt builder beyond the supervisor's, no token accounting, no fallback path that returns control to RGMercs. |
| Individual character identities | PARTIAL | Per-process identity: `session_id`, character, server, class and level are published (`SessionSnapshot`); each client has its own data folder. No agent identity, persona or per-agent prompt state. | An agent identity that survives a zone or a relog, and is distinct from the game character's name. |
| Independent character contexts | PARTIAL | One client process per character (`hailstone24_broker` launch, `--session-id`), so state cannot leak between characters at the process level. `src/cortex/observation.h` now builds one observation per character. | A context object per agent (observation + recent decisions + memory handle) with no shared mutable state. |
| Persistent character memory | PARTIAL | `src/cortex/memory.{h,cpp}`: a store keyed by character identity that the supervisor writes decisions and events to (`add_decision`, `add_memory`, `load_profile`, `upsert_profile`), with a mock for tests. | Retention rules and a decision on what is remembered; nothing reads it back into a prompt yet. |
| Event-driven inference scheduling | PARTIAL | `CortexSupervisor::submit_observation()` wakes on significant events only (HP/mana thresholds, combat, target, zone, group state, inbox), deduplicates a queued character's observation, gives low HP/mana priority, and applies a per-character cooldown and a circuit breaker. | Which events wake an agent beyond the roster snapshot's fields, coalescing across a group, and a global token/cpu budget. |
| Inter-character communication | COMPLETE (transport) | `MsgType::ActorSend` + `ActorPacket` (`src/ipc/session_bus.h:90`, mailbox routing), command scopes `/bca`, `/bcaa`, `/rc` (`CommandScope`), telemetry roster. Test: `tests/hailstone24/session_bus_test.cpp`. | An agent-to-agent message *semantic* (ask for help, claim a target, report an outcome) and delivery guarantees above the mailbox. |
| Group-level intelligence | PARTIAL | Group profiles with named leader and leader-follow defaults (`src/core/group_profile.*`, `tests/hailstone24/group_profile_test.cpp`), group state from the protocol (`src/game/group.h:30`), combat controller. All of it is scripted policy, not reasoning. | Any group reasoning: role assignment, target arbitration, assist/heal coordination driven by an agent. |
| Raid-level intelligence | PARTIAL | Raid state is parsed (`src/game/social.h:216` `RaidMember`, `:230` `RaidState`), raid leadership rules in `src/game/group_rules.*`, raid window in both UI packs. | Raid-scale observation (up to 72 members) and any raid-level decision-making. |
| Tactical action validation | PARTIAL | The client validates its own commands (target existence, nav reachability, `move_melee` range, combat preconditions in `src/combat/`). | A validator for *agent intentions*: what an agent may propose, what is refused, and what is refused silently. |
| Action ownership and arbitration | PARTIAL | Manual input takes the movement decision away from the controller for a cooldown (`src/combat/movement_controller.cpp`, `manual_override_active`/`manual_override_remaining_ms` in `src/combat/combat_types.h:398`), and `/estop` latches the combat engine. No arbitration between an agent and RGMercs. | An explicit owner per action class (movement, targeting, casting, looting) with a stated precedence order and a test for each. |
| Natural-language objectives | NOT STARTED | Nothing parses an objective; `/` commands are the only input. | Objective intake, decomposition, and a plan representation that maps to valid intentions. |
| Commander Cortex tab | PARTIAL | The Combat/Commander window has four tabs: `Players / Clients`, `Overview & Rules`, `Spell Loadout`, `Transitions & Log` (`ui/modern/combat.lua:708`). The status data the tab needs is now exposed read-only: `hs.CommanderStatus().cortex` (`src/app/lua_ui_combat.cpp` `push_cortex_view`, fed from `UiHost::CommanderStatusSnapshot.cortex`). | The tab itself: rendering the per-agent state, last decision, token use, mode and kill switch, plus a confirm step for a proposal. |
| Manual override | PARTIAL | Human commands and native controls work; the combat controller yields to manual movement for a cooldown. There is nothing to override yet. | Manual override that is explicit, visible, and tested against a live agent. |
| Global emergency stop | PARTIAL | `/estop` latches `EmergencyStop` per client (`src/app/client_app_commands.cpp:699`, `:944`), the Combat window exposes it (`ui/modern/combat.lua:606`), and the bus can dispatch a command to every session (`CommandScope::AllIncludingSelf`). Tested at the combat-engine level. | One control that stops every agent *and* every character, fails closed if the bus is down, and is tested as a fleet action. |
| Observation-only mode | PARTIAL | `CortexRuntime::tick()` runs observe -> submit -> decision with the supervisor in `Observe` mode, where arbitration always refuses Cortex execution (`observation mode: proposals only`) and the runtime reports no proposal. The client starts its service in this mode. Tests: `tests/cortex_runtime_test.cpp` ("Observe mode ..."), `tests/cortex_commander_view_test.cpp` (the view reports `CORTEX_OBSERVE` and no proposal). | Nothing logs the observation/decision stream yet; the view shows state only. |
| Assisted mode | PARTIAL | The same tick in `Assisted` mode yields `cortex::AssistedProposal` records: the character, the allowlisted intention and action, the reason and the arbitration result (`assisted mode: awaiting confirmation`), read-only, `executed` always false. They now reach the UI as `hs.CommanderStatus().cortex.proposals[]` (and each waiting character as `characters[].proposalPending`), still read-only. Tests: `tests/cortex_runtime_test.cpp` ("Assisted mode ...", "Assisted proposals ..."), `tests/cortex_commander_view_test.cpp`. | A human confirmation step and the window that shows them; the runtime and the view never send a command. |
| Autonomous mode | NOT STARTED | Deliberately: `CortexRuntime` exposes no path to it, and `arbitrate()` is the only code that could ever authorize it. | Gated behind arbitration, validation and the safety tests above. |
| Headless background client compatibility | COMPLETE | `ExecutionState::BackgroundHeadless` (`src/app/execution_state.h`), entered by `ClientApp::enter_background_headless()` (`src/app/client_app.cpp:1303`), telemetry keeps publishing. Tests: `tests/hailstone24/execution_state_test.cpp`. | Nothing for Cortex; an agent must not assume a rendered frame. |
| 24 simultaneous clients | PARTIAL | The bus is written for 24 (peer table, roster, 250 ms telemetry, bounded 1 MiB frames) and a 24-box harness exists (`hailstone24/scripts/bench_24box.sh`), but no 24-agent run is recorded, and no 24-*agent* capacity exists because there are no agents. | A recorded 24-client run with per-client latency, RSS and VRAM. |
| Autonomous quest planning | NOT STARTED | — | Quest state is read (`src/game/tasks.*`, quest journal), but nothing plans. |

## The observation layer added with this document

`src/cortex/observation.{h,cpp}` is the first stage of the pipeline and the only part
implemented today:

- `cortex::observe(self, roster, now_ms)` copies what the character published about itself
  and selects the group from the bus roster: online peers on the same server that name the
  same leader, plus the leader, sorted by name.
- `cortex::to_json()` renders it as one line with a fixed key order.
- `cortex::to_prompt_context()` renders it as one line of text for a model prompt or a log.

It is read-only by construction: no command, no movement, no file write, no network, no
clock of its own. Tests: `tests/cortex_observation_test.cpp` (8 cases) covering the field
pass-through, follower/leader group selection, the no-leader case, roster-order
independence, JSON escaping and one-line output, and the prompt text.

What it is not: an agent. It has no model, no memory, no decision, and it cannot act.

## The supervisor and the telemetry runtime

Three more pieces exist now, all read-only towards the game:

- `src/cortex/supervisor.{h,cpp}` — one shared `CortexSupervisor` per process. It owns the
  agents (identity, mode, enabled flag, last observation, latest decision), runs a worker
  pool over an inference queue, and applies `set_agent_enabled()`, `set_mode()` and the
  emergency stop. `submit_observation()` queues inference only for an enabled agent and only
  on a significant event.
- `src/cortex/validator.{h,cpp}` — `validate_decision_json()` turns raw model output into a
  `Decision` (or a rejection reason), and `arbitrate()` decides, per mode, who owns an
  action: in Observe and Assisted mode it never authorizes Cortex execution.
- `src/cortex/runtime.{h,cpp}` — `cortex::CortexRuntime` is the telemetry adapter:
  `tick(roster, now)` (or `tick(SessionBusClient&, now)`, which snapshots `cached_roster()`)
  observes every online, named character with `cortex::observe()`, submits the observations
  to its supervisor, and returns a `RuntimeReport`: the observations, the supervisor
  snapshot, and the assisted proposals of this tick. A proposal carries the character, the
  allowlisted intention and action, the reason, the arbitration owner and reason, and
  `awaiting_confirmation = true`, `executed = false`.

Nothing in these files sends a command: there is no command path, and the only output a
caller can act on is the read-only proposal list. A proposal exists only when the agent is
enabled, observed on the current tick, in `Assisted` mode, outside the emergency stop, with
a valid decision whose action resolves to an allowlisted intention and which arbitration
refuses to run on Cortex's behalf.

Tests: `tests/cortex_supervisor_test.cpp` (8 cases) and `tests/cortex_runtime_test.cpp`
(4 cases: observation feeding and agent registration, Observe mode, the assisted proposal
gates including emergency stop and a roster departure, and a rejected non-allowlisted
action).

## The application-owned service and the Commander status path

Two more pieces wire that runtime into the client:

- `src/cortex/service.{h,cpp}` — `cortex::CortexService` is the process's owner of the shared
  supervisor and its `CortexRuntime`: created and started by the `ClientApp` constructor
  (`src/app/client_app_internal.h`), ticked from `publish_bus_telemetry()` on the roster the
  session bus caches, and stopped with the app. It starts in `Observe` mode. Its API is
  `start/stop/running/tick/view/supervisor` — there is no method that runs, confirms or
  dispatches anything, which the test states as a compile-time assertion.
- `src/cortex/commander_view.{h,cpp}` — `CortexStatusView` and `to_commander_view(report,
  running)` copy a `RuntimeReport` into display fields: the supervisor's health, mode and
  counters, the tick's roster numbers, one `CortexCharacterView` per agent and one
  `CortexProposalView` per proposal. A proposal carries the allowlisted action, the untrusted
  reason and explanation text, the arbitration owner and reason, `awaiting_confirmation` and
  `executed` — always false. There is no command field.

The client reads it in `ClientApp::commander_status()` and the Lua binding
`hs.CommanderStatus()` adds one table, `cortex`, beside the keys it already returned
(`available`, `busConnected`, `peers`, `trackedCommands`, `sessionId`, `characterName`,
`executionState`, `activeProfile`, ... — all unchanged, and present with the same shapes in
the unavailable branch):

```
cortex = { available, running, mode, emergencyStop, inferenceHealthy, modelName, endpoint,
           pendingQueue, totalRequests, totalDecisions, totalErrors, observedMs,
           rosterSize, onlineCount,
           characters[] = { key, serverName, characterName, brainStatus, mode, objective,
                            intention, decisionAction, lastError, enabled, hasDecision,
                            proposalPending, consecutiveErrors },
           proposals[]  = { key, characterName, action, reasonCode, explanation,
                            arbitrationOwner, arbitrationReason, awaitingConfirmation,
                            executed, decidedMs } }
```

Nothing on this path sends a command: the view is data, the service has no execution method,
and the Lua table has no field a script could hand to the game. A proposal leaves as display
data; a human confirming it is a later slice.

Tests: `tests/cortex_commander_view_test.cpp` (3 cases: the report-to-view mapping, the
service lifecycle and its gates including emergency stop, and the Lua table with the
pre-existing schema).

## Safety stance

- No autonomous live action is enabled. Nothing in `src/cortex/` can send a command.
- The observation layer needs no game running to be tested.
- `docs/todo.md` carries the open items; this document carries the status and the
  evidence.

## How to check this document

```sh
cmake --build build -j8                  # Release, Ninja
build/hailstone_tests -tc="cortex observation*"
build/hailstone_tests -tc="*cortex*,*Cortex*"
build/hailstone_tests -tc="*Commander*"  # the Commander status path (Cortex view + Lua table)
build/hailstone_tests                    # whole suite; TMPDIR must be short (docs/ci.md)
python3 tools/sync_wiki.py --check
```
