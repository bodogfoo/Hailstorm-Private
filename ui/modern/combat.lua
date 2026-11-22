-- Hailstone Combat Controller (CombatWnd): native Hailstone combat UI (/combat, /estop).
-- Uses only Hailstone API.combat bindings; zero MacroQuest / RGMercs / /stick dependencies.

local API = hs.UI
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color

local WINDOW = "CombatWnd"
local WINDOW_OPTS = {
  size = function() return 860, 680 end,
  resizable = { 640, 480 },
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
  LevelRestricted = "danger",
  MissingSpellData = "danger",
  SlotEmpty = "faint",
}

local ACK_COLORS = {
  Pending = "gold",
  Acked = "good",
  Partial = "gold",
  Failed = "danger",
  TimedOut = "danger",
}

local CORTEX_MODE_LABELS = {
  DISABLED = "Disabled",
  CORTEX_OBSERVE = "Observe",
  CORTEX_ASSISTED = "Assist",
  CORTEX_AUTONOMOUS = "Autonomous (Gated)",
}

local CORTEX_STATUS_COLORS = {
  DISABLED = "muted",
  OBSERVING = "good",
  QUEUED = "gold",
  THINKING = "gold",
  ["DECISION READY"] = "good",
  WAITING = "gold",
  ERROR = "danger",
}

local assistInput = ""
local followInput = ""
local syncedChar = ""
local lastStatusFollow = ""
local lastMsg = ""
local lastOk = true
local cmdScope = "all"
local selectedProfile = "Main Tank"
local scriptArgInput = "rgmercs"
local selectedGroup = 0

local function colored(c, text)
  local col = c or C.text
  ui.TextColored(col[1], col[2], col[3], 1, text)
end

local function colorByName(name)
  return C[name] or C.text
end

local function assistIndicator(peer)
  local state = string.lower(peer.combatState or "idle")
  local active = state == "assist" or state == "engage" or state == "combat"
  if not peer.online then
    return C.danger, "OFF", "Offline"
  end
  if active then
    return C.good, "ON", "Assist/combat active"
  end
  return C.danger, "OFF", peer.combatState ~= "" and peer.combatState or "Not assisting"
end

local function peerGroup(peer)
  local n = string.match(peer.sessionId or "", "mq%-group([1-4])%-%d+")
  return n and tonumber(n) or 0
end

local function runCmd(action, arg)
  local ok, msg = API.combat.Do(action, arg or "")
  lastOk = ok == true
  if msg and msg ~= "" then lastMsg = msg end
  return ok
end

local function runCommander(action, target, arg)
  if not API.commander or not API.commander.Do then
    lastOk = false
    lastMsg = "Commander API unavailable"
    return false
  end
  local ok, msg = API.commander.Do(action, target or "", arg or "")
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
    local requestedFollow = followInput == "" and "none" or followInput
    if runCmd("follow_target", requestedFollow) then
      -- Keep the visible field in sync immediately; CombatStatus will also
      -- refresh it after a tab/window switch.
      followInput = requestedFollow == "none" and "" or requestedFollow
    end
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
    colored(C.bad or C.danger, "AUTO LOADOUT ENABLED: Missing or manually changed gems WILL be overwritten (out of combat).")
  else
    colored(C.good, "Manual Gem Protection ACTIVE: Hailstone will NOT overwrite manually changed gems.")
  end

  local missingCount = #(st.missingSpells or {})
  local manualCount = #(st.manuallyChangedGems or {})
  local unknownCount = #(st.unknownSpells or {})
  local levelCount = #(st.levelRestrictedSpells or {})
  local dataCount = #(st.missingSpellData or {})
  local failCount = #(st.failedMemorizations or {})
  colored(C.muted, string.format(
    "Missing: %d  |  Manual (protected): %d  |  Unavailable: %d  |  Level-restricted: %d  |  Missing data: %d",
    missingCount, manualCount, unknownCount, levelCount, dataCount))
  ui.SameLine()
  if ui.SmallButton("Rememorize Missing Now") then
    runCmd("rememorize_now")
  end
  if ui.IsItemHovered() then
    ui.SetTooltip("Memorizes missing desired spells into empty slots while idle/out of combat (skips manually changed gems unless Auto Loadout is enabled)")
  end

  if failCount > 0 then
    colored(C.danger, "Failed memorizations: " .. table.concat(st.failedMemorizations, "  |  "))
  end

  ui.Spacing()
  local flags = ui.TableFlags.RowBg | ui.TableFlags.ScrollY | ui.TableFlags.SizingStretchProp | ui.TableFlags.BordersInnerV
  if ui.BeginTable("##combatLoadout", 6, flags, 0, 0) then
    ui.TableSetupColumn("Gem", ui.TableColumnFlags.WidthFixed, 36 * s)
    ui.TableSetupColumn("Desired Spec", ui.TableColumnFlags.WidthStretch, 1.1)
    ui.TableSetupColumn("Resolved", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableSetupColumn("Current Gem", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableSetupColumn("Status", ui.TableColumnFlags.WidthFixed, 108 * s)
    ui.TableSetupColumn("Diagnostic", ui.TableColumnFlags.WidthStretch, 1.2)
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
      ui.TableNextColumn()
      local diag = slot.diagnostic or ""
      if diag == "" and (slot.requiredLevel or 0) > 0 then
        diag = string.format("Requires level %d", slot.requiredLevel)
      end
      colored(diag ~= "" and C.danger or C.faint, diag)
    end
    ui.EndTable()
  end
end

local function drawClientsTab(s, h)
  if not API.commander or not API.commander.Status then
    colored(C.muted, "MQ Commander bridge unavailable.")
    return
  end
  local cst = API.commander.Status()
  if not cst or not cst.available then
    colored(C.muted, "Session bus commander offline.")
    return
  end

  local peers = cst.peers or {}

  -- Clickable Player/Client Switcher Tabs (never destroys or reconnects a client)
  ui.Spacing()
  colored(C.heading or C.accent, "Active Client Tabs")
  ui.SameLine()
  colored(C.muted, string.format("[%d connected]", #peers))
  ui.SameLine()
  colored(cst.busConnected and C.good or C.gold, cst.busConnected and " [Bus Connected]" or " [Local Only]")
  ui.SameLine()
  local ah, ahCh = ui.Checkbox("Auto-Headless Others##cmdAutoHeadless", cst.autoHeadlessOthers == true)
  if ahCh then runCommander("set_auto_headless", "", ah and "1" or "0") end
  if ui.IsItemHovered() then
    ui.SetTooltip("When clicking a player tab, demote previously active client to Headless/Background without reconnecting")
  end

  ui.Spacing()
  ui.Text("Groups:")
  ui.SameLine()
  local groupTabs = { { 0, "All" }, { 1, "Group 1" }, { 2, "Group 2" },
    { 3, "Group 3" }, { 4, "Group 4" } }
  for i, g in ipairs(groupTabs) do
    if i > 1 then ui.SameLine() end
    if ui.SmallButton(g[2] .. (selectedGroup == g[1] and " *" or "")) then
      selectedGroup = g[1]
    end
  end

  for idx, p in ipairs(peers) do
    if selectedGroup == 0 or peerGroup(p) == selectedGroup then
    -- Two switcher buttons per row keeps the control bar usable on narrow windows.
    if idx > 1 and idx % 2 == 0 then ui.SameLine() end
    ui.PushID("ptab_" .. tostring(idx))
    local assistColor, assistText, assistTip = assistIndicator(p)
    colored(assistColor, "●")
    if ui.IsItemHovered() then ui.SetTooltip(assistTip) end
    ui.SameLine()
    local label = (p.characterName ~= "" and p.characterName or p.sessionId)
      .. (p.classShort ~= "" and (" (" .. p.classShort .. ")") or "")
      .. (p.isLive and " [LIVE]" or " [BG]")
    if p.isLive then
      ui.PushStyleColor(ui.Col.Button, C.good[1], C.good[2], C.good[3], 0.35)
      ui.PushStyleColor(ui.Col.ButtonHovered, C.good[1], C.good[2], C.good[3], 0.55)
    end
    if ui.SmallButton(label) then
      runCommander("switch_tab", p.sessionId, "")
    end
    if p.isLive then ui.PopStyleColor(2) end
    if ui.IsItemHovered() then
      ui.SetTooltip(string.format("Click to make %s the live visible client (no reconnect)\nAssist: %s (%s)  |  Zone: %s  |  Combat: %s  |  Script: %s",
        p.characterName ~= "" and p.characterName or p.sessionId,
        assistText, assistTip,
        p.zoneShort ~= "" and p.zoneShort or "offline",
        p.combatState or "Idle",
        p.scriptState or "stopped"))
    end
    ui.PopID()
    end
  end

  -- Connected Clients Roster Table
  ui.Spacing()
  local tflags = ui.TableFlags.RowBg | ui.TableFlags.ScrollY | ui.TableFlags.SizingStretchProp | ui.TableFlags.BordersInnerV
  local rosterH = math.max(110, math.min(250, (h or 680) * 0.28))
  if ui.BeginTable("##commanderPeers", 9, tflags, 0, rosterH) then
    ui.TableSetupColumn("Character", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableSetupColumn("Class", ui.TableColumnFlags.WidthFixed, 44 * s)
    ui.TableSetupColumn("Lvl", ui.TableColumnFlags.WidthFixed, 34 * s)
    ui.TableSetupColumn("Zone", ui.TableColumnFlags.WidthStretch, 0.8)
    ui.TableSetupColumn("Conn", ui.TableColumnFlags.WidthFixed, 62 * s)
    ui.TableSetupColumn("Combat", ui.TableColumnFlags.WidthFixed, 70 * s)
    ui.TableSetupColumn("Script", ui.TableColumnFlags.WidthFixed, 76 * s)
    ui.TableSetupColumn("Mode", ui.TableColumnFlags.WidthFixed, 68 * s)
    ui.TableSetupColumn("Actions", ui.TableColumnFlags.WidthFixed, 236 * s)
    ui.TableHeadersRow()

    for idx, p in ipairs(peers) do
      if selectedGroup == 0 or peerGroup(p) == selectedGroup then
      ui.PushID("peer_" .. tostring(idx))
      ui.TableNextRow()
      ui.TableNextColumn()
      colored(p.isSelf and C.accent or C.text, p.characterName ~= "" and p.characterName or p.sessionId)
      ui.TableNextColumn()
      colored(C.muted, p.classShort or "")
      ui.TableNextColumn()
      colored(C.muted, tostring(p.level or 0))
      ui.TableNextColumn()
      colored(C.text, p.zoneShort ~= "" and p.zoneShort or "-")
      ui.TableNextColumn()
      colored(p.online and C.good or C.gold, p.connectionState ~= "" and p.connectionState or (p.online and "in_game" or "offline"))
      ui.TableNextColumn()
      local assistColor, assistText, assistTip = assistIndicator(p)
      colored(assistColor, "● " .. assistText)
      if ui.IsItemHovered() then ui.SetTooltip(assistTip) end
      ui.TableNextColumn()
      local sc = (p.scriptState == "error" and C.danger) or ((p.scriptState == "RUNNING" or p.scriptState == "running") and C.good or C.muted)
      colored(sc, (p.scriptName ~= "" and (p.scriptName .. ":") or "") .. (p.scriptState ~= "" and p.scriptState or "stopped"))
      ui.TableNextColumn()
      colored(p.isLive and C.good or C.faint, p.isLive and "Live" or "Headless")
      ui.TableNextColumn()
      if ui.SmallButton("Live") then runCommander("make_live", p.sessionId, "") end
      ui.SameLine()
      if ui.SmallButton("BG") then runCommander("make_headless", p.sessionId, "") end
      ui.SameLine()
      if ui.SmallButton("Follow") then
        local ok = runCommander("follow", p.sessionId, cst.characterName or "")
        if ok and p.isSelf then followInput = cst.characterName or "" end
      end
      ui.SameLine()
      if ui.SmallButton("Stop") then runCommander("stop", p.sessionId, "") end
      ui.SameLine()
      if ui.SmallButton("Restart") then runCommander("restart_script", p.sessionId, scriptArgInput) end
      ui.PopID()
      end
    end
    ui.EndTable()
  end

  -- MQ Commander Group Controls
  ui.Spacing()
  ui.SeparatorText("MQ Commander Group Controls")

  ui.Text("Target Scope:")
  ui.SameLine()
  ui.SetNextItemWidth(120 * s)
  if ui.BeginCombo("##cmdScope", cmdScope) then
    if ui.Selectable("all", cmdScope == "all") then cmdScope = "all" end
    if ui.Selectable("others", cmdScope == "others") then cmdScope = "others" end
    for _, p in ipairs(peers) do
      local id = p.sessionId or ""
      local disp = (p.characterName ~= "" and p.characterName or id) .. " (" .. id .. ")"
      if ui.Selectable(disp, cmdScope == id) then cmdScope = id end
    end
    ui.EndCombo()
  end

  ui.SameLine()
  ui.Text("Profile:")
  ui.SameLine()
  ui.SetNextItemWidth(130 * s)
  local profs = cst.availableProfiles or { "Main Tank", "Caster Group", "Melee Group", "Raid", "Solo" }
  if ui.BeginCombo("##cmdProfile", selectedProfile) then
    for _, pr in ipairs(profs) do
      if ui.Selectable(pr, pr == selectedProfile) then selectedProfile = pr end
    end
    ui.EndCombo()
  end
  ui.SameLine()
  if ui.SmallButton("Apply / Reload Profile") then
    runCommander("apply_profile", cmdScope, selectedProfile)
  end
  ui.SameLine()
  ui.SetNextItemWidth(85 * s)
  scriptArgInput = ui.InputTextWithHint("##cmdScript", "script", scriptArgInput, 32)

  -- Group action buttons row 1
  if ui.SmallButton("Follow Me") then runCommander("follow", cmdScope, cst.characterName or "") end
  ui.SameLine()
  if ui.SmallButton("Stop Follow") then runCommander("stop_follow", cmdScope, "") end
  ui.SameLine()
  if ui.SmallButton("Assist Me") then runCommander("assist", cmdScope, cst.characterName or "") end
  ui.SameLine()
  if ui.SmallButton("Attack On") then runCommander("attack_on", cmdScope, "") end
  ui.SameLine()
  if ui.SmallButton("Attack Off") then runCommander("attack_off", cmdScope, "") end
  ui.SameLine()
  if ui.SmallButton("Pause Script") then runCommander("pause_script", cmdScope, scriptArgInput) end
  ui.SameLine()
  if ui.SmallButton("Resume Script") then runCommander("resume_script", cmdScope, scriptArgInput) end

  -- Group action buttons row 2
  if ui.SmallButton("Reload Spells") then runCommander("reload_spells", cmdScope, "") end
  ui.SameLine()
  if ui.SmallButton("Camp") then runCommander("camp", cmdScope, "") end
  ui.SameLine()
  if ui.SmallButton("Return to Bind") then runCommander("return_to_bind", cmdScope, "") end
  ui.SameLine()
  if ui.SmallButton("TP To Me (Safe)") then runCommander("tp_to_me", cmdScope, cst.characterName or "") end
  if ui.IsItemHovered() then ui.SetTooltip("Uses only existing server-authorized safe teleport commands (never forges position packets)") end
  ui.SameLine()
  if ui.SmallButton("TP Here (Safe)") then runCommander("tp_here", cmdScope, "") end
  if ui.IsItemHovered() then ui.SetTooltip("Uses only existing server-authorized safe teleport commands (never forges position packets)") end
  ui.SameLine()
  if ui.SmallButton("Open RGMercs") then API.windows.Show("RGMercsWnd", true) end
  ui.SameLine()
  if ui.SmallButton("Special Functions") then API.windows.Show("SpecialFunctionsWnd", true) end
  if ui.IsItemHovered() then ui.SetTooltip("Open Special Functions (Teleport zone dropdown & PORTER (ALL))") end

  -- Command Acknowledgements / Failure / Timeout Log
  ui.Spacing()
  colored(C.heading or C.accent, "Command Acknowledgements & Diagnostics")
  ui.SameLine()
  if ui.SmallButton("Clear ACKs") then runCommander("clear_acks", "", "") end

  local acks = cst.trackedCommands or {}
  if ui.BeginTable("##commanderAcks", 5, tflags, 0, 0) then
    ui.TableSetupColumn("Seq", ui.TableColumnFlags.WidthFixed, 42 * s)
    ui.TableSetupColumn("Target", ui.TableColumnFlags.WidthFixed, 86 * s)
    ui.TableSetupColumn("Command", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableSetupColumn("Status", ui.TableColumnFlags.WidthFixed, 74 * s)
    ui.TableSetupColumn("Detail / Failure Reason", ui.TableColumnFlags.WidthStretch, 1.4)
    ui.TableHeadersRow()
    for i = #acks, 1, -1 do
      local a = acks[i]
      ui.TableNextRow()
      ui.TableNextColumn()
      colored(C.faint, tostring(a.seq or 0))
      ui.TableNextColumn()
      colored(C.muted, a.target or "")
      ui.TableNextColumn()
      colored(C.text, a.command or "")
      ui.TableNextColumn()
      colored(colorByName(ACK_COLORS[a.status] or "muted"), a.status or "")
      ui.TableNextColumn()
      colored((a.status == "Failed" or a.status == "TimedOut") and C.danger or C.text, a.detail or "")
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

local function drawCortexTab(s, h)
  if not API.commander or not API.commander.Status then
    colored(C.muted, "MQ Commander bridge unavailable.")
    return
  end
  local cst = API.commander.Status()
  if not cst or not cst.available then
    colored(C.muted, "Session bus commander offline.")
    return
  end
  local cx = cst.cortex
  if not cx then
    colored(C.muted, "Cortex status unavailable.")
    return
  end

  local available = cx.available == true
  local running = cx.running == true
  local stateText = not available and "Unavailable" or (running and "Enabled (Running)" or "Disabled (Stopped)")
  local stateColor = not available and C.muted or (running and C.good or C.gold)

  local rawMode = cx.mode or "DISABLED"
  local modeText = CORTEX_MODE_LABELS[rawMode] or rawMode
  local modeColor = rawMode == "CORTEX_ASSISTED" and C.good
    or (rawMode == "CORTEX_OBSERVE" and C.accent
    or (rawMode == "CORTEX_AUTONOMOUS" and C.danger or C.muted))

  local healthy = cx.inferenceHealthy == true
  local healthText = healthy and "Healthy" or "Unhealthy"
  local healthColor = healthy and C.good or C.danger
  local modelText = (cx.modelName and cx.modelName ~= "") and cx.modelName or "(unset)"
  local endpointText = (cx.endpoint and cx.endpoint ~= "") and cx.endpoint or "(unset)"

  local chars = cx.characters or {}
  local proposals = cx.proposals or {}
  local inFlight = 0
  for _, ch in ipairs(chars) do
    if ch.brainStatus == "THINKING" then
      inFlight = inFlight + 1
    end
  end

  ui.Spacing()
  colored(C.heading or C.accent, "Cortex Status (Read-Only)")
  ui.SameLine()
  colored(stateColor, "[" .. stateText .. "]")
  ui.SameLine()
  colored(modeColor, "Mode: " .. modeText)
  ui.SameLine()
  colored(healthColor, "Inference: " .. healthText)

  if cx.emergencyStop then
    colored(C.danger, "EMERGENCY STOP ACTIVE: Cortex inference paused and proposals held.")
  end

  colored(C.muted, string.format("Endpoint: %s  |  Model: %s", endpointText, modelText))
  colored(C.muted, string.format(
    "Counters — Queued: %d  |  In-Flight: %d  |  Completed: %d  |  Errors/Dropped: %d  |  Requests: %d  |  Roster: %d/%d",
    cx.pendingQueue or 0,
    inFlight,
    cx.totalDecisions or 0,
    cx.totalErrors or 0,
    cx.totalRequests or 0,
    cx.onlineCount or 0,
    cx.rosterSize or 0))

  ui.Spacing()
  colored(C.heading or C.accent, string.format("Per-Character Cortex Status (%d)", #chars))
  local tflags = ui.TableFlags.RowBg | ui.TableFlags.ScrollY | ui.TableFlags.SizingStretchProp | ui.TableFlags.BordersInnerV
  local charH = math.max(95 * s, math.min(200 * s, (h or 680) * 0.25))
  if #chars == 0 then
    colored(C.faint, "No characters observed by Cortex.")
  elseif ui.BeginTable("##cortexCharacters", 7, tflags, 0, charH) then
    ui.TableSetupColumn("Character", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableSetupColumn("State", ui.TableColumnFlags.WidthFixed, 68 * s)
    ui.TableSetupColumn("Brain", ui.TableColumnFlags.WidthFixed, 108 * s)
    ui.TableSetupColumn("Mode", ui.TableColumnFlags.WidthFixed, 68 * s)
    ui.TableSetupColumn("Objective", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableSetupColumn("Decision / Intention", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableSetupColumn("Proposal / Error", ui.TableColumnFlags.WidthStretch, 1.1)
    ui.TableHeadersRow()
    for idx, ch in ipairs(chars) do
      ui.PushID("cxchar_" .. tostring(idx))
      ui.TableNextRow()
      ui.TableNextColumn()
      local charLabel = (ch.characterName and ch.characterName ~= "") and ch.characterName or (ch.key or "")
      colored(C.text, charLabel)
      ui.TableNextColumn()
      colored(ch.enabled and C.good or C.muted, ch.enabled and "Enabled" or "Disabled")
      ui.TableNextColumn()
      local bStatus = ch.brainStatus or "DISABLED"
      colored(colorByName(CORTEX_STATUS_COLORS[bStatus] or "muted"), bStatus)
      ui.TableNextColumn()
      colored(C.muted, CORTEX_MODE_LABELS[ch.mode or ""] or (ch.mode or ""))
      ui.TableNextColumn()
      colored(C.text, (ch.objective and ch.objective ~= "") and ch.objective or "-")
      ui.TableNextColumn()
      local decText = (ch.hasDecision and ch.decisionAction and ch.decisionAction ~= "")
        and ch.decisionAction
        or ((ch.intention and ch.intention ~= "" and ch.intention ~= "none") and ch.intention or "-")
      colored(ch.hasDecision and C.good or C.muted, decText)
      ui.TableNextColumn()
      if ch.lastError and ch.lastError ~= "" then
        colored(C.danger, string.format("Err(%d): %s", ch.consecutiveErrors or 0, ch.lastError))
      elseif ch.proposalPending then
        colored(C.gold, "Pending confirmation")
      else
        colored(C.faint, "-")
      end
      ui.PopID()
    end
    ui.EndTable()
  end

  ui.Spacing()
  colored(C.heading or C.accent, string.format("Pending Assisted Proposals (%d)", #proposals))
  ui.SameLine()
  colored(C.gold, "Pending confirmation only — read-only, not auto-executed")
  if cx.emergencyStop then
    colored(C.danger, "Emergency stop active: all pending proposals are suppressed.")
  elseif #proposals == 0 then
    colored(C.faint, "No pending proposals awaiting confirmation.")
  elseif ui.BeginTable("##cortexProposals", 6, tflags, 0, 0) then
    ui.TableSetupColumn("Character", ui.TableColumnFlags.WidthFixed, 100 * s)
    ui.TableSetupColumn("Proposed Action", ui.TableColumnFlags.WidthFixed, 120 * s)
    ui.TableSetupColumn("Status", ui.TableColumnFlags.WidthFixed, 130 * s)
    ui.TableSetupColumn("Reason Code", ui.TableColumnFlags.WidthFixed, 120 * s)
    ui.TableSetupColumn("Explanation", ui.TableColumnFlags.WidthStretch, 1.3)
    ui.TableSetupColumn("Arbitration", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableHeadersRow()
    for idx, p in ipairs(proposals) do
      ui.PushID("cxprop_" .. tostring(idx))
      ui.TableNextRow()
      ui.TableNextColumn()
      colored(C.text, (p.characterName and p.characterName ~= "") and p.characterName or (p.key or ""))
      ui.TableNextColumn()
      colored(C.accent, p.action or "none")
      ui.TableNextColumn()
      local confText = (p.awaitingConfirmation ~= false and not p.executed)
        and "Pending confirmation"
        or (p.executed and "Executed" or "Held")
      colored(C.gold, confText)
      ui.TableNextColumn()
      colored(C.muted, p.reasonCode or "")
      ui.TableNextColumn()
      colored(C.text, p.explanation or "")
      ui.TableNextColumn()
      local arb = p.arbitrationOwner or ""
      if p.arbitrationReason and p.arbitrationReason ~= "" then
        arb = arb ~= "" and (arb .. ": " .. p.arbitrationReason) or p.arbitrationReason
      end
      colored(C.muted, arb)
      ui.PopID()
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
    lastStatusFollow = st.followTarget or ""
  elseif (st.followTarget or "") ~= lastStatusFollow then
    -- A commander action may update this client while the window is hidden.
    -- Adopt the new status only when the user has not started editing the
    -- field, so tab/window switches do not leave a stale value displayed.
    if followInput == lastStatusFollow then
      followInput = st.followTarget or ""
    end
    lastStatusFollow = st.followTarget or ""
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
    if ui.BeginTabItem("Players / Clients") then
      drawClientsTab(s, h)
      ui.EndTabItem()
    end
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
    if ui.BeginTabItem("Cortex") then
      drawCortexTab(s, h)
      ui.EndTabItem()
    end
    ui.EndTabBar()
  end
end

ui.window("Combat Controller", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Hailstone Combat", WINDOW_OPTS, drawWindow)
end)
