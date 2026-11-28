-- Hailstone Special Functions (SpecialFunctionsWnd): Teleport menu with zone filter/dropdown
-- and PORTER (ALL) multi-session dispatch (/specialfunctions, /special, /porterall, /porter).

local API = hs.UI
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color

local WINDOW = "SpecialFunctionsWnd"
local WINDOW_OPTS = {
  size = function() return 920, 680 end,
  resizable = { 640, 480 },
}

local epicGroupFilter = "All"
local epicClassFilter = "All"
local epicStateFilter = "All"
local epicCharSearch = ""
local epicSelectedKey = ""
local epicManualStage = ""
local epicManualReason = "Manual stage completion"
local epicPriorityChar = ""
local epicPriorityVal = "80"
local epicReserveItem = ""
local epicReserveChar = ""
local epicImportJsonBuf = ""
local epicShowImportModal = false
local epicMsg = ""
local epicOk = true

local ACK_COLORS = {
  Pending = "gold",
  Acked = "good",
  Partial = "gold",
  Failed = "danger",
  TimedOut = "danger",
}

local PORTER_MODES = {
  { id = "auto", label = "Auto (porter_go.lua -> Native NPC)" },
  { id = "script", label = "Script Only (porter_go.lua)" },
  { id = "native", label = "Native Only (Target Porter + /say)" },
}

local QUICK_ZONES = {
  "poknowledge",
  "guildlobby",
  "nexus",
  "bazaar",
  "crescent",
  "qeynos2",
  "freportw",
  "gfaydark",
}

local cachedZones = nil
local zoneFilter = ""
local selectedShort = "poknowledge"
local selectedLong = "The Plane of Knowledge"
local customZoneInput = ""
local driverAnchor = ""
local driverInitialized = false
local selectedScope = "all"
local porterMode = "auto"
local porterNpcName = "Porter"
local lastMsg = ""
local lastOk = true
local lastPorterDest = ""

local function colored(c, text)
  local col = c or C.text
  ui.TextColored(col[1], col[2], col[3], 1, text)
end

local function colorByName(name)
  return C[name] or C.text
end

local function trim(s)
  if type(s) ~= "string" then return "" end
  return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function loadZones(force)
  if cachedZones ~= nil and not force then return cachedZones end
  local list = {}
  if API.commander and API.commander.Zones then
    local ok, res = pcall(API.commander.Zones)
    if ok and type(res) == "table" then
      list = res
    end
  end
  cachedZones = list
  -- Keep selectedLong in sync with loaded table
  if selectedShort ~= "" then
    local want = selectedShort:lower()
    for _, z in ipairs(cachedZones) do
      if z.shortName and z.shortName:lower() == want then
        selectedShort = z.shortName
        selectedLong = z.longName or z.shortName
        break
      end
    end
  elseif #cachedZones > 0 then
    selectedShort = cachedZones[1].shortName or ""
    selectedLong = cachedZones[1].longName or selectedShort
  end
  return cachedZones
end

local function selectZone(z)
  if not z then return end
  selectedShort = z.shortName or ""
  selectedLong = z.longName or selectedShort
  customZoneInput = ""
end

local function activeDestination()
  local custom = trim(customZoneInput):lower()
  if custom ~= "" then return custom end
  return trim(selectedShort):lower()
end

local function buildPorterArg(dest)
  local parts = { dest }
  local drv = trim(driverAnchor)
  if drv ~= "" then
    parts[#parts + 1] = "driver=" .. drv
  end
  if porterMode ~= "" and porterMode ~= "auto" then
    parts[#parts + 1] = "mode=" .. porterMode
  end
  local npc = trim(porterNpcName)
  if npc ~= "" and npc:lower() ~= "porter" then
    parts[#parts + 1] = "npc=" .. npc
  end
  return table.concat(parts, " ")
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

local function triggerPorterAll()
  local dest = activeDestination()
  if dest == "" then
    lastOk = false
    lastMsg = "Select or enter a destination zone short name first."
    return
  end
  lastPorterDest = dest
  runCommander("porter_all", "all", buildPorterArg(dest))
end

local function triggerPorterScoped()
  local dest = activeDestination()
  if dest == "" then
    lastOk = false
    lastMsg = "Select or enter a destination zone short name first."
    return
  end
  lastPorterDest = dest
  runCommander("porter", selectedScope, buildPorterArg(dest))
end

local function modeLabel(id)
  for _, m in ipairs(PORTER_MODES) do
    if m.id == id then return m.label end
  end
  return PORTER_MODES[1].label
end

local function drawPorterTab()
  local s = T.scale or 1
  local zones = loadZones(false)
  local cst = (API.commander and API.commander.Status) and API.commander.Status() or {}
  local peers = cst.peers or {}
  local selfName = cst.characterName or ""

  if not driverInitialized and selfName ~= "" then
    driverAnchor = selfName
    driverInitialized = true
  end

  -- Header status strip
  colored(C.heading or C.accent, "Special Functions: Teleport & Porter")
  ui.SameLine()
  colored(C.muted, string.format("(%d sessions | Bus: %s)",
    #peers, cst.busConnected and "Connected" or "Standalone"))
  ui.SameLine()
  if ui.SmallButton("Reload Zones") then
    loadZones(true)
    lastOk = true
    lastMsg = string.format("Reloaded %d zones", #cachedZones)
  end
  ui.SameLine()
  if ui.SmallButton("Combat Controller") then
    API.windows.Show("CombatWnd", true)
  end

  if lastMsg ~= "" then
    colored(lastOk and C.good or C.danger, lastMsg)
  end

  ui.Separator()

  -- Filter row
  local fltLower = trim(zoneFilter):lower()
  local filtered = {}
  local iniCount = 0
  for _, z in ipairs(zones) do
    if z.source == "zones_ini" then iniCount = iniCount + 1 end
    local sn = z.shortName or ""
    local ln = z.longName or ""
    if fltLower == "" or sn:lower():find(fltLower, 1, true) or ln:lower():find(fltLower, 1, true) then
      filtered[#filtered + 1] = z
    end
  end

  ui.Text("Zone Filter:")
  ui.SameLine()
  ui.SetNextItemWidth(220 * s)
  local newFilter = ui.InputTextWithHint("##sfZoneFilter", "Search short or long zone name...", zoneFilter, 64)
  zoneFilter = newFilter
  ui.SameLine()
  if ui.SmallButton("Clear##sfClearFlt") then zoneFilter = "" end
  ui.SameLine()
  colored(C.muted, string.format("Showing %d / %d zones%s",
    #filtered, #zones, iniCount > 0 and string.format(" (%d Zones.ini)", iniCount) or ""))

  -- Destination Zone Dropdown
  ui.Text("Destination:")
  ui.SameLine()
  ui.SetNextItemWidth(340 * s)
  local comboPreview = selectedShort ~= ""
    and (selectedLong ~= "" and selectedLong ~= selectedShort
      and (selectedShort .. "  -  " .. selectedLong)
      or selectedShort)
    or "(Select a zone)"
  if ui.BeginCombo("##sfZoneDropdown", comboPreview) then
    for i = 1, #filtered do
      local z = filtered[i]
      local sn = z.shortName or ""
      local ln = z.longName or sn
      local label = (ln ~= "" and ln ~= sn)
        and string.format("%s  -  %s%s##z%d", sn, ln, z.source == "zones_ini" and " [INI]" or "", i)
        or string.format("%s%s##z%d", sn, z.source == "zones_ini" and " [INI]" or "", i)
      local isSel = (sn:lower() == selectedShort:lower())
      if ui.Selectable(label, isSel) then
        selectZone(z)
      end
    end
    ui.EndCombo()
  end

  ui.SameLine()
  ui.SetNextItemWidth(155 * s)
  local customTxt, customEnter = ui.InputTextWithHint("##sfCustomZone", "Custom short name", customZoneInput, 48,
    ui.InputTextFlags.EnterReturnsTrue)
  customZoneInput = customTxt
  if ui.IsItemHovered() then
    ui.SetTooltip("Optional manual zone short name override (press Enter to run PORTER (ALL))")
  end
  if customEnter then triggerPorterAll() end

  -- Quick hub shortcuts
  ui.Text("Quick Hubs:")
  for i, qz in ipairs(QUICK_ZONES) do
    ui.SameLine()
    if ui.SmallButton(qz .. "##qh" .. i) then
      local found = false
      for _, z in ipairs(zones) do
        if z.shortName and z.shortName:lower() == qz then
          selectZone(z)
          found = true
          break
        end
      end
      if not found then
        selectedShort = qz
        selectedLong = qz
        customZoneInput = ""
      end
    end
  end

  -- Compact scrollable zone picker matching MQ Commander 5 list workflow
  local listFlags = ui.TableFlags.RowBg | ui.TableFlags.BordersInnerV | ui.TableFlags.ScrollY | ui.TableFlags.SizingStretchProp
  if ui.BeginTable("##sfZoneList", 3, listFlags, 0, 128 * s) then
    ui.TableSetupColumn("Short Name", ui.TableColumnFlags.WidthFixed, 135 * s)
    ui.TableSetupColumn("Zone Name", ui.TableColumnFlags.WidthStretch, 1.5)
    ui.TableSetupColumn("Source", ui.TableColumnFlags.WidthFixed, 76 * s)
    ui.TableHeadersRow()
    local cap = math.min(#filtered, 250)
    for i = 1, cap do
      local z = filtered[i]
      local sn = z.shortName or ""
      local ln = z.longName or sn
      local isSel = (sn:lower() == activeDestination())
      ui.TableNextRow()
      ui.TableNextColumn()
      if ui.Selectable(sn .. "##row" .. i, isSel, ui.SelectableFlags.SpanAllColumns) then
        selectZone(z)
      end
      ui.TableNextColumn()
      colored(isSel and (C.heading or C.accent) or C.text, ln)
      ui.TableNextColumn()
      colored(z.source == "zones_ini" and C.gold or C.faint, z.source == "zones_ini" and "Zones.ini" or "Built-in")
    end
    ui.EndTable()
  end

  ui.Separator()

  -- Porter Options Row: Driver anchor + Mode + Scope
  ui.Text("Driver Anchor:")
  ui.SameLine()
  ui.SetNextItemWidth(145 * s)
  local drvPreview = trim(driverAnchor) ~= "" and driverAnchor or "(None)"
  if ui.BeginCombo("##sfDriverAnchor", drvPreview) then
    if ui.Selectable("(None)##drvNone", trim(driverAnchor) == "") then
      driverAnchor = ""
    end
    for i, p in ipairs(peers) do
      local name = p.characterName or p.sessionId or ""
      if name ~= "" then
        if ui.Selectable(name .. "##drv" .. i, driverAnchor == name) then
          driverAnchor = name
        end
      end
    end
    ui.EndCombo()
  end
  if ui.IsItemHovered() then
    ui.SetTooltip("Passed as driver=<name> so toons resume follow/chase after zoning")
  end

  ui.SameLine()
  ui.Text("Mode:")
  ui.SameLine()
  ui.SetNextItemWidth(220 * s)
  if ui.BeginCombo("##sfPorterMode", modeLabel(porterMode)) then
    for _, m in ipairs(PORTER_MODES) do
      if ui.Selectable(m.label, m.id == porterMode) then
        porterMode = m.id
      end
    end
    ui.EndCombo()
  end

  ui.SameLine()
  ui.SetNextItemWidth(90 * s)
  local npcTxt = ui.InputTextWithHint("##sfPorterNpc", "Porter NPC", porterNpcName, 32)
  porterNpcName = npcTxt
  if ui.IsItemHovered() then
    ui.SetTooltip("NPC name to target in Native mode (default: Porter)")
  end

  -- Destination summary & PORTER (ALL) primary button bar
  local dest = activeDestination()
  ui.Spacing()
  colored(C.muted, "Destination short name: ")
  ui.SameLine()
  colored(dest ~= "" and C.good or C.danger, dest ~= "" and dest or "(none selected)")
  if trim(driverAnchor) ~= "" then
    ui.SameLine()
    colored(C.muted, "  |  Driver: ")
    ui.SameLine()
    colored(C.text, driverAnchor)
  end

  -- Primary PORTER (ALL) button
  if ui.Button("PORTER (ALL)##sfPorterAll") then
    triggerPorterAll()
  end
  if ui.IsItemHovered() then
    ui.SetTooltip("Dispatch Porter to EVERY connected session across all groups (ignores single-group filter)")
  end

  ui.SameLine()
  ui.SetNextItemWidth(135 * s)
  local scopePreview = selectedScope == "all" and "All Sessions" or selectedScope
  if ui.BeginCombo("##sfScope", scopePreview) then
    if ui.Selectable("All Sessions", selectedScope == "all") then selectedScope = "all" end
    for i, p in ipairs(peers) do
      local key = p.sessionId or ""
      local lbl = (p.characterName ~= "" and p.characterName or key) .. "##sc" .. i
      if ui.Selectable(lbl, selectedScope == key) then selectedScope = key end
    end
    ui.EndCombo()
  end
  ui.SameLine()
  if ui.Button("Porter Target##sfPorterOne") then
    triggerPorterScoped()
  end

  -- Utility teleport / follow actions row
  if ui.SmallButton("Return to Bind (All)") then runCommander("return_to_bind", "all", "") end
  ui.SameLine()
  if ui.SmallButton("TP To Me (Safe)") then
    runCommander("tp_to_me", "all", trim(driverAnchor) ~= "" and driverAnchor or selfName)
  end
  ui.SameLine()
  if ui.SmallButton("TP Here (Safe)") then runCommander("tp_here", "all", "") end
  ui.SameLine()
  if ui.SmallButton("Follow Driver (All)") then
    runCommander("follow", "all", trim(driverAnchor) ~= "" and driverAnchor or selfName)
  end
  ui.SameLine()
  if ui.SmallButton("Stop Follow (All)") then runCommander("stop_follow", "all", "") end
  ui.SameLine()
  if ui.SmallButton("Hail Target (All)") then runCommander("hail_all", "", "") end
  ui.SameLine()
  if ui.SmallButton("Corpse Hotbuttons (All)") then runCommander("corpse_hotbuttons_all", "", "") end
  ui.SameLine()
  if ui.SmallButton("Mount (All)") then runCommander("mount_all", "", "") end
  ui.SameLine()
  if ui.SmallButton("Unmount (All)") then runCommander("unmount_all", "", "") end

  ui.Separator()

  -- Live Peer Zone Verification Table
  colored(C.heading or C.accent, "Connected Sessions & Zone Status")
  if lastPorterDest ~= "" then
    ui.SameLine()
    colored(C.muted, "(Tracking destination: " .. lastPorterDest .. ")")
  end

  local tflags = ui.TableFlags.RowBg | ui.TableFlags.BordersInnerV | ui.TableFlags.ScrollY | ui.TableFlags.SizingStretchProp
  if ui.BeginTable("##sfPeersTable", 5, tflags, 0, 118 * s) then
    ui.TableSetupColumn("Character", ui.TableColumnFlags.WidthFixed, 120 * s)
    ui.TableSetupColumn("Current Zone", ui.TableColumnFlags.WidthFixed, 125 * s)
    ui.TableSetupColumn("Porter Status", ui.TableColumnFlags.WidthFixed, 100 * s)
    ui.TableSetupColumn("State", ui.TableColumnFlags.WidthFixed, 90 * s)
    ui.TableSetupColumn("Follow / Driver", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableHeadersRow()
    for _, p in ipairs(peers) do
      local zone = p.zoneShort or ""
      local matchDest = lastPorterDest ~= "" and zone:lower() == lastPorterDest:lower()
      ui.TableNextRow()
      ui.TableNextColumn()
      colored(p.isSelf and (C.heading or C.accent) or C.text,
        (p.characterName ~= "" and p.characterName or p.sessionId or "?") .. (p.isSelf and " *" or ""))
      ui.TableNextColumn()
      colored(matchDest and C.good or C.text, zone ~= "" and zone or "-")
      ui.TableNextColumn()
      if lastPorterDest == "" then
        colored(C.faint, "Ready")
      elseif matchDest then
        colored(C.good, "ZONED")
      else
        colored(C.gold, "Waiting")
      end
      ui.TableNextColumn()
      colored(p.online and C.good or C.muted, p.online and (p.isLive and "Live" or "Headless") or "Offline")
      ui.TableNextColumn()
      colored(C.muted, p.followTarget ~= "" and p.followTarget or "-")
    end
    ui.EndTable()
  end

  -- Recent Porter / Teleport ACKs
  ui.Spacing()
  colored(C.heading or C.accent, "Porter & Teleport Acknowledgements")
  ui.SameLine()
  if ui.SmallButton("Clear ACKs##sfClearAcks") then runCommander("clear_acks", "", "") end

  local acks = cst.trackedCommands or {}
  if ui.BeginTable("##sfAcksTable", 4, tflags, 0, 0) then
    ui.TableSetupColumn("Seq", ui.TableColumnFlags.WidthFixed, 42 * s)
    ui.TableSetupColumn("Command", ui.TableColumnFlags.WidthStretch, 1.1)
    ui.TableSetupColumn("Status", ui.TableColumnFlags.WidthFixed, 76 * s)
    ui.TableSetupColumn("Detail / Result", ui.TableColumnFlags.WidthStretch, 1.5)
    ui.TableHeadersRow()
    for i = #acks, 1, -1 do
      local a = acks[i]
      ui.TableNextRow()
      ui.TableNextColumn()
      colored(C.faint, tostring(a.seq or 0))
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

local STATE_COLORS = {
  ["Completed"] = "good",
  ["Completed with upgrade available"] = "good",
  ["Ready for completion"] = "good",
  ["Awaiting turn-in"] = "gold",
  ["In progress"] = "accent",
  ["Awaiting item"] = "gold",
  ["Not started"] = "muted",
  ["Unknown"] = "faint",
  ["Blocked"] = "danger",
  ["Manual override"] = "accent",
}

local CONFLICT_RULES = {
  "ActiveStageFirst",
  "HighestPriorityCharacter",
  "MainCharacterFirst",
  "HighestStageProgress",
  "GroupPriorityThenCharacter",
  "ClassPriority",
  "ManualApproval",
}

local function runEpicDo(action, a1, a2, a3)
  if not API.epic or not API.epic.Do then
    epicOk = false
    epicMsg = "Epic API unavailable"
    return false
  end
  local ok, msg = API.epic.Do(action, a1 or "", a2 or "", a3 or "")
  epicOk = ok == true
  if msg and msg ~= "" then epicMsg = msg end
  return ok
end

local function drawEpicTab()
  local s = T.scale or 1
  if not API.epic or not API.epic.Status then
    colored(C.danger, "Epic Quest & Loot system is not available in this session.")
    return
  end

  local st = API.epic.Status() or {}
  if not st.available then
    colored(C.muted, "Epic Quest & Loot manager offline.")
    return
  end

  local pol = st.policy or {}
  local profiles = st.profiles or {}
  local defs = st.definitions or {}

  -- Top Control Strip
  if pol.emergencyDisable then
    if ui.SmallButton("RESUME EPIC LOOT (Emergency Off)##epEstop") then
      runEpicDo("emergency_disable", "false")
    end
    ui.SameLine()
    colored(C.danger, "EMERGENCY DISABLE ACTIVE")
  else
    if ui.SmallButton("EMERGENCY DISABLE##epEstop") then
      runEpicDo("emergency_disable", "true")
    end
  end
  ui.SameLine()
  local dryRun = pol.dryRun == true
  local newDry, dryChanged = ui.Checkbox("Dry-Run Mode##epDryRun", dryRun)
  if dryChanged then
    runEpicDo("dry_run", newDry and "true" or "false")
  end
  ui.SameLine()
  if ui.SmallButton("Scan & Reconcile All##epScan") then
    runEpicDo("scan_all")
  end
  ui.SameLine()
  if ui.SmallButton("Backup DB##epBackup") then
    runEpicDo("backup")
  end
  ui.SameLine()
  if ui.SmallButton("Restore DB##epRestore") then
    runEpicDo("restore")
  end
  ui.SameLine()
  if ui.SmallButton("Export JSON##epExport") and API.epic.Export then
    epicImportJsonBuf = API.epic.Export() or ""
    epicShowImportModal = true
    epicOk = true
    epicMsg = string.format("Exported %d bytes to JSON buffer below.", #epicImportJsonBuf)
  end
  ui.SameLine()
  if ui.SmallButton("Import JSON##epImportToggle") then
    epicShowImportModal = not epicShowImportModal
  end

  if epicMsg ~= "" then
    colored(epicOk and C.good or C.danger, epicMsg)
  end

  if epicShowImportModal then
    ui.Separator()
    colored(C.heading or C.accent, "Epic Database JSON Import / Export")
    epicImportJsonBuf = ui.InputTextMultiline("##epJsonBuf", epicImportJsonBuf, 65536, -1, 110 * s)
    if ui.SmallButton("Import (Merge)##epImpMerge") and API.epic.Import then
      local ok, msg = API.epic.Import(epicImportJsonBuf, true)
      epicOk = ok == true
      epicMsg = msg or ""
    end
    ui.SameLine()
    if ui.SmallButton("Import (Replace)##epImpRepl") and API.epic.Import then
      local ok, msg = API.epic.Import(epicImportJsonBuf, false)
      epicOk = ok == true
      epicMsg = msg or ""
    end
    ui.SameLine()
    if ui.SmallButton("Close##epImpClose") then
      epicShowImportModal = false
    end
  end

  ui.Separator()

  if ui.BeginTabBar("##epicSubTabs") then
    -- Sub-tab 1: Characters (24-Box View)
    if ui.BeginTabItem("Characters (" .. tostring(#profiles) .. ")##epCharsTab") then
      local groups = { "All" }
      local groupSeen = {}
      local classes = { "All" }
      local classSeen = {}
      for _, p in ipairs(profiles) do
        local g = p.groupName or ""
        if g ~= "" and not groupSeen[g] then groupSeen[g] = true; groups[#groups + 1] = g end
        local c = p.classShort or ""
        if c ~= "" and not classSeen[c] then classSeen[c] = true; classes[#classes + 1] = c end
      end

      ui.Text("Filter:")
      ui.SameLine()
      ui.SetNextItemWidth(140 * s)
      epicCharSearch = ui.InputTextWithHint("##epSearch", "Character...", epicCharSearch, 48)
      ui.SameLine()
      ui.SetNextItemWidth(110 * s)
      if ui.BeginCombo("##epGrpFlt", "Group: " .. epicGroupFilter) then
        for _, g in ipairs(groups) do
          if ui.Selectable(g .. "##eg" .. g, epicGroupFilter == g) then epicGroupFilter = g end
        end
        ui.EndCombo()
      end
      ui.SameLine()
      ui.SetNextItemWidth(95 * s)
      if ui.BeginCombo("##epClsFlt", "Class: " .. epicClassFilter) then
        for _, c in ipairs(classes) do
          if ui.Selectable(c .. "##ec" .. c, epicClassFilter == c) then epicClassFilter = c end
        end
        ui.EndCombo()
      end

      local filtered = {}
      local searchLower = trim(epicCharSearch):lower()
      local selectedProfile = nil
      for _, p in ipairs(profiles) do
        local okG = (epicGroupFilter == "All" or p.groupName == epicGroupFilter)
        local okC = (epicClassFilter == "All" or p.classShort == epicClassFilter)
        local okS = (searchLower == "" or (p.character or ""):lower():find(searchLower, 1, true))
        if okG and okC and okS then
          filtered[#filtered + 1] = p
        end
        if p.storageKey == epicSelectedKey then
          selectedProfile = p
        end
      end
      if not selectedProfile and #filtered > 0 then
        selectedProfile = filtered[1]
        epicSelectedKey = selectedProfile.storageKey or ""
      end

      local tflags = ui.TableFlags.RowBg | ui.TableFlags.BordersInnerV | ui.TableFlags.ScrollY | ui.TableFlags.SizingStretchProp
      if ui.BeginTable("##epCharsTbl", 8, tflags, 0, 210 * s) then
        ui.TableSetupColumn("Character", ui.TableColumnFlags.WidthFixed, 105 * s)
        ui.TableSetupColumn("Cls", ui.TableColumnFlags.WidthFixed, 38 * s)
        ui.TableSetupColumn("Server", ui.TableColumnFlags.WidthFixed, 85 * s)
        ui.TableSetupColumn("Group", ui.TableColumnFlags.WidthFixed, 80 * s)
        ui.TableSetupColumn("Variant", ui.TableColumnFlags.WidthFixed, 85 * s)
        ui.TableSetupColumn("Stage", ui.TableColumnFlags.WidthFixed, 95 * s)
        ui.TableSetupColumn("Prog", ui.TableColumnFlags.WidthFixed, 48 * s)
        ui.TableSetupColumn("Status", ui.TableColumnFlags.WidthStretch, 1.0)
        ui.TableHeadersRow()

        for i, p in ipairs(filtered) do
          local isSel = (p.storageKey == epicSelectedKey)
          ui.TableNextRow()
          ui.TableNextColumn()
          if ui.Selectable((p.character or "?") .. "##epRow" .. i, isSel, ui.SelectableFlags.SpanAllColumns) then
            epicSelectedKey = p.storageKey or ""
            selectedProfile = p
          end
          ui.TableNextColumn()
          colored(C.text, p.classShort or "-")
          ui.TableNextColumn()
          colored(C.muted, p.server or "-")
          ui.TableNextColumn()
          colored(C.muted, (p.groupName and p.groupName ~= "") and p.groupName or "-")
          ui.TableNextColumn()
          colored(C.text, p.epicVariant or "-")
          ui.TableNextColumn()
          colored(C.text, p.currentStageId or "-")
          ui.TableNextColumn()
          colored(C.text, string.format("%.0f%%", p.progressPct or 0))
          ui.TableNextColumn()
          local stText = p.needsReview and ("REVIEW: " .. (p.state or "Unknown")) or (p.state or "Unknown")
          local stColor = p.needsReview and "danger" or (STATE_COLORS[p.state or ""] or "text")
          colored(colorByName(stColor), stText)
        end
        ui.EndTable()
      end

      if selectedProfile then
        ui.Separator()
        local p = selectedProfile
        colored(C.heading or C.accent, string.format("%s (%s | %s | %s)",
          p.character or "?", p.classShort or "?", p.server or "?", p.epicVariant or "?"))
        ui.SameLine()
        colored(C.muted, string.format("Source: %s | Confidence: %s | Stages: %d/%d",
          p.source or "Unknown", p.confidence or "None", p.completedStageCount or 0, p.totalStages or 0))

        if p.needsReview then
          colored(C.danger, "Review Needed: " .. (p.reviewReason or "Inconsistency detected"))
          ui.SameLine()
          if ui.SmallButton("Clear Review##epClrRev") then
            local keyStr = string.format("%s|%s|%s|%s", p.server or "", p.character or "", p.classShort or "", p.epicVariant or "")
            runEpicDo("clear_review", keyStr)
          end
        end

        -- Manual Override Controls
        local keyStr = string.format("%s|%s|%s|%s", p.server or "", p.character or "", p.classShort or "", p.epicVariant or "")
        ui.SetNextItemWidth(130 * s)
        epicManualStage = ui.InputTextWithHint("##epManStg", "Stage ID (e.g. stage_1)", epicManualStage, 64)
        ui.SameLine()
        if ui.SmallButton("Mark Stage Complete##epMarkStg") then
          local stg = trim(epicManualStage) ~= "" and trim(epicManualStage) or (p.currentStageId or "")
          runEpicDo("mark_stage", keyStr, stg, epicManualReason)
        end
        ui.SameLine()
        if ui.SmallButton("Rollback Manual Change##epRollback") then
          runEpicDo("rollback", keyStr)
        end

        -- Variant Switcher
        ui.SameLine()
        ui.SetNextItemWidth(130 * s)
        if ui.BeginCombo("##epVarPick", "Variant: " .. (p.epicVariant or "")) then
          for _, d in ipairs(defs) do
            local matchCls = false
            for _, c in ipairs(d.classes or {}) do
              if c == p.classShort or c == "ALL" then matchCls = true; break end
            end
            if matchCls then
              if ui.Selectable((d.variantName or d.epicId) .. "##ev" .. d.epicId, d.epicId == p.epicId) then
                local baseKey = string.format("%s|%s|%s", p.server or "", p.character or "", p.classShort or "")
                runEpicDo("set_variant", baseKey, d.variantName or d.epicId)
              end
            end
          end
          ui.EndCombo()
        end

        -- Required / Owned / Missing Items table for selected character
        local reqs = p.requiredItems or {}
        if ui.BeginTable("##epCharItems", 5, tflags, 0, 110 * s) then
          ui.TableSetupColumn("Item Name", ui.TableColumnFlags.WidthStretch, 1.4)
          ui.TableSetupColumn("Item ID", ui.TableColumnFlags.WidthFixed, 68 * s)
          ui.TableSetupColumn("Stage", ui.TableColumnFlags.WidthFixed, 85 * s)
          ui.TableSetupColumn("Owned/Req", ui.TableColumnFlags.WidthFixed, 75 * s)
          ui.TableSetupColumn("State", ui.TableColumnFlags.WidthFixed, 90 * s)
          ui.TableHeadersRow()
          for _, it in ipairs(reqs) do
            ui.TableNextRow()
            ui.TableNextColumn()
            colored(C.text, it.itemName or "?")
            ui.TableNextColumn()
            colored(C.muted, tostring(it.itemId or 0))
            ui.TableNextColumn()
            colored(C.muted, it.stageId or "-")
            ui.TableNextColumn()
            colored(C.text, string.format("%d / %d", it.ownedQty or 0, it.requiredQty or 1))
            ui.TableNextColumn()
            if it.obsolete then
              colored(C.faint, "Obsolete")
            elseif (it.missingQty or 0) == 0 then
              colored(C.good, "Owned")
            else
              colored(C.gold, "Needed")
            end
          end
          ui.EndTable()
        end
      end
      ui.EndTabItem()
    end

    -- Sub-tab 2: Items & Conflicts
    if ui.BeginTabItem("Items & Conflicts##epItemsTab") then
      local items = (API.epic.Items and API.epic.Items()) or {}
      local tflags = ui.TableFlags.RowBg | ui.TableFlags.BordersInnerV | ui.TableFlags.ScrollY | ui.TableFlags.SizingStretchProp
      if ui.BeginTable("##epItemsTbl", 7, tflags, 0, 0) then
        ui.TableSetupColumn("Item Name", ui.TableColumnFlags.WidthStretch, 1.3)
        ui.TableSetupColumn("ID", ui.TableColumnFlags.WidthFixed, 60 * s)
        ui.TableSetupColumn("Characters", ui.TableColumnFlags.WidthStretch, 1.1)
        ui.TableSetupColumn("Req/Own", ui.TableColumnFlags.WidthFixed, 65 * s)
        ui.TableSetupColumn("Zone", ui.TableColumnFlags.WidthFixed, 80 * s)
        ui.TableSetupColumn("Action", ui.TableColumnFlags.WidthFixed, 130 * s)
        ui.TableSetupColumn("Reason", ui.TableColumnFlags.WidthStretch, 1.4)
        ui.TableHeadersRow()
        for _, it in ipairs(items) do
          ui.TableNextRow()
          ui.TableNextColumn()
          colored(it.hasConflict and C.gold or C.text, it.itemName or "?")
          ui.TableNextColumn()
          colored(C.muted, tostring(it.itemId or 0))
          ui.TableNextColumn()
          colored(C.text, table.concat(it.needingCharacters or {}, ", "))
          ui.TableNextColumn()
          colored(C.text, string.format("%d / %d", it.totalRequired or 0, it.totalOwned or 0))
          ui.TableNextColumn()
          colored(C.muted, (it.sourceZone and it.sourceZone ~= "") and it.sourceZone or "-")
          ui.TableNextColumn()
          colored(it.hasConflict and C.gold or C.good, it.recommendedAction or "-")
          ui.TableNextColumn()
          colored(C.muted, it.reason or "-")
        end
        ui.EndTable()
      end
      ui.EndTabItem()
    end

    -- Sub-tab 3: Policy & Priorities
    if ui.BeginTabItem("Policy & Priorities##epPolTab") then
      local function policyCheck(label, key, cur)
        local val, chg = ui.Checkbox(label .. "##epPol_" .. key, cur == true)
        if chg then runEpicDo("set_policy", key, val and "true" or "false") end
      end

      policyCheck("Enable Epic-Aware Looting", "epic_loot_enabled", pol.epicLootEnabled)
      policyCheck("Loot Only Active Epic Stage", "loot_only_active_stage", pol.lootOnlyActiveStage)
      policyCheck("Loot Future Stage Items", "loot_future_stage_items", pol.lootFutureStageItems)
      policyCheck("Loot Items For Other Characters", "loot_for_other_characters", pol.lootForOtherCharacters)
      policyCheck("Stop Looting After Epic Completion", "stop_after_completion", pol.stopAfterCompletion)
      policyCheck("Allow Looting Obsolete Epic Items", "allow_obsolete_items", pol.allowObsoleteItems)
      policyCheck("Require Confirmation For NO-DROP Items", "require_confirm_no_drop", pol.requireConfirmNoDrop)
      policyCheck("Allow Manual Conflict Approval", "allow_manual_conflict_approval", pol.allowManualConflictApproval)
      policyCheck("Enable Shared Raid Inventory Tracking", "shared_raid_inventory_tracking", pol.sharedRaidInventoryTracking)

      ui.Separator()
      ui.Text("Conflict Resolution Rule:")
      ui.SameLine()
      ui.SetNextItemWidth(220 * s)
      if ui.BeginCombo("##epConfRule", pol.conflictRule or "ActiveStageFirst") then
        for _, r in ipairs(CONFLICT_RULES) do
          if ui.Selectable(r, pol.conflictRule == r) then
            runEpicDo("set_policy", "conflict_rule", r)
          end
        end
        ui.EndCombo()
      end

      ui.Separator()
      ui.Text("Character Priority:")
      ui.SameLine()
      ui.SetNextItemWidth(120 * s)
      epicPriorityChar = ui.InputTextWithHint("##epPrioChar", "Character", epicPriorityChar, 48)
      ui.SameLine()
      ui.SetNextItemWidth(60 * s)
      epicPriorityVal = ui.InputTextWithHint("##epPrioVal", "0-100", epicPriorityVal, 8)
      ui.SameLine()
      if ui.SmallButton("Set Priority##epSetPrio") then
        runEpicDo("set_char_priority", trim(epicPriorityChar), trim(epicPriorityVal))
      end

      ui.Text("Reserve Item:")
      ui.SameLine()
      ui.SetNextItemWidth(140 * s)
      epicReserveItem = ui.InputTextWithHint("##epResItem", "Item ID or Name", epicReserveItem, 64)
      ui.SameLine()
      ui.SetNextItemWidth(120 * s)
      epicReserveChar = ui.InputTextWithHint("##epResChar", "Character", epicReserveChar, 48)
      ui.SameLine()
      if ui.SmallButton("Reserve##epResBtn") then
        runEpicDo("reserve_item", trim(epicReserveItem), trim(epicReserveChar))
      end
      ui.EndTabItem()
    end

    -- Sub-tab 4: Audit & Diagnostics
    if ui.BeginTabItem("Audit & Diagnostics##epAuditTab") then
      if ui.SmallButton("Clear Diagnostics##epClrDiag") then
        runEpicDo("clear_diagnostics")
      end
      local diags = (API.epic.Audit and API.epic.Audit()) or {}
      local tflags = ui.TableFlags.RowBg | ui.TableFlags.BordersInnerV | ui.TableFlags.ScrollY | ui.TableFlags.SizingStretchProp
      if ui.BeginTable("##epDiagTbl", 6, tflags, 0, 0) then
        ui.TableSetupColumn("Char", ui.TableColumnFlags.WidthFixed, 95 * s)
        ui.TableSetupColumn("Item", ui.TableColumnFlags.WidthFixed, 130 * s)
        ui.TableSetupColumn("Decision", ui.TableColumnFlags.WidthFixed, 130 * s)
        ui.TableSetupColumn("Recipient", ui.TableColumnFlags.WidthFixed, 95 * s)
        ui.TableSetupColumn("Mode", ui.TableColumnFlags.WidthFixed, 60 * s)
        ui.TableSetupColumn("Reason", ui.TableColumnFlags.WidthStretch, 1.6)
        ui.TableHeadersRow()
        for i = #diags, 1, -1 do
          local d = diags[i]
          ui.TableNextRow()
          ui.TableNextColumn()
          colored(C.text, d.character or "-")
          ui.TableNextColumn()
          colored(C.text, d.itemName or "-")
          ui.TableNextColumn()
          colored(C.accent, d.decision or "-")
          ui.TableNextColumn()
          colored(C.good, (d.recipient and d.recipient ~= "") and d.recipient or "-")
          ui.TableNextColumn()
          colored(d.dryRun and C.gold or C.muted, d.dryRun and "DRY" or "LIVE")
          ui.TableNextColumn()
          colored(C.muted, d.reason or "-")
        end
        ui.EndTable()
      end
      ui.EndTabItem()
    end

    ui.EndTabBar()
  end
end

local function drawWindow()
  if ui.BeginTabBar("##specialFunctionsMainTabs") then
    if ui.BeginTabItem("Teleport & Porter##sfTabPorter") then
      drawPorterTab()
      ui.EndTabItem()
    end
    if ui.BeginTabItem("Epic Quests & Loot##sfTabEpic") then
      drawEpicTab()
      ui.EndTabItem()
    end
    ui.EndTabBar()
  end
end

ui.window("Special Functions", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Special Functions", WINDOW_OPTS, drawWindow)
end)
