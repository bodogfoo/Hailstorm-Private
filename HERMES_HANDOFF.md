# Hermes Handoff — Project Hailstone Native

Written for the draft pull request
[`#1`](https://github.com/bodogfoo/Hailstorm-Private/pull/1), branch
`codex/hailstone-unified-review` into `main`.

## Repository and environment

- Repository: `/home/john/src/hailstone-native`
- Build folder: `/home/john/src/hailstone-native/build` (Ninja, Release, tests on)
- Runtime data: `/home/john/.local/share/hailstone24` (separate from this repo; not modified here)
- Game assets: `/home/john/Games/EZ-RoF2-clean-20260828`
- Session bus socket: `/run/user/1000/hailstone24/bus.sock`

## Branch state

`codex/hailstone-unified-review` is built in three layers:

1. `993e7441` "Add summon companion confirmation" — the published
   `origin/codex/hailstone-commander-build-fix` tip this work started from.
2. `b951498d` — a normal merge of the local grass/flora line (`main` at `16d4a24d`), plus the
   commander work that had been sitting uncommitted in the worktree, organised into conventional
   commits on top of `993e7441`. Both lines are preserved; the only conflict was the coverage line of
   `docs/ui-api-reference.md`, resolved by regenerating that document from the merged sources with
   `tools/gen_ui_api_docs.py`.
3. `c5e2751b` — a merge of `origin/main` (`809b9ef8`) with `--allow-unrelated-histories`, which is
   what makes the pull request into `main` possible at all. See below.

### Why `origin/main` had to be merged, and why that is safe

`main` in `bodogfoo/Hailstorm-Private` is **not** this project's native history. It is a separate
history with its own root (`8c652221` "Initial commit", 11 commits, 154 files): the Vite/React
"Hailstone 24 Workbench" app plus `hailstone24/overlay` copies of client sources, configs, docs and
patches. This line's root is `3fdabc8a` (1531 commits, 2372 files). The two share no ancestor —
`git merge-base origin/main HEAD` exits 1 — and GitHub refuses a pull request between them:

```
$ gh pr create --draft --base main --head codex/hailstone-unified-review ...
pull request create failed: GraphQL: The codex/hailstone-unified-review branch has no history in
common with main (createPullRequest)
```

That output is the verified limitation, not a guess. Rather than change the base, invent a
`codex/native-main` branch or force anything, `origin/main` was merged into the integration branch
with `--allow-unrelated-histories`. A dry run (`git merge-tree --write-tree
--allow-unrelated-histories`) showed exactly one conflict — `.gitignore`, add/add — resolved as the
union of both rule sets. The merge:

- adds main's 154 files unchanged (+129448 lines) and deletes nothing from main;
- leaves every native file as it was (no source file is touched, so the build and test results above
  still apply to this tree);
- does not modify, push to, or otherwise touch `main` itself.

The PR is therefore an import/bridge: it brings the native tree into the repository that hosts
`main`, keeping main's workbench files in place. That is a decision for the reviewer at merge time,
which is why the PR is a draft.

Local `main` (`16d4a24d`) and `origin/main` (`809b9ef8`) were not modified by this work.

## What is already committed at 993e7441 (baseline, not this branch)

- `/combatstick [distance] | off` — native combat stick: validates an NPC target, sets melee range,
  allows melee attack and starts the combat engine; `off` releases it.
- `/summoncompanion` — finds the carried horn ("Call of the Companions" / "Summon Companion";
  Possessions, one bag level), refuses when the target is not this character or another confirmation
  is pending, and shows a 3000 ms Yes/No confirmation that **answers Yes on timeout** via the new
  `UiHost::Confirm::timeout_answer`.
- Neither had a test at that commit.

## What this branch adds on top

Group launch and login:

- A group slot already online in the session bus is refused before a second client is started (no
  duplicate client's RAM/VRAM), and the profile folder scan is logged.
- "Force stop all" in both login screens: the first press only arms it (5 s), a second press sends
  SIGKILL — and only to a PID whose `/proc` entry still runs this client's program with that
  `--session-id`. Anything else is refused and reported. Any other login action disarms it.
- The Lua login screen receives the group profiles and slots it needs to draw the profile card, and
  the slot table uses the same borders as the rest of the screen.

Group profiles:

- A profile's named leader is the follow authority: the leader slot follows nobody, other slots with
  no target of their own follow the leader, explicit targets win.

Combat:

- Combat navigation forces run mode before follow, assist-range, melee and disengage movement.
- Arriving in melee range holds the stick (`MoveMelee`, "Holding melee stick on <name>") instead of
  calling `force_stop()` and releasing it.
- Combat window: 860x680 with a 640x480 floor, per-client assist dot, group tabs (All, Group 1-4),
  connected count, follow field synced with commander actions, roster height taken from the window
  height.

Session bus, RGMercs, launch:

- Foreground tab switching skips peers that report offline, so a stale roster entry cannot absorb the
  switch.
- Zone entry normalises the character's autorun file: `rgmercs/heartbeat` off, `rgmercs` on, started
  if needed.
- The manager passes `--no-audio` to group clients.
- `--data-dir` is applied before the data folder is validated.

## Behaviour changes to weigh before shipping

- Run mode is forced unconditionally during combat navigation: a deliberate slow-walk preference is
  overridden.
- The melee stick is no longer released by the controller on arrival. Release is `/combatstick off`,
  disengage, or losing the target; if none happens the character keeps sticking.
- Zone entry rewrites the autorun file, so an intentional "RGMercs off" is reverted on the next
  zone-in.
- Leader-follow defaults change launch/follow behaviour for existing profiles that name a leader.
- Group tabs key off the `mq-group<N>-<n>` session-id convention; other ids appear only under "All".
- Force stop's verified kill is Linux-only; on Windows only `box_clients_.stop_all()` runs, and the
  status line reports 0 killed.
- `/summoncompanion` answering Yes on timeout means an unanswered prompt summons when the dialog
  expires.
- The merge of `origin/main` puts the workbench app at the repository root beside the native tree.
  Reviewers who do not want that layout should reject the merge commit rather than edit it.

## Tests

Added:

- `tests/hailstone24/companion_call_test.cpp` — the horn's names (matched and refused), the carried
  search (top level, one bag deep, not the bank, not a bag-in-a-bag), and
  `confirmation_timeout_answer` (Yes only for question 3 with `timeout_answer`, false for every other
  question and for no pending dialog).
- `tests/hailstone24/process_identity_test.cpp` — a spawned child carrying `--session-id`: wrong
  program name and wrong session id refused, the verified kill observed as SIGKILL, a reaped PID not
  killed again, and PIDs 0/1/own/gone refused.
- `tests/hailstone24/combat_controller_test.cpp` — the melee hold: one `move_melee` issued, arrival
  reports the hold with no `stop_movement`, nothing re-issued, and a disengage mode still moves away.
- `tests/hailstone24/group_profile_test.cpp` — leader defaults: leader's stale target cleared, empty
  target becomes the leader, explicit target survives, no leader means no default.

Updated stale baseline expectations (pre-existing defects, not behaviour of this branch):

- `tests/lua_ui_bindings_test.cpp` — the EQ menu now includes Combat Controller, RGMercs and Special
  Functions.
- `tests/mq_lua_test.cpp` — the sandbox exposes a text-only `load` (`lua_mq.cpp` `l_load_text`); the
  old expectation was `nil nil nil`. It now asserts text-only loading and that a binary chunk is
  refused.

Not covered: `ClientApp`'s command wiring and the Lua UI scripts themselves (the test binary links the
engine, not `client_app_commands.cpp`, and no test host runs `ui/modern/*.lua`). The force-stop
arming flow is therefore verified by the platform helper's tests plus code review, not end to end.

## Build and test state

All of the following was run on the merged tree, on Linux, GNU 13.3.0, with no client or runtime
started.

Build (`build/CMakeCache.txt`: `CMAKE_BUILD_TYPE=Release`, `CMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG`,
`HAILSTONE_BUILD_TESTS=ON`; configure log `~/.hermes/cache/scratch/hailstone-unify-configure.log`):

```
cmake --build build -j8          →  [1656/1656] Linking CXX executable hailstone_tests
                                    BUILD_EXIT=0            (log: hailstone-unify-build.log)
cmake --build build -j8          →  [24/24] after the origin/main merge, links only, exit 0
```

Full suite (`TMPDIR=/home/john/.hermes/cache/scratch/hs-tmp`):

```
commit b951498d (pre-merge tree):  test cases 4000 | 4000 passed | 0 failed | 74 skipped
                                   assertions 1729703 | 1729703 passed | 0 failed   EXIT=0
                                   (log: hailstone-unify-tests.log)
commit c5e2751b (final tree):      test cases 4000 | 4000 passed | 0 failed | 74 skipped
                                   assertions 1729702 | 1729702 passed | 0 failed   EXIT=0
                                   (log: hailstone-unify-tests-final.log)
```

Status `SUCCESS!` in both runs. The one-assertion difference between them is a conditional assertion
in the suite, not a failure; no case failed in either run. 74 cases are skipped by the suite's own
`--skip`/`gt` guards in both runs.

Compiler warnings on this tree are pre-existing except one branch-related `-Wswitch` in
`src/app/ui_demo.cpp:196`: the demo app's switch over `LoginAction` does not handle the new
`LaunchGroup`, `LaunchGroupSlot`, `SelectGroupProfile`, `RefreshGroupProfiles`, `RelaunchGroup` and
`StopGroup` values. It is a warning only (the demo has no behaviour for them) and no test covers it.

Test environment note: the session-bus tests create a socket under `TMPDIR`, and a deep default
scratch path pushes it past the 108-byte `sun_path` limit, so they fail for an environmental reason
(`REQUIRE( srv.start(sock) )`). Run the suite with a short `TMPDIR`, e.g.
`TMPDIR=/home/john/.hermes/cache/scratch/hs-tmp`.

## Safety notes

- `main` on GitHub is the workbench repo; it was merged here deliberately (see above) and is not
  modified or pushed by this work.
- No branch was force-pushed. `main` (local `16d4a24d`, remote `809b9ef8`) is unchanged.
- Runtime scripts under `~/.local/share/hailstone24/scripts/rgmercs/` and `porter_go.lua` are
  separate from this repository and were not touched (fingerprints in the final report).
- No client, broker or manager process was started by this work, and no game window was touched.
- Do not start a second broker with a different socket or data directory.
- If a group reports "already online", inspect the roster before launching again; the bus guard
  exists because a duplicate client costs a full client's memory.

## Last verified build

The numbers in "Build and test state" are the last verified build: Release, 1656/1656 targets,
exit 0, and the full suite green (4000/4000 cases) on `c5e2751b` — the tree the suite was run on. The
two commits after it touch only this document and `tools/sync_wiki.py`'s PAGES list, so no compiled
source changed; the wiki check they fix (`python3 tools/sync_wiki.py --check`) exits 0 and writes 45
pages.
