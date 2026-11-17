-- Smoke test exercising the core APIs required by DerpleDude/rgmercs in Hailstone 24.
local mq = require('mq')
local PackageMan = require('mq.PackageMan')
local lsqlite3 = PackageMan.Require('lsqlite3')
local lfs = PackageMan.Require('luafilesystem', 'lfs')
local actors = require('actors')
local icons = require('mq.ICONS')
local Set = require('mq.set')
local bit32 = require('bit32')

assert(lsqlite3 ~= nil, 'lsqlite3 missing')
assert(lfs ~= nil, 'lfs missing')
assert(actors ~= nil, 'actors missing')
assert(icons.FA_PLAY ~= nil, 'mq.ICONS missing FA_PLAY')
assert(bit32.bor(1, 4) == 5, 'bit32.bor failed')

local s = Set.new({ 'WAR', 'CLR' })
assert(s:contains('WAR') and not s:contains('BRD'), 'mq.Set failed')

-- Test lsqlite3 + 5-arg update_hook as used by rgmercs utils/config_db.lua.
local hook_called = false
local db = lsqlite3.open_memory()
assert(db ~= nil, 'open_memory failed')
db:update_hook(function(ud, op, dbName, tableName, rowId)
    if ud == 'rg_ud' and tableName == 'settings' and rowId >= 1 then
        hook_called = true
    end
end, 'rg_ud')
assert(db:exec('CREATE TABLE settings (k TEXT PRIMARY KEY, v TEXT);') == lsqlite3.OK)
assert(db:exec("INSERT INTO settings VALUES('PullMode', 'Chain');") == lsqlite3.OK)
assert(hook_called, 'lsqlite3 update_hook did not fire with 5 args')

local read_val = nil
for row in db:nrows("SELECT v FROM settings WHERE k='PullMode'") do
    read_val = row.v
end
assert(read_val == 'Chain', 'lsqlite3 nrows failed')
db:close()

-- Test lfs under mq.configDir and verify sandbox escape rejection.
local cfg_dir = mq.configDir .. '/rg_smoke'
lfs.mkdir(cfg_dir)
local attrs = lfs.attributes(cfg_dir)
assert(attrs and attrs.mode == 'directory', 'lfs.attributes directory check failed')
assert(lfs.currentdir() ~= '', 'lfs.currentdir empty')
assert(lfs.attributes('/etc/passwd') == nil, 'lfs escape should be blocked')
assert(lfs.mkdir('/tmp/hailstone_escape_dir') == nil, 'lfs.mkdir escape should be blocked')
assert(lsqlite3.open('/tmp/hailstone_escape_test.db') == nil, 'lsqlite3.open escape should be blocked')

-- Test package & safe debug subsets.
assert(type(package) == 'table' and type(package.config) == 'string' and #package.config >= 5, 'package.config missing')
assert(package.loadlib == nil, 'package.loadlib must remain nil')
assert(type(debug) == 'table' and type(debug.getinfo) == 'function' and type(debug.traceback) == 'function', 'debug.getinfo/traceback missing')
assert(debug.getinfo(1, 'S') ~= nil, 'debug.getinfo call failed')
assert(type(debug.traceback('ok', 1)) == 'string', 'debug.traceback call failed')
assert(debug.sethook == nil and debug.getregistry == nil and debug.setmetatable == nil, 'unsafe debug functions must be nil')

-- Test ImVec4:ToImU32() and ImGui.ConsoleWidget.
local col = ImVec4(1, 0, 0, 1)
assert(col:ToImU32() == 0xFF0000FF, 'ImVec4:ToImU32 mismatch')
assert(ImGui ~= nil and ImGui.ConsoleWidget ~= nil, 'ImGui.ConsoleWidget missing')
local con = ImGui.ConsoleWidget.new('rg_test')
con:AppendText('line %d', 1)
con:AppendTextUnformatted('line 2')
assert(#con._lines == 2, 'ConsoleWidget AppendText failed')
con:Clear()
assert(#con._lines == 0, 'ConsoleWidget Clear failed')

-- Test sandboxed io inside mq.configDir, read-only mq.luaDir, and escape rejection.
assert(io ~= nil and type(io.open) == 'function' and type(io.close) == 'function', 'sandboxed io missing')
assert(io.popen == nil and io.tmpfile == nil, 'unsafe io.popen/tmpfile must be nil')
local test_file = cfg_dir .. '/io_test.txt'
local wf, werr = io.open(test_file, 'w')
assert(wf ~= nil, 'sandboxed io.open write failed: ' .. tostring(werr))
assert(wf:write('alpha=1\nbeta=2\n') == wf, 'file:write return mismatch')
assert(io.type(wf) == 'file', 'io.type open mismatch')
assert(io.close(wf) == true, 'io.close failed')
assert(io.type(wf) == 'closed file', 'io.type closed mismatch')

local rf, rerr = io.open(test_file, 'r')
assert(rf ~= nil, 'sandboxed io.open read failed: ' .. tostring(rerr))
assert(rf:read('*a') == 'alpha=1\nbeta=2\n', 'file:read(*a) mismatch')
rf:close()

local seen_lines = {}
for line in io.lines(test_file) do
    seen_lines[#seen_lines + 1] = line
end
assert(#seen_lines == 2 and seen_lines[1] == 'alpha=1' and seen_lines[2] == 'beta=2', 'io.lines mismatch')

local ro_script, ro_err = io.open(mq.luaDir .. '/rgmercs_validation.lua', 'r')
assert(ro_script ~= nil, 'read-only luaDir io.open failed: ' .. tostring(ro_err))
ro_script:close()
assert(io.open(mq.luaDir .. '/rgmercs_validation.lua', 'w') == nil, 'write into luaDir should be blocked')
assert(io.open('/etc/passwd', 'r') == nil, 'io.open absolute read escape should be blocked')
assert(io.open(mq.configDir .. '/../../outside.txt', 'w') == nil, 'io.open traversal write escape should be blocked')

-- Test mq.DataType.new + mq.AddTopLevelObject (RG Mercs RGMercs TLO).
local rgType = mq.DataType.new('RGMercsType', {
    Members = {
        Status = function(_, _)
            return 'string', 'Running'
        end,
    },
    ToString = function(_)
        return 'RGMercs'
    end,
})
assert(rgType ~= nil, 'mq.DataType.new failed')
mq.AddTopLevelObject('RGMercs', function()
    return rgType, true
end)
assert(mq.TLO.RGMercs.Status() == 'Running', 'mq.AddTopLevelObject failed')

-- Test actors dropbox register + local roundtrip over SessionBusClient.
local received_msg = nil
local dropbox = actors.register('rg_smoke_box', function(msg)
    received_msg = msg.content
end)
assert(dropbox ~= nil, 'actors.register failed')
dropbox:send({ character = 'Tester' }, { action = 'heal', pct = 45 })

for _ = 1, 20 do
    mq.delay(10)
    if received_msg ~= nil then break end
end
assert(received_msg ~= nil and received_msg.action == 'heal' and received_msg.pct == 45, 'actors loopback failed')

-- Verify essential TLOs used across RG Mercs.
assert(mq.TLO.Me.AAPoints() == 7, 'Me.AAPoints mismatch')
assert(mq.TLO.Me.AmIGroupLeader() == false, 'Me.AmIGroupLeader mismatch')
assert(mq.TLO.Group.Leader() == 'Groupie', 'Group.Leader mismatch')
assert(mq.TLO.MacroQuest.Path('lua')() ~= '', 'MacroQuest.Path(lua) empty')
assert(mq.TLO.Lua.Script('rgmercs_validation').Status() == 'RUNNING', 'Lua.Script status mismatch')

_G.RGMERCS_VALIDATION_OK = 'PASS'
print('RGMERCS_SMOKE_PASS')
