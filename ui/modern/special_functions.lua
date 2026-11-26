-- Hailstone Special Functions (SpecialFunctionsWnd): Teleport menu with zone filter/dropdown
-- and PORTER (ALL) multi-session dispatch (/specialfunctions, /special, /porterall, /porter).

local API = hs.UI
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color

local WINDOW = "SpecialFunctionsWnd"
local WINDOW_OPTS = {
  size = function() return 720, 600 end,
  resizable = { 540, 440 },
}

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

local function drawWindow()
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

ui.window("Special Functions", function()
  if not API.session.InGame() then return end
  hud.Dialog(WINDOW, "Special Functions", WINDOW_OPTS, drawWindow)
end)
