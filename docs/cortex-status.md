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
commit `ef765c22`. This document is the durable record; the audit that produced it is
in `CORTEX_STATUS_REPORT.md` at the repository root.

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
| Native C++ agent runtime | NOT STARTED | No scheduler, tick loop, or agent class in `src/`. | The whole runtime: per-agent context, decision cadence, cancellation, failure handling. |
| RGMercs user-module integration | COMPLETE | `src/mq/rgmercs_bridge.h` (typed settings, module toggles, bounded logs, SQLite/Lua import, adapter queue), `src/mq/lua_hailstone.cpp` (`rgmercs.*` bindings), `ui/modern/rgmercs.lua`. Tests: `tests/hailstone24/rgmercs_trusted_loader_test.cpp`, `tests/hailstone24/rgmercs_native_ui_test.cpp`. Runtime scripts are separate from this repository (`~/.local/share/hailstone24/scripts/rgmercs/`). | Nothing for Cortex; this is the interface an intention has to end in. |
| Shared miniature model inference | NOT STARTED | No model client, endpoint, prompt builder or token accounting in `src/`. The only local endpoint is `llama-server` pid 1520426 on `127.0.0.1:8096` (`qwen3-4b`, Q4_K_M, 32k ctx, 1 slot, CPU) — it serves the Hindsight memory system, not this project. | Model choice, a dedicated endpoint or in-process backend, request/response plumbing, timeouts, and a fallback path that returns control to RGMercs. |
| Individual character identities | PARTIAL | Per-process identity: `session_id`, character, server, class and level are published (`SessionSnapshot`); each client has its own data folder. No agent identity, persona or per-agent prompt state. | An agent identity that survives a zone or a relog, and is distinct from the game character's name. |
| Independent character contexts | PARTIAL | One client process per character (`hailstone24_broker` launch, `--session-id`), so state cannot leak between characters at the process level. `src/cortex/observation.h` now builds one observation per character. | A context object per agent (observation + recent decisions + memory handle) with no shared mutable state. |
| Persistent character memory | NOT STARTED | The client persists per-character settings, UI and autorun lists under the data folder, which is not agent memory. | A store keyed by character identity, with retention rules, and a decision on what is remembered (events, outcomes, lessons). |
| Event-driven inference scheduling | NOT STARTED | No trigger, queue or budget code. | Which events wake an agent (target change, HP threshold, group call, timer), coalescing, and a global token/cpu budget. |
| Inter-character communication | COMPLETE (transport) | `MsgType::ActorSend` + `ActorPacket` (`src/ipc/session_bus.h:90`, mailbox routing), command scopes `/bca`, `/bcaa`, `/rc` (`CommandScope`), telemetry roster. Test: `tests/hailstone24/session_bus_test.cpp`. | An agent-to-agent message *semantic* (ask for help, claim a target, report an outcome) and delivery guarantees above the mailbox. |
| Group-level intelligence | PARTIAL | Group profiles with named leader and leader-follow defaults (`src/core/group_profile.*`, `tests/hailstone24/group_profile_test.cpp`), group state from the protocol (`src/game/group.h:30`), combat controller. All of it is scripted policy, not reasoning. | Any group reasoning: role assignment, target arbitration, assist/heal coordination driven by an agent. |
| Raid-level intelligence | PARTIAL | Raid state is parsed (`src/game/social.h:216` `RaidMember`, `:230` `RaidState`), raid leadership rules in `src/game/group_rules.*`, raid window in both UI packs. | Raid-scale observation (up to 72 members) and any raid-level decision-making. |
| Tactical action validation | PARTIAL | The client validates its own commands (target existence, nav reachability, `move_melee` range, combat preconditions in `src/combat/`). | A validator for *agent intentions*: what an agent may propose, what is refused, and what is refused silently. |
| Action ownership and arbitration | PARTIAL | Manual input takes the movement decision away from the controller for a cooldown (`src/combat/movement_controller.cpp`, `manual_override_active`/`manual_override_remaining_ms` in `src/combat/combat_types.h:398`), and `/estop` latches the combat engine. No arbitration between an agent and RGMercs. | An explicit owner per action class (movement, targeting, casting, looting) with a stated precedence order and a test for each. |
| Natural-language objectives | NOT STARTED | Nothing parses an objective; `/` commands are the only input. | Objective intake, decomposition, and a plan representation that maps to valid intentions. |
| Commander Cortex tab | NOT STARTED | The Combat/Commander window has four tabs: `Players / Clients`, `Overview & Rules`, `Spell Loadout`, `Transitions & Log` (`ui/modern/combat.lua:708`). | The tab itself: per-agent state, last decision, token use, mode, kill switch. |
| Manual override | PARTIAL | Human commands and native controls work; the combat controller yields to manual movement for a cooldown. There is nothing to override yet. | Manual override that is explicit, visible, and tested against a live agent. |
| Global emergency stop | PARTIAL | `/estop` latches `EmergencyStop` per client (`src/app/client_app_commands.cpp:699`, `:944`), the Combat window exposes it (`ui/modern/combat.lua:606`), and the bus can dispatch a command to every session (`CommandScope::AllIncludingSelf`). Tested at the combat-engine level. | One control that stops every agent *and* every character, fails closed if the bus is down, and is tested as a fleet action. |
| Observation-only mode | NOT STARTED | — | A mode that runs the pipeline up to observation and logs it, with no proposals. |
| Assisted mode | NOT STARTED | — | Proposals shown in the Commander window, applied only on human approval. |
| Autonomous mode | NOT STARTED | — | Gated behind arbitration, validation and the safety tests above. |
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

## Safety stance

- No autonomous live action is enabled. Nothing in `src/cortex/` can send a command.
- The observation layer needs no game running to be tested.
- `docs/todo.md` carries the open items; this document carries the status and the
  evidence.

## How to check this document

```sh
cmake --build build -j8                  # Release, Ninja
build/hailstone_tests -tc="cortex observation*"
build/hailstone_tests                    # whole suite; TMPDIR must be short (docs/ci.md)
python3 tools/sync_wiki.py --check
```
