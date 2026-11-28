# Hailstone Native Epic Quest & Loot Management System

## Overview

Hailstone's epic quest and loot management system (`src/epic/`) tracks per-character epic quest progression independently by `(server, character, class, epic_variant)` and makes conservative, progression-aware loot decisions without any MacroQuest runtime dependencies.

## Persistence & Data Files

Under `<data_dir>` (default `/home/john/.local/share/hailstone24`):

- `epic_progress.json` — Versioned JSON database (`schema_version: 1`) storing per-character progress, global/group/character loot policies, manual overrides, and audit history. Never stores passwords, tokens, or secrets.
- `epic_quests.json` — Optional external epic quest definitions (`schema_version: 1`) merged on top of built-in defaults.
- `epic_progress.backup.json` — Backup snapshot created by the UI **Backup DB** button or `/epic backup`.

## Supported Epic Variants

Built-in definitions include:
- Classic EQ 1.0 epics across all 16 classes (`war_1_0`, `clr_1_0`, `pal_1_0`, `shd_1_0`, `rng_1_0`, `dru_1_0`, `mnk_1_0`, `brd_1_0`, `rog_1_0`, `shm_1_0`, `nec_1_0`, `wiz_1_0`, `mag_1_0`, `enc_1_0`, `bst_1_5`, `ber_1_5`)
- EZ Server variants (`war_ez_v2`, `clr_ez_2_0`) with multi-stage progression, alternative item support, and upgrade links
- Any custom server/class epic definitions added via `epic_quests.json`

## UI & Commands

Open **Special Functions** (`/specialfunctions` or `/special` or `/epic`) and select the **Epic Quests & Loot** tab:
- **Characters (24-Box)**: Group/class/name filters, progress percentage, state badges, manual stage completion, variant switcher, and rollback.
- **Items & Conflicts**: Aggregated item needs, conflicts, source zones, and recommended actions.
- **Policy & Priorities**: Global/group/character loot policies, conflict resolution rules, character priorities, and item reservations.
- **Audit & Diagnostics**: Structured loot decision logs and dry-run/emergency-disable controls.

Slash commands:
- `/epic` or `/epic ui` — Open Special Functions
- `/epic scan` — Scan current character inventory
- `/epic dry_run on|off` — Toggle dry-run mode
- `/epic emergency_disable on|off` — Toggle global emergency disable
- `/epic evaluate_loot` — Evaluate the currently open loot session
