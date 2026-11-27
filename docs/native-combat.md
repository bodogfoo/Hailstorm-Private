# Native combat engine

Hailstone drives a character's fighting from C++ (`src/combat/`). The RGMercs Lua
add-on can do it too, and on an install that came from the add-on it is what
players used. This document describes the native side: what decides that it is in
charge, what each class does, why an action was or was not taken, how a legacy
group profile is carried over, and what is still missing.

The engine itself (states, assist/follow resolution, melee and range movement,
gem loadout and casting, pet handling, retreat and recovery) predates this work
and is described by its own types in `src/combat/combat_types.h`. This document
covers the parts added to make it a replacement rather than a second opinion.

## Engine selection

Two things must never issue actions for one character in one tick. Every native
action passes `ActionIssuerGate::allow_native()`; the legacy bridge asks
`allow_legacy()`. A tick belongs to whichever engine the selection names, and a
second engine that tries to act anyway is refused and recorded (last_violation,
violation_count) rather than silently racing.

The selection is layered, each layer replacing the one above it:

    combat.native.native_combat              the master switch
    combat.native.native_class_profiles      use the per-class table (below)
    combat.native.native_movement            native assist/follow/movement
    combat.native.native_spellcasting        native gem loadout and casting

    combat.native.group.<group id>.<key>     one group
    combat.native.character.<name>.<key>     one character

These are keys in the client's own settings file (`hailstone.ini`, or the
character's file, at the same paths as any other setting; see
[layout.md](layout.md)). Values are `1/0`, `true/false`, `on/off`, and a layer
that sets any of the four keys replaces the whole set, so a character can switch
one subsystem off without repeating the rest.

The master switch gates the rest: `native_spellcasting=1` on its own does
nothing.

Nothing is configured by default, and `resolve_engine_from_settings()` returns
"not configured" rather than a choice, so an install that changes nothing behaves
as it did before this feature existed. When the keys are set and both engines ask
to act, native wins (the player switched it on explicitly) and the legacy engine
is told to stand down with a reason recorded on the selection.

The client reads the keys at a zone load and on `/hcombat reload`
(`ClientApp::refresh_combat_engine_selection()`), so a changed flag takes effect
at the next zone, not mid-fight. When the native engine owns the character the
client pauses the `rgmercs` script once (`ScriptHost::pause()`) instead of
stopping it, so a player who turns the flags back off gets the add-on back; a
script that is not running is left alone. Only `CombatEngine::tick()` is gated:
the snapshot is still built when another engine owns the character, so the combat
window keeps showing what the client sees. `/hcombat status` ends with the engine
and its flags, and `/hcombat start` refuses, naming the reason, when the selection
says the legacy engine owns the character. `native_class_profiles` replaces the
character's saved profile with the class table entry for the class, keeping the
saved profile path so `/hcombat save` still writes where it did.

## Class profiles

`src/combat/class_profiles.cpp` holds one table entry per supported class: WAR,
ROG, CLR, MAG, NEC and ENC. An entry says what the class is for and how it fights:

- role, movement mode, melee or ranged, pet use, whether it heals, resurrects,
  buffs or crowd-controls;
- engagement thresholds (engage at, heal at, retreat at, pet recover at) and the
  ranges used for assist, melee, ranged and disengage;
- a gem loadout (which spell goes in which gem, in order);
- an ordered spell rule list: spell name (alternatives may be separated by `|`),
  target, condition, priority, mana window, target/self HP window, range and
  cooldown;
- `required_spells` and `core_buffs`.

The melee range an entry carries is where the melee stick stops — positioning — not
the distance at which an attack lands. `melee_range` is 3 units by default and is
configurable. Attack activation is separate: the engine turns auto-attack on and
leaves Engage at the original's own melee range for the two spawns,
`__get_melee_range` (`game/mechanics/melee.h`: `max(14, (radius_a*scale_a +
radius_b*scale_b)*0.75) + 2 + |AvatarHeight_a - AvatarHeight_b|`, at most 75). The
app layer computes it for every snapshot (`CombatSnapshot::melee_reach`), so it
follows the spawns' MeleeRadius, model scale and AvatarHeight instead of a fixed
number; two 6-radius spawns give 16. A snapshot with no spawn pair has no reach to
trust, and the engine then attacks only at the stick distance. A class table's
ability `max_range` is that ability's cast range and is not evidence of melee
reach. `disengage_distance` is measured from the target as before.

`build_native_combat_profile()` turns one entry into the `CombatProfile` the
engine already consumes, so adding a class is a table entry rather than a new
code path. A class with no entry is reported ("No native combat profile for class
..."), never given a guessed one.

Spell names are the RoF2-era names the player's own `spells_us.txt` carries; the
client reads that file from the game folder. A name the client cannot resolve is
reported as `SpellDataMissing` when it tries to use it. It is not silently
substituted, and no spell table is shipped.

## Casting

`evaluate_cast_readiness()` answers "can this be cast, and if not why" as a value
instead of a silent no-op. The blockers are:

    NotInZone, CharacterDead, Stunned, AlreadyCasting, Memorizing,
    NotInSpellbook, LevelTooLow, SpellDataMissing, NotMemorized, GemLocked,
    NotEnoughMana, CooldownActive, NoTarget, TargetOutOfRange

`GemLocked` and `NotMemorized` are distinct on purpose: a gem holding a spell the
character cannot use (out of level, unusable) is not the same problem as an empty
gem, and the fix differs.

## Healing, pets and buffs

`choose_support_action()` reads a snapshot and returns one decision:

    Resurrect, Heal, CriticalHeal, GroupHeal, Cure, PetHeal, PetAttack, Buff

In order: a dead group member with a resurrection spell available is raised
first; then the worst-hurt living member below the heal threshold gets the
largest heal the character can cast (a critical heal below the critical
threshold); a hurt member with a removable effect gets the cure; a hurt pet gets
a pet heal; a pet without a target gets the current hostile; a missing core buff
on the character itself is refreshed.

The decision is a pure function of the snapshot, so six-box behaviour is tested
without a server. `CombatEngine::try_support_action()` consumes it: it runs the
decision through the same readiness check a rule goes through (memorized, ready,
in range, mana, rule cooldown), targets the decision's spawn when it differs from
the current target, casts the gem, and records the action under the profile rule
the decision came from (`Resurrect`, `Heal`, `HealCritical`, ...) so a
policy-chosen heal shares that rule's cooldown and diagnostics. A blocked support
action is recorded once with its reason and the damage rotation takes the tick —
a healer whose heal is not memorized still fights. The support pass runs before
the priority rules in combat and before the buff/idle path out of combat, held
back while the character is memorizing. Its thresholds come from the profile's own
rules (`support_settings_for_profile()`), so a class table entry and a saved
profile drive healing from one place; a class with no `PetHeal` rule falls back to
40%.

Two things are deliberately not done yet: buffing the tank and
other group members (their active buffs are not in the group snapshot) and curing
group members other than the one being healed.

## Diagnostics

A six-box session is unreadable if the same failure is logged every tick. Every
diagnostic is a record — character, server, class, group, module, severity,
action, target, reason, game state, ok, and how many times it occurred — and an
identical record inside a five-second window only bumps the count. `format_diagnostic()`
renders it as one line of `key=value` pairs, with newlines, tabs and pipes
replaced so a value cannot forge another field.

Modules: Engine, Combat, Movement, Spellbook, Casting, Healing, Pets, Buffs,
Recovery, Migration, Command, Ui.

The engine records every action it issues, and a tick that issues nothing records
why (`Idle` with the reason the controller gives — "Waiting for target", "Assist
target 'Tank' not in zone/range", "Paused by user"...), so "nothing happened" is
answerable from the log alone. A legacy command the native stack does not
implement warns once per session (`emit_unsupported_command`); later calls only
bump its count.

## Migrating from RGMercs

A legacy install keeps its groups in `profiles/<group>.ini` (`core::GroupProfile`:
slots with a character name, an account, a server and a `class_role`). The native
engine wants one combat profile per character. `src/combat/profile_migration.*`
carries the old arrangement over without touching it:

1. `plan_profile_migration()` reads every legacy group profile and returns a
   report: what it read, how many slots, which characters it can migrate, which
   legacy roles have no native class (reported, not guessed), and where each
   output would go.
2. `apply_profile_migration()` writes
   `<data_dir>/profiles/migrated/<group>/<character>.combat.ini` with a
   `[Migration]` section (schema version, source file, source slot, the legacy
   class role) and backs up each legacy file it touched.
3. `rollback_profile_migration()` removes exactly what the run created — including
   the backups — and leaves the legacy files as they were.

Role mapping is explicit: tank→WAR, healer→CLR, melee/dps→ROG, magician→MAG,
necromancer→NEC, enchanter/cc→ENC. Ambiguous roles ("caster", "dps", "bard") are
reported as unsupported rather than mapped to the nearest class.

A second run skips profiles already at the current schema version and upgrades an
older one (its `detail` says which), and a character name is confined inside the
data directory, so a hostile name cannot write outside it.

The legacy RGMercs tree itself is read-only reference. Its GPL-3.0-or-later code
is not copied: the class tables here were written from the class roles the game
gives them, and nothing from the add-on's Lua or its config database is
committed.

## Limitations

- The gate and the class table are wired into the client: `ClientApp` resolves the
  selection at a zone load and on `/hcombat reload`, enforces ownership every tick,
  and refuses a legacy Lua script command while the native engine owns the
  character. There is still no window for the flags: they are set in the settings
  file or by a script.
- The combat window's "Group 1".."Group 4" tabs read the group number out of a
  peer's session id (`mq-group<N>-<n>`), while every producer writes `slotNN`.
  With stock profiles every peer therefore falls under "All". Fixing it needs a
  decision on where a launched client learns its group number (a group profile
  has no group index today); `SessionSnapshot::group_slot` is serialized but no
  producer assigns it, so it can carry that number once the source is chosen.
- The session-id fallback for a slot that names none (`slotNN`) is written out in
  both launch paths (the client's own launcher and the session broker). They now
  agree, but a shared helper and a test would keep them from drifting again.
- The broker's `--launch-all` has no bus "this slot is already online" guard;
  only the in-process launcher checks the roster before spawning.
- Six classes. BRD, SHM, DRU, BST and the rest fall through to the existing
  defaults or report that they have no native profile.
- Group-wide buffing and curing group members other than the one being healed are
  not implemented (see above).
- Healing and buff thresholds are Hailstone's own defaults, not measured against
  the original client (see [unknowns.md](unknowns.md)).
