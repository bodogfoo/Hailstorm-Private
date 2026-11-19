-- RGMercs: Hailstone-native ImGui presentation window (RGMercsWnd).
-- Decouples presentation from RG Mercs Lua gameplay/combat logic via eq.RGMercs* and RGMercsBridge.
-- Built strictly on native Hailstone ui.* and hud.Dialog APIs.
local hud = dofile("lib/hud.lua")
local T = hud.T
local C = T.color
local WINDOW = "RGMercsWnd"

local SECTIONS = {
  { id = "Combat",         label = "Combat",         cat = "Combat",    mod = "Combat" },
  { id = "Targeting",      label = "Targeting",      cat = "Targeting", mod = "Targeting" },
  { id = "Healing",        label = "Healing",        cat = "Healing",   mod = "Healing" },
  { id = "Buffs",          label = "Buffs",          cat = "Buffs",     mod = "Buffs" },
  { id = "Debuffs",        label = "Debuffs",        cat = "Debuffs",   mod = "Debuffs" },
  { id = "Movement",       label = "Movement",       cat = "Movement",  mod = "Movement" },
  { id = "Loot",           label = "Loot",           cat = "Loot",      mod = "Loot" },
  { id = "Class settings", label = "Class settings", cat = "Class",     mod = "Class", includeExtra = true },
  { id = "Debug/logging",  label = "Debug/logging",  debug = true },
}

local KNOWN_CATS = {
  Combat = true, Targeting = true, Healing = true, Buffs = true,
  Debuffs = true, Movement = true, Loot = true, Class = true,
}

local STATUS_LABEL = {
  running = "Running",
  paused  = "Paused",
  exited  = "Ended",
  error   = "Error",
}

local activeSection = "Combat"
local filterText = ""
local cmdInput = ""
local logFilter = ""
local autoScroll = true
local selectedDbIdx = 1
local noticeText = ""
local noticeErr = false
local wasShown = false
local textDrafts = {}

local function colored(c, text)
  ui.TextColored(c[1], c[2], c[3], 1, text)
end

local function setNotice(text, isErr)
  noticeText = text or ""
  noticeErr = isErr == true
end

local function toBool(v)
  if type(v) == "boolean" then return v end
  local s = tostring(v or ""):lower()
  return s == "1" or s == "true" or s == "yes" or s == "on"
end

local function statusColor(st)
  if st.status == "running" then return C.good end
  if st.status == "paused" then return C.warn end
  if st.status == "error" or st.last_error ~= "" then return C.danger end
  return C.muted
end

local function statusDisplay(st)
  if st.status ~= "" and STATUS_LABEL[st.status] then
    return STATUS_LABEL[st.status]
  end
  if st.last_error ~= "" then return "Error" end
  if not st.found then return "Not Found" end
  if not st.allowed then return "Not Allowed" end
  return "Stopped"
end

local function findModule(st, name)
  if not name or not st.modules then return nil end
  for _, m in ipairs(st.modules) do
    if m.name == name then return m end
  end
  return nil
end

local function fileBasename(p)
  if not p or p == "" then return "" end
  return p:match("([^/\\]+)$") or p
end

local function drawSettingRow(e, s)
  ui.PushID("set_" .. e.key)
  ui.TableNextRow()

  ui.TableNextColumn()
  local label = e.key .. (e.imported and " *" or "")
  colored(e.value ~= e.default_value and C.accent or C.text, label)
  if ui.IsItemHovered() then
    local tip = { e.key }
    if e.description ~= "" then tip[#tip + 1] = e.description end
    tip[#tip + 1] = "Category: " .. e.category .. "  |  Default: " .. e.default_value
    if e.imported then tip[#tip + 1] = "Imported from existing RG Mercs config" end
    ui.SetTooltip(table.concat(tip, "\n"))
  end
  if e.description ~= "" then
    ui.TextDisabled(e.description)
  end

  ui.TableNextColumn()
  ui.SetNextItemWidth(-1)
  if e.type == "bool" then
    local cur = toBool(e.value)
    local nxt, changed = ui.Checkbox("##v", cur)
    if changed then
      eq.RGMercsSetSetting(e.key, nxt and "true" or "false", e.category)
    end
  elseif e.type == "choice" and e.choices and #e.choices > 0 then
    if ui.BeginCombo("##v", e.value ~= "" and e.value or e.default_value, 0) then
      for _, ch in ipairs(e.choices) do
        if ui.Selectable(ch, ch == e.value, 0, 0, 0) then
          eq.RGMercsSetSetting(e.key, ch, e.category)
        end
      end
      ui.EndCombo()
    end
  elseif e.type == "int" then
    local cur = math.floor(tonumber(e.value) or tonumber(e.default_value) or 0)
    local lo = math.floor(e.min_val or 0)
    local hi = math.floor(e.max_val or 100)
    if hi <= lo then hi = lo + 100 end
    if (hi - lo) <= 1000 then
      local nxt, changed = ui.SliderInt("##v", cur, lo, hi)
      if changed then
        eq.RGMercsSetSetting(e.key, tostring(math.floor(nxt)), e.category)
      end
    else
      local draft = textDrafts[e.key] or tostring(cur)
      local nxt, changed = ui.InputText("##v", draft, 32, ui.InputTextFlags.CharsDecimal)
      textDrafts[e.key] = nxt
      if changed then
        local num = tonumber(nxt)
        if num then
          local clamped = math.max(lo, math.min(hi, math.floor(num)))
          eq.RGMercsSetSetting(e.key, tostring(clamped), e.category)
        end
      end
    end
  elseif e.type == "float" then
    local cur = tonumber(e.value) or tonumber(e.default_value) or 0
    local lo = tonumber(e.min_val) or 0
    local hi = tonumber(e.max_val) or 100
    if hi <= lo then hi = lo + 100 end
    local nxt, changed = ui.SliderFloat("##v", cur, lo, hi)
    if changed then
      eq.RGMercsSetSetting(e.key, string.format("%.2f", nxt), e.category)
    end
  else
    local cur = e.value or ""
    local nxt, changed = ui.InputText("##v", cur, 256, 0)
    if changed then
      eq.RGMercsSetSetting(e.key, nxt, e.category)
    end
  end

  ui.PopID()
end

local function drawConfigSection(st, sec, s)
  if sec.mod then
    local mod = findModule(st, sec.mod)
    if mod then
      local on, changed = ui.Checkbox("Enable " .. mod.name .. " module##modtoggle", mod.enabled)
      if changed then
        eq.RGMercsSetModule(mod.name, on)
      end
      ui.SameLine()
      colored(mod.enabled and C.muted or C.faint, "Module state: " .. mod.status)
    end
  end

  ui.SetNextItemWidth(240 * s)
  filterText = ui.InputTextWithHint("##cfgfilter", "Filter settings (all categories when typing)...", filterText, 64)
  if filterText ~= "" then
    ui.SameLine()
    if ui.SmallButton("Clear##cfgf") then filterText = "" end
  end
  ui.Separator()

  local q = filterText:lower()
  local rows = {}
  for _, e in ipairs(st.settings or {}) do
    local catMatch
    if q ~= "" then
      catMatch = true
    elseif sec.includeExtra then
      catMatch = (e.category == sec.cat) or not KNOWN_CATS[e.category]
    else
      catMatch = (e.category == sec.cat)
    end
    if catMatch then
      if q == "" or e.key:lower():find(q, 1, true)
         or e.description:lower():find(q, 1, true)
         or e.category:lower():find(q, 1, true) then
        rows[#rows + 1] = e
      end
    end
  end

  local tflags = ui.TableFlags.RowBg | ui.TableFlags.ScrollY | ui.TableFlags.SizingStretchProp | ui.TableFlags.BordersInnerH
  if ui.BeginTable("##cfgtable", 2, tflags, 0, -24 * s) then
    ui.TableSetupColumn("Setting", ui.TableColumnFlags.WidthStretch, 1.35)
    ui.TableSetupColumn("Value", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableHeadersRow()
    for _, e in ipairs(rows) do
      drawSettingRow(e, s)
    end
    ui.EndTable()
  end

  if #rows == 0 then
    ui.TextDisabled(q ~= "" and "No RG Mercs settings match the filter." or "No settings in this section yet.")
  elseif sec.includeExtra and st.settings_path ~= "" then
    ui.TextDisabled("Config file: " .. st.settings_path)
  end
end

local function drawDebugSection(st, s)
  ui.Text("Module & Script Diagnostics")
  if st.settings_path ~= "" then
    ui.TextDisabled("Per-character config: " .. st.settings_path)
  end
  if st.runtime_state ~= "" then
    ui.TextDisabled("Runtime state: " .. st.runtime_state)
  end

  local mflags = ui.TableFlags.RowBg | ui.TableFlags.BordersInnerH | ui.TableFlags.SizingStretchProp
  if ui.BeginTable("##modules", 3, mflags, 0, 125 * s) then
    ui.TableSetupColumn("Module", ui.TableColumnFlags.WidthStretch, 1.0)
    ui.TableSetupColumn("Enabled", ui.TableColumnFlags.WidthFixed, 70 * s)
    ui.TableSetupColumn("Status", ui.TableColumnFlags.WidthStretch, 1.2)
    ui.TableHeadersRow()
    for _, m in ipairs(st.modules or {}) do
      ui.PushID("mod_" .. m.name)
      ui.TableNextRow()
      ui.TableNextColumn()
      ui.Text(m.name)
      ui.TableNextColumn()
      local on, changed = ui.Checkbox("##en", m.enabled)
      if changed then eq.RGMercsSetModule(m.name, on) end
      ui.TableNextColumn()
      local mc = m.status == "Error" and C.danger or (m.enabled and C.good or C.faint)
      colored(mc, m.detail ~= "" and (m.status .. " (" .. m.detail .. ")") or m.status)
      ui.PopID()
    end
    ui.EndTable()
  end

  ui.Spacing()
  ui.SeparatorText("Logs & Command Console")

  ui.SetNextItemWidth(170 * s)
  logFilter = ui.InputTextWithHint("##logfilter", "Filter logs...", logFilter, 64)
  ui.SameLine()
  if ui.SmallButton("Clear Logs") then
    eq.RGMercsClearLogs()
  end
  ui.SameLine()
  if ui.SmallButton("Copy Logs") then
    local lines = {}
    if st.last_error ~= "" then
      lines[#lines + 1] = "[ERROR] " .. st.last_error
    end
    for _, l in ipairs(st.logs or {}) do
      lines[#lines + 1] = string.format("[%s] [%s] %s", l.timestamp, (l.level or "info"):upper(), l.message)
    end
    ui.SetClipboardText(table.concat(lines, "\n"))
    setNotice("Copied " .. tostring(#lines) .. " log lines to clipboard.", false)
  end
  ui.SameLine()
  local scOn, scChanged = ui.Checkbox("Auto-scroll", autoScroll)
  if scChanged then autoScroll = scOn end

  local _, lh = ui.CalcTextSize("Ag")
  local cmdBarH = lh + 18 * s
  local lq = logFilter:lower()
  ui.BeginChild("##rglogscroll", 0, -cmdBarH, true, 0)
  local shownLogs = 0
  for _, l in ipairs(st.logs or {}) do
    local line = string.format("[%s] %s", l.timestamp, l.message)
    if lq == "" or line:lower():find(lq, 1, true) or (l.level or ""):lower():find(lq, 1, true) then
      shownLogs = shownLogs + 1
      local lc = C.text
      if l.level == "error" then lc = C.danger
      elseif l.level == "warn" then lc = C.warn
      end
      ui.PushTextWrapPos(0)
      colored(lc, line)
      ui.PopTextWrapPos()
    end
  end
  if shownLogs == 0 then
    ui.TextDisabled("No RG Mercs log entries yet.")
  end
  if autoScroll and ui.GetScrollY() >= ui.GetScrollMaxY() - 20 then
    ui.SetScrollHereY(1.0)
  end
  ui.EndChild()

  ui.SetNextItemWidth(-95 * s)
  local nextCmd, submitted = ui.InputTextWithHint("##rglcmd", "Send /rgl command (e.g. pause, pull, set ...)", cmdInput, 256,
                                                ui.InputTextFlags.EnterReturnsTrue)
  cmdInput = nextCmd
  ui.SameLine()
  local sendClicked = ui.Button("Send##rgl", 85 * s, 0)
  if (sendClicked or submitted) and cmdInput ~= "" then
    eq.RGMercsCommand(cmdInput)
    cmdInput = ""
  end
end

local WINDOW_OPTS = {
  size = function() return 760, 560 end,
  resizable = { 580, 400 },
}

local function drawWindow(x, y, w, h, s)
  local st = eq.RGMercsState(not wasShown)
  wasShown = true

  -- Header: Character, Server, State, Lifecycle Controls
  local charLabel = st.character ~= "" and st.character or (eq.Name and eq.Name() or "Unknown")
  if st.class_name ~= "" then
    charLabel = charLabel .. " (" .. st.class_name .. ")"
  end
  local srvLabel = st.server ~= "" and st.server or "Current Server"

  colored(C.text, "RGMercs")
  ui.SameLine()
  ui.TextDisabled(" ·  " .. charLabel .. " @ " .. srvLabel)
  ui.SameLine()
  colored(statusColor(st), " [" .. statusDisplay(st) .. "]")
  if st.dirty then
    ui.SameLine()
    colored(C.warn, " (unsaved)")
  end

  local live = (st.status == "running" or st.status == "paused")
  if not live then
    ui.BeginDisabled(not st.found or not st.allowed)
    if ui.SmallButton("Start") then
      local ok, why = eq.RGMercsControl("start")
      setNotice(ok and "Started rgmercs." or (why or "Failed to start rgmercs."), not ok)
    end
    ui.EndDisabled()
  else
    ui.BeginDisabled(st.kept)
    if ui.SmallButton("Stop") then
      local ok, why = eq.RGMercsControl("stop")
      setNotice(ok and "Stopped rgmercs." or (why or "Failed to stop rgmercs."), not ok)
    end
    ui.SameLine()
    if ui.SmallButton(st.status == "paused" and "Resume" or "Pause") then
      eq.RGMercsControl("pause")
    end
    ui.EndDisabled()
  end

  ui.SameLine()
  ui.BeginDisabled(not st.found or not st.allowed or st.kept)
  if ui.SmallButton("Reload") then
    local ok, why = eq.RGMercsControl("reload")
    setNotice(ok and "Reloaded rgmercs." or (why or "Failed to reload rgmercs."), not ok)
  end
  ui.EndDisabled()

  ui.SameLine()
  ui.BeginDisabled(not st.autorun_available)
  local ar, arChanged = ui.Checkbox("Autorun", st.autorun)
  if arChanged then
    eq.RGMercsControl(ar and "autorun_on" or "autorun_off")
  end
  ui.EndDisabled()
  if ui.IsItemHovered(ui.HoveredFlags.AllowWhenDisabled) then
    ui.SetTooltip(st.autorun_available and "Auto-launch rgmercs when this character enters the world" or "Available once character enters world")
  end

  ui.SameLine()
  local sup, supChanged = ui.Checkbox("Native UI only", st.suppress_script_ui)
  if supChanged then
    eq.RGMercsControl(sup and "suppress_ui_on" or "suppress_ui_off")
  end
  if ui.IsItemHovered() then
    ui.SetTooltip("Suppress RG Mercs' built-in script ImGui windows and render with this Hailstone-native UI")
  end

  ui.SameLine()
  if ui.SmallButton("Save") then
    local ok = eq.RGMercsSaveSettings()
    setNotice(ok and "Saved settings to character config." or "Unable to save settings (no character folder).", not ok)
  end
  ui.SameLine()
  if ui.SmallButton("Reset") then
    eq.RGMercsResetDefaults()
    textDrafts = {}
    setNotice("Reset RG Mercs settings to defaults (click Save to persist).", false)
  end

  -- Error Banner (visible without opening game chat)
  local errText = st.last_error
  if errText == "" and not st.found then
    errText = "Script 'rgmercs' was not found in Hailstone's script folders."
  elseif errText == "" and not st.allowed and st.why ~= "" then
    errText = "Script execution blocked: " .. st.why
  end

  if errText ~= "" then
    ui.Separator()
    colored(C.danger, "Error: " .. errText)
    ui.SameLine()
    if ui.SmallButton("Debug Log") then activeSection = "Debug/logging" end
    if st.last_error ~= "" then
      ui.SameLine()
      if ui.SmallButton("Dismiss") then eq.RGMercsControl("clear_error") end
    end
  end

  -- Existing RG Mercs SQLite/Lua Migration Banner
  local dbs = st.available_dbs or {}
  if st.imported_from ~= "" then
    colored(C.good, string.format("Migrated %d settings from %s", st.imported_count or 0, fileBasename(st.imported_from)))
    if #dbs > 0 then
      ui.SameLine()
      if ui.SmallButton("Re-import") then
        local path = dbs[selectedDbIdx] or dbs[1]
        local ok, cnt, detail = eq.RGMercsImportDb(path)
        textDrafts = {}
        setNotice(ok and string.format("Imported %d settings from %s", cnt, fileBasename(detail)) or ("Import failed: " .. tostring(detail)), not ok)
      end
    end
  elseif #dbs > 0 then
    if selectedDbIdx > #dbs then selectedDbIdx = 1 end
    colored(C.warn, "Existing RG Mercs config detected:")
    ui.SameLine()
    ui.SetNextItemWidth(220 * s)
    if ui.BeginCombo("##dbpick", fileBasename(dbs[selectedDbIdx]), 0) then
      for i, p in ipairs(dbs) do
        if ui.Selectable(fileBasename(p) .. "##" .. i, i == selectedDbIdx, 0, 0, 0) then
          selectedDbIdx = i
        end
      end
      ui.EndCombo()
    end
    ui.SameLine()
    if ui.SmallButton("Import Config (Read-Only)") then
      local ok, cnt, detail = eq.RGMercsImportDb(dbs[selectedDbIdx])
      textDrafts = {}
      setNotice(ok and string.format("Imported %d settings from %s (Save to persist)", cnt, fileBasename(detail)) or ("Import failed: " .. tostring(detail)), not ok)
    end
  end

  if noticeText ~= "" then
    colored(noticeErr and C.danger or C.muted, noticeText)
  end

  ui.Separator()

  -- Main Two-Pane Layout: Left Navigation + Right Content
  local navW = 148 * s
  ui.BeginChild("##rgnav", navW, 0, true, 0)
  local activeObj = SECTIONS[1]
  for _, sec in ipairs(SECTIONS) do
    local isActive = (activeSection == sec.id)
    local label = sec.label
    if sec.debug and (st.error_count or 0) > 0 then
      label = label .. " (" .. tostring(st.error_count) .. ")"
    end
    if ui.Selectable(label, isActive, 0, 0, 0) then
      activeSection = sec.id
    end
    if activeSection == sec.id then activeObj = sec end
  end
  ui.EndChild()

  ui.SameLine()

  ui.BeginChild("##rgcontent", 0, 0, true, 0)
  if activeObj.debug then
    drawDebugSection(st, s)
  else
    drawConfigSection(st, activeObj, s)
  end
  ui.EndChild()
end

ui.window("RGMercs", function()
  if not eq.InGame() then
    wasShown = false
    return
  end
  hud.Dialog(WINDOW, "RGMercs", WINDOW_OPTS, drawWindow)
end)
