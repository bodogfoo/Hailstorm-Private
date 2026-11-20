-- Hailstone Combat Controller (CombatWnd): native Hailstone combat UI (/combat, /estop).
-- Uses only Hailstone API.combat bindings; zero MacroQuest / RGMercs / /stick dependencies.

local API = hs.UI
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color

local WINDOW = "CombatWnd"
local WINDOW_OPTS = {
  size = function() return 700, 560 end,
  resizable = { 520, 400 },
}

local MOVEMENT_MODES = {
  "None",
  "Follow",
  "AssistRange",
  "MeleeRange",
  "RangedRange",
  "DisengageDistance",
}

local STATE_COLORS = {
  Idle = "muted",
  Assist = "good",
  Engage = "good",
  Combat = "good",
  Disengage = "gold",
  Retreat = "bad",
  Dead = "danger",
  Paused = "gold",
  EmergencyStop = "danger",
}

local GEM_COLORS = {
  Matched = "good",
  Missing = "gold",
  ManualChange = "bad",
  NotInBook = "danger",
  SlotEmpty = "faint",
}

local assistInput = ""
local followInput = ""
local syncedChar = ""
local lastMsg = ""
local lastOk = true

local function colored(c, text)
  local col = c or C.text
  ui.TextColored(col[1], col[2], col[3], 1, text)
end

local function colorByName(name)
  return C[name] or C.text
end

local function runCmd(action, arg)
  local ok, msg = API.combat.Do(action, arg or "")
  lastOk = ok == true
  if msg and msg ~= "" then lastMsg = msg end
  return ok
end

local function checkboxCmd(label, val, action, tip)
  local v, changed = ui.Checkbox(label, val == true)
  if changed then runCmd(action, v and "1" or "0") end
  if tip and ui.IsItemHovered() then ui.SetTooltip(tip) end
end

local function drawOverviewTab(st, s)
  ui.Spacing()

  -- Movement Mode + Assist/Follow targets
  ui.Text("Movement Mode:")
  ui.SameLine()
  ui.SetNextItemWidth(170 * s)
  local curMode = st.movementMode or "None"
  if ui.BeginCombo("##moveMode", curMode) then
    for _, m in ipairs(MOVEMENT_MODES) do
      if ui.Selectable(m, m == curMode) then
        runCmd("movement_mode", m)
      end
    end
    ui.EndCombo()
  end

  ui.SameLine()
  ui.SetNextItemWidth(145 * s)
  local aTxt, aEnter = ui.InputTextWithHint("##assistTarget", "Assist (main_assist/name)", assistInput, 64,
    ui.InputTextFlags.EnterReturnsTrue)
  assistInput = aTxt
  ui.SameLine()
  if ui.SmallButton("Set Assist") or aEnter then
    runCmd("assist_target", assistInput == "" and "none" or assistInput)
  end

  ui.SameLine()
  ui.SetNextItemWidth(135 * s)
  local fTxt, fEnter = ui.InputTextWithHint("##followTarget", "Follow target", followInput, 64,
    ui.InputTextFlags.EnterReturnsTrue)
  followInput = fTxt
  ui.SameLine()
  if ui.SmallButton("Set Follow") or fEnter then
    runCmd("follow_target", followInput == "" and "none" or followInput)
  end

  -- Threshold & distance sliders
  ui.Spacing()
  local colW = math.max(180 * s, ui.GetContentRegionAvail() * 0.48)
  ui.SetNextItemWidth(colW)
  local retHp, retCh = ui.SliderInt("Retreat HP %##retHp", st.retreatHpPct or 20, 0, 95, "%d%%")
  if retCh then runCmd("retreat_hp_pct", tostring(retHp)) end
  ui.SameLine()
  ui.SetNextItemWidth(colW)
  local engHp, engCh = ui.SliderInt("Engage Max Target HP %##engHp", st.engageAtHpPct or 99, 1, 100, "%d%%")
  if engCh then runCmd("engage_at_hp_pct", tostring(engHp)) end

  ui.SetNextItemWidth(colW)
  local ar, arCh = ui.SliderFloat("Assist Range##ar", st.assistRange or 120, 10, 300, "%.0f")
  if arCh then runCmd("assist_range", string.format("%.1f", ar)) end
  ui.SameLine()
  ui.SetNextItemWidth(colW)
  local mr, mrCh = ui.SliderFloat("Melee Range##mr", st.meleeRange or 12, 4, 40, "%.1f")
  if mrCh then runCmd("melee_range", string.format("%.1f", mr)) end

  ui.SetNextItemWidth(colW)
  local rmin, rminCh = ui.SliderFloat("Ranged Min##rmin", st.rangedMinRange or 25, 0, 200, "%.0f")
  if rminCh then runCmd("ranged_min_range", string.format("%.1f", rmin)) end
  ui.SameLine()
  ui.SetNextItemWidth(colW)
  local rmax, rmaxCh = ui.SliderFloat("Ranged Max##rmax", st.rangedMaxRange or 110, 20, 300, "%.0f")
  if rmaxCh then runCmd("ranged_max_range", string.format("%.1f", rmax)) end

  ui.SetNextItemWidth(colW)
  local fd, fdCh = ui.SliderFloat("Follow Dist##fd", st.followDistance or 18, 5, 100, "%.0f")
  if fdCh then runCmd("follow_distance", string.format("%.1f", fd)) end
  ui.SameLine()
  ui.SetNextItemWidth(colW)
  local dd, ddCh = ui.SliderFloat("Disengage Dist##dd", st.disengageDistance or 45, 10, 200, "%.0f")
  if ddCh then runCmd("disengage_distance", string.format("%.1f", dd)) end

  ui.Spacing()
  colored(C.heading or C.accent, "Active Combat Rules")
  local flags = ui.TableFlags.RowBg | ui.TableFlags.ScrollY | ui.TableFlags.SizingStretchProp | ui.TableFlags.BordersInnerV
  if ui.BeginTable("##combatRules", 6, flags, 0, 0) then
    ui.TableSetupColumn("On", ui.TableColumnFlags.WidthFixed, 34 * s)
    ui.TableSetupColumn("Pri", ui.TableColumnFlags.WidthFixed, 34 * s)
    ui.TableSetupColumn("Rule", ui.TableColumnFlags.WidthStretch, 0.9)
    ui.TableSetupColumn("Spell / Ability", ui.TableColumnFlags.WidthStretch, 1.3)
    ui.TableSetupColumn("Action", ui.TableColumnFlags.WidthFixed, 85 * s)
    ui.TableSetupColumn("Condition", ui.TableColumnFlags.WidthFixed, 105 * s)
    ui.TableHeadersRow()

    for idx, r in ipairs(st.rules or {}) do
      ui.PushID("rule_" .. idx)
      ui.TableNextRow()
      ui.TableNextColumn()
      local en, ch = ui.Checkbox("##en", r.enabled == true)
      if ch then runCmd("toggle_rule", tostring(idx)) end
      ui.TableNextColumn()
      colored(C.muted, tostring(r.priority or 0))
      ui.TableNextColumn()
      colored(r.enabled and C.text or C.faint, r.name or "")
      ui.TableNextColumn()
      colored(r.enabled and C.text or C.faint, r.spell or "")
      ui.TableNextColumn()
      colored(C.muted, r.action or "")
      ui.TableNextColumn()
      colored(C.muted, (r.target or "") .. " / " .. (r.condition or ""))
      ui.PopID()
    end
    ui.EndTable()
  end
end

local function drawLoadoutTab(st, s)
  ui.Spacing()
  if st.autoLoadout then
    colored(C.bad or C.danger, "AUTO LOADOUT ENABLED: Missing or manually changed gems WILL be overwritten.")
  else
    colored(C.good, "Manual Gem Protection ACTIVE: Hailstone will NOT overwrite manually changed gems.")
  end

  local missingCount = #(st.missingSpells or {})
  local manualCount = #(st.manuallyChangedGems or {})
  local unknownCount = #(st.unknownSpells or {})
  colored(C.muted, string.format(
    "Missing empty slots: %d  |  Manually changed (protected): %d  |  Not in spellbook: %d",
    missingCount, manualCount, unknownCount))
  ui.SameLine()
  if ui.SmallButton("Rememorize Missing Now") then
    runCmd("rememorize_now")
  end
  if ui.IsItemHovered() then
    ui.SetTooltip("Memorizes missing desired spells into empty slots (and skips manually changed gems unless Auto Loadout is enabled)")
  end

  ui.Spacing()
  local flags = ui.TableFlags.RowBg | ui.TableFlags.ScrollY | ui.TableFlags.SizingStretchProp | ui.TableFlags.BordersInnerV
  if ui.BeginTable("##combatLoadout", 5, flags, 0, 0) then
    ui.TableSetupColumn("Gem", ui.TableColumnFlags.WidthFixed, 40 * s)
    ui.TableSetupColumn("Desired Spec", ui.TableColumnFlags.WidthStretch, 1.3)
    ui.TableSetupColumn("Resolved", ui.TableColumnFlags.WidthStretch, 1.1)
    ui.TableSetupColumn("Current Gem", ui.TableColumnFlags.WidthStretch, 1.1)
    ui.TableSetupColumn("Status", ui.TableColumnFlags.WidthFixed, 105 * s)
    ui.TableHeadersRow()

    for _, slot in ipairs(st.loadout or {}) do
      ui.TableNextRow()
      ui.TableNextColumn()
      colored(C.muted, tostring(slot.gem or 0))
      ui.TableNextColumn()
      colored(C.text, slot.desired or "")
      ui.TableNextColumn()
      colored(slot.resolved ~= "" and C.text or C.faint, slot.resolved ~= "" and slot.resolved or "(not found)")
      ui.TableNextColumn()
      colored(slot.current ~= "" and C.text or C.faint, slot.current ~= "" and slot.current or "<empty>")
      ui.TableNextColumn()
      local status = slot.status or ""
      colored(colorByName(GEM_COLORS[status] or "muted"), status)
    end
    ui.EndTable()
  end
end

local function drawLogTab(st, s)
  ui.Spacing()
  if ui.SmallButton("Clear Logs") then runCmd("clear_logs") end
  ui.SameLine()
  colored(C.muted, "Profile: " .. (st.profilePath ~= "" and st.profilePath or "(built-in defaults)"))

  local availW, availH = ui.GetContentRegionAvail()
  local topH = math.max(110 * s, availH * 0.46)
  local flags = ui.TableFlags.RowBg | ui.TableFlags.ScrollY | ui.TableFlags.SizingStretchProp

  colored(C.heading or C.accent, "State Transitions")
  if ui.BeginTable("##combatTransitions", 4, flags, availW, topH) then
    ui.TableSetupColumn("Time", ui.TableColumnFlags.WidthFixed, 68 * s)
    ui.TableSetupColumn("From", ui.TableColumnFlags.WidthFixed, 95 * s)
    ui.TableSetupColumn("To", ui.TableColumnFlags.WidthFixed, 105 * s)
    ui.TableSetupColumn("Reason", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableHeadersRow()

    local tr = st.transitions or {}
    for i = #tr, 1, -1 do
      local t = tr[i]
      ui.TableNextRow()
      ui.TableNextColumn()
      colored(C.faint, string.format("%.1fs", (t.timeMs or 0) / 1000.0))
      ui.TableNextColumn()
      colored(C.muted, t.from or "")
      ui.TableNextColumn()
      colored(colorByName(STATE_COLORS[t.to] or "text"), t.to or "")
      ui.TableNextColumn()
      colored(C.text, t.reason or "")
    end
    ui.EndTable()
  end

  ui.Spacing()
  colored(C.heading or C.accent, "Action & Dry-Run Log")
  if ui.BeginTable("##combatActions", 4, flags, 0, 0) then
    ui.TableSetupColumn("Time", ui.TableColumnFlags.WidthFixed, 68 * s)
    ui.TableSetupColumn("Mode", ui.TableColumnFlags.WidthFixed, 72 * s)
    ui.TableSetupColumn("Category", ui.TableColumnFlags.WidthFixed, 90 * s)
    ui.TableSetupColumn("Detail", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableHeadersRow()

    local acts = st.actions or {}
    for i = #acts, 1, -1 do
      local a = acts[i]
      ui.TableNextRow()
      ui.TableNextColumn()
      colored(C.faint, string.format("%.1fs", (a.timeMs or 0) / 1000.0))
      ui.TableNextColumn()
      colored(a.dryRun and C.gold or C.good, a.dryRun and "DRY-RUN" or "LIVE")
      ui.TableNextColumn()
      colored(C.muted, a.category or "")
      ui.TableNextColumn()
      colored(C.text, a.detail or "")
    end
    ui.EndTable()
  end
end

local function drawWindow(x, y, w, h, s)
  local st = API.combat.Status()
  if not st or not st.available then
    colored(C.muted, st and st.idleReason or "Combat controller unavailable.")
    return
  end

  local charTag = (st.characterName or "") .. "@" .. (st.serverName or "")
  if charTag ~= syncedChar then
    syncedChar = charTag
    assistInput = st.assistTarget or ""
    followInput = st.followTarget or ""
  end

  -- Prominent Emergency Stop + Control Row
  local estop = st.state == "EmergencyStop"
  ui.PushStyleColor(ui.Col.Button, C.danger[1], C.danger[2], C.danger[3], 0.78)
  ui.PushStyleColor(ui.Col.ButtonHovered, C.danger[1], C.danger[2], C.danger[3], 0.96)
  ui.PushStyleColor(ui.Col.ButtonActive, C.danger[1] * 0.85, C.danger[2] * 0.85, C.danger[3] * 0.85, 1.0)
  if ui.Button("EMERGENCY STOP##combatEstop", 148 * s, 26 * s) then
    runCmd("estop")
  end
  ui.PopStyleColor(3)
  if ui.IsItemHovered() then
    ui.SetTooltip("Immediately halt movement, casting, auto-attack, and recall pet (/estop)")
  end

  ui.SameLine()
  if estop then
    if ui.Button("Reset E-Stop##resetEstop", 105 * s, 26 * s) then
      runCmd("reset_estop")
    end
  elseif not st.running then
    if ui.Button("Start##combatStart", 70 * s, 26 * s) then
      runCmd("start")
    end
  else
    if st.state == "Paused" then
      if ui.Button("Resume##combatResume", 72 * s, 26 * s) then runCmd("resume") end
    else
      if ui.Button("Pause##combatPause", 72 * s, 26 * s) then runCmd("pause") end
    end
    ui.SameLine()
    if ui.Button("Stop##combatStop", 64 * s, 26 * s) then runCmd("stop") end
  end

  ui.SameLine()
  if ui.Button("Reload##combatReload", 64 * s, 26 * s) then
    if runCmd("reload") then
      syncedChar = ""
    end
  end
  ui.SameLine()
  if ui.Button("Save##combatSave", 58 * s, 26 * s) then
    runCmd("save")
  end

  ui.SameLine()
  local stateColor = colorByName(STATE_COLORS[st.state] or "text")
  colored(stateColor, string.format("  [%s]", st.state or "Idle"))
  ui.SameLine()
  colored(C.muted, string.format("%s (%s) · %s · %s",
    st.characterName ~= "" and st.characterName or "Unknown",
    st.classShort ~= "" and st.classShort or "?",
    st.zoneShort ~= "" and st.zoneShort or "Offline",
    st.role ~= "" and st.role or "Default"))

  -- Target & Safety Status Strip
  local targetLine = st.targetId and st.targetId ~= 0
    and string.format("Target: %s (#%d, %d%% HP, %.1f dist)",
      st.targetName or "", st.targetId, st.targetHpPct or 0, st.targetDistance or 0)
    or "Target: <none>"
  local assistLine = (st.assistTargetName and st.assistTargetName ~= "")
    and ("  |  Assist: " .. st.assistTargetName)
    or ""
  local flagsLine = (st.autoAttackActive and "  |  AutoAttack ON" or "")
    .. (st.dryRun and "  |  DRY-RUN" or "")
    .. (st.manualOverrideActive and string.format("  |  MANUAL OVERRIDE (%.1fs)", (st.manualOverrideRemainingMs or 0) / 1000.0) or "")
  colored(C.text, targetLine .. assistLine .. flagsLine)

  if estop then
    colored(C.danger, "EMERGENCY STOP ACTIVE: All combat & movement halted until Reset E-Stop.")
  elseif st.state == "Idle" and st.idleReason and st.idleReason ~= "" then
    colored(C.gold, "Idle Reason: " .. st.idleReason)
  elseif st.transitionReason and st.transitionReason ~= "" then
    colored(C.muted, "Transition: " .. st.transitionReason ..
      (st.lastCast ~= "" and ("  |  Last Cast: " .. st.lastCast) or "") ..
      (st.lastMovementDecision ~= "" and ("  |  Move: " .. st.lastMovementDecision) or ""))
  end

  if lastMsg ~= "" then
    colored(lastOk and C.good or C.danger, lastMsg)
  end

  -- Quick safety & mode checkboxes
  ui.Separator()
  checkboxCmd("Dry-Run##cbDry", st.dryRun, "dry_run", "Log decisions without sending attack/cast/move actions")
  ui.SameLine()
  checkboxCmd("Auto Loadout##cbLoadout", st.autoLoadout, "auto_loadout",
    "OFF = never overwrite manually changed gems. ON = overwrite mismatched gems.")
  ui.SameLine()
  checkboxCmd("Remem Missing##cbRemem", st.rememorizeMissing, "rememorize_missing",
    "Safely memorize missing profile spells into empty gem slots")
  ui.SameLine()
  checkboxCmd("Melee##cbMelee", st.allowMeleeAttack, "allow_melee_attack", "Allow melee auto-attack when in range")
  ui.SameLine()
  checkboxCmd("Pet##cbPet", st.allowPetAttack, "allow_pet_attack", "Send pet on valid combat target")
  ui.SameLine()
  checkboxCmd("Abilities##cbAbil", st.useAbilities, "use_abilities", "Allow class abilities (Backstab, Bash, Kick, Taunt)")
  ui.SameLine()
  checkboxCmd("Behind##cbBehind", st.meleeBehind, "melee_behind", "Prefer positioning behind target when melee sticking")
  ui.SameLine()
  checkboxCmd("Manual Pause##cbManualPause", st.pauseOnManualMove, "pause_on_manual_move",
    "Pause combat when player moves/backs away manually")
  ui.Separator()

  if ui.BeginTabBar("##combatTabs") then
    if ui.BeginTabItem("Overview & Rules") then
      drawOverviewTab(st, s)
      ui.EndTabItem()
    end
    if ui.BeginTabItem("Spell Loadout") then
      drawLoadoutTab(st, s)
      ui.EndTabItem()
    end
    if ui.BeginTabItem("Transitions & Log") then
      drawLogTab(st, s)
      ui.EndTabItem()
    end
    ui.EndTabBar()
  end
end

ui.window("Combat Controller", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Hailstone Combat", WINDOW_OPTS, drawWindow)
end)
