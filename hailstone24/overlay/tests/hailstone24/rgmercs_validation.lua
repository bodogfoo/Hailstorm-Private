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

-- Test lfs under mq.configDir.
local cfg_dir = mq.configDir .. '/rg_smoke'
lfs.mkdir(cfg_dir)
local attrs = lfs.attributes(cfg_dir)
assert(attrs and attrs.mode == 'directory', 'lfs.attributes directory check failed')

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
