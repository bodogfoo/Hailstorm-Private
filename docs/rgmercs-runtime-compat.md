# RGMercs runtime compatibility fixes

These fixes are applied to the user-installed RGMercs tree under
`~/.local/share/hailstone24/scripts/rgmercs`. They are kept here as a tracked
handoff because the upstream Hailstone repository does not vendor the third-
party RGMercs scripts.

## Applied changes

- `utils/ui.lua`
  - Fall back to `<Unused>` when `ImGui.GetStyleColorName()` returns no name.
  - Read `ImGuiStyleVar` through its `__index` table when available.
  - Cache `mq.TLO.Raid.Members()` as a nil-safe numeric `raidMembers` value.
- `ui/options.lua` and `ui/images.lua`
  - Use Hailstone's `mq.luaDir` for RGMercs image paths instead of the
    unavailable `mq.TLO.Lua.Dir()` TLO.
- `modules/pull.lua`
  - Treat missing `Taunt` and `Kick` skill values as zero before comparison.

## Installation

The live files are already installed at:

```text
~/.local/share/hailstone24/scripts/rgmercs/
```

If the RGMercs script tree is restored or replaced, reapply the changes above.
These Lua fixes complement the native compatibility changes in this branch;
they do not vendor credentials or account data.

## Runtime note

The client now gets through RGMercs initialization farther than before. Missing
MacroQuest companion plugins such as MQ2AdvPath, MQ2MoveUtils, MQ2Nav, and
MQ2DanNet remain expected compatibility warnings unless equivalent Hailstone
services are enabled.
