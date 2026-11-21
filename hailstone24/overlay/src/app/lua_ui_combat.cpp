#include "app/lua_ui_combat.h"

#include <string>
#include <string_view>

#include <lua.hpp>

#include "app/lua_ui.h"
#include "combat/combat_types.h"
#include "plugin/lua_keep.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {
namespace {

UiHost& host_of(lua_State* L) {
    return *static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1)));
}

bool plugin_code() {
    return !ui::lua::LuaUi::caller_tag().empty();
}

void set_str(lua_State* L, const char* key, std::string_view val) {
    lua_pushlstring(L, val.data(), val.size());
    lua_setfield(L, -2, key);
}

void set_bool(lua_State* L, const char* key, bool val) {
    lua_pushboolean(L, val ? 1 : 0);
    lua_setfield(L, -2, key);
}

void set_int(lua_State* L, const char* key, lua_Integer val) {
    lua_pushinteger(L, val);
    lua_setfield(L, -2, key);
}

void set_num(lua_State* L, const char* key, double val) {
    lua_pushnumber(L, val);
    lua_setfield(L, -2, key);
}

void push_string_array(lua_State* L, const char* key, const std::vector<std::string>& items) {
    lua_createtable(L, static_cast<int>(items.size()), 0);
    for (std::size_t i = 0; i < items.size(); ++i) {
        lua_pushlstring(L, items[i].data(), items[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, key);
}

int CombatStatus(lua_State* L) {
    UiHost& h = host_of(L);
    if (plugin_code() || !h.combat.status || !h.combat.profile) {
        lua_createtable(L, 0, 4);
        set_str(L, "state", "Idle");
        set_bool(L, "running", false);
        set_bool(L, "available", false);
        set_str(L, "idleReason", plugin_code() ? "Restricted" : "Combat controller offline");
        return 1;
    }

    auto& st = plugin::lua_keep<combat::CombatStatusSnapshot>(L);
    auto& prof = plugin::lua_keep<combat::CombatProfile>(L);
    plugin::cxx_guard(L, [&] {
        st = h.combat.status();
        prof = h.combat.profile();
    });

    lua_createtable(L, 0, 36);
    set_bool(L, "available", true);
    set_str(L, "state", combat::combat_state_name(st.state));
    set_str(L, "movementMode", combat::movement_mode_name(st.movement_mode));
    set_bool(L, "running", st.running);
    set_bool(L, "dryRun", st.dry_run);
    set_bool(L, "autoLoadout", st.auto_loadout);
    set_bool(L, "rememorizeMissing", prof.rememorize_missing);
    set_bool(L, "allowMeleeAttack", prof.allow_melee_attack);
    set_bool(L, "allowPetAttack", prof.allow_pet_attack);
    set_bool(L, "useAbilities", prof.use_abilities);
    set_bool(L, "meleeBehind", prof.melee_behind);
    set_bool(L, "pauseOnManualMove", prof.pause_on_manual_move);
    set_bool(L, "manualOverrideActive", st.manual_override_active);
    set_int(L, "manualOverrideRemainingMs", static_cast<lua_Integer>(st.manual_override_remaining_ms));
    set_bool(L, "autoAttackActive", st.auto_attack_active);

    set_str(L, "characterName", st.character_name);
    set_str(L, "classShort", st.class_short);
    set_str(L, "serverName", st.server_name);
    set_str(L, "zoneShort", st.zone_short);
    set_str(L, "profileName", prof.profile_name);
    set_str(L, "role", prof.role);
    set_str(L, "mode", prof.mode);
    set_str(L, "profilePath", st.profile_path);

    set_str(L, "assistTarget", prof.assist_target);
    set_str(L, "followTarget", prof.follow_target);
    set_num(L, "assistRange", prof.assist_range);
    set_num(L, "meleeRange", prof.melee_range);
    set_num(L, "rangedMinRange", prof.ranged_min_range);
    set_num(L, "rangedMaxRange", prof.ranged_max_range);
    set_num(L, "disengageDistance", prof.disengage_distance);
    set_num(L, "followDistance", prof.follow_distance);
    set_int(L, "retreatHpPct", prof.retreat_hp_pct);
    set_int(L, "engageAtHpPct", prof.engage_at_hp_pct);
    set_num(L, "maxEngageDistance", prof.max_engage_distance);

    set_int(L, "targetId", static_cast<lua_Integer>(st.target_id));
    set_str(L, "targetName", st.target_name);
    set_int(L, "targetHpPct", st.target_hp_pct);
    set_num(L, "targetDistance", st.target_distance);
    set_int(L, "assistTargetId", static_cast<lua_Integer>(st.assist_target_id));
    set_str(L, "assistTargetName", st.assist_target_name);

    set_str(L, "transitionReason", st.transition_reason);
    set_str(L, "idleReason", st.idle_reason);
    set_str(L, "lastAction", st.last_action);
    set_str(L, "lastCast", st.last_cast);
    set_str(L, "lastMovementDecision", st.last_movement_decision);

    // Loadout audit slots
    lua_createtable(L, static_cast<int>(st.loadout_audit.slots.size()), 0);
    for (std::size_t i = 0; i < st.loadout_audit.slots.size(); ++i) {
        const auto& s = st.loadout_audit.slots[i];
        lua_createtable(L, 0, 8);
        set_int(L, "gem", s.gem_index + 1);
        set_str(L, "desired", s.desired_spec);
        set_str(L, "resolved", s.resolved_spell_name);
        set_str(L, "current", s.current_spell_name);
        set_str(L, "status", combat::gem_sync_kind_name(s.status));
        set_int(L, "requiredLevel", s.required_level);
        set_str(L, "diagnostic", s.diagnostic);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "loadout");

    push_string_array(L, "missingSpells", st.loadout_audit.missing_spells);
    push_string_array(L, "manuallyChangedGems", st.loadout_audit.manually_changed_gems);
    push_string_array(L, "unknownSpells", st.loadout_audit.unknown_spells);
    push_string_array(L, "levelRestrictedSpells", st.loadout_audit.level_restricted_spells);
    push_string_array(L, "missingSpellData", st.loadout_audit.missing_spell_data);
    push_string_array(L, "failedMemorizations", st.loadout_audit.failed_memorizations);

    // Rules summary
    lua_createtable(L, static_cast<int>(prof.rules.size()), 0);
    for (std::size_t i = 0; i < prof.rules.size(); ++i) {
        const auto& r = prof.rules[i];
        lua_createtable(L, 0, 7);
        set_str(L, "name", r.name);
        set_str(L, "spell", r.spell_name);
        set_str(L, "action", combat::spell_action_kind_name(r.action_kind));
        set_str(L, "target", combat::target_kind_name(r.target));
        set_str(L, "condition", combat::combat_condition_name(r.combat_condition));
        set_int(L, "priority", r.priority);
        set_bool(L, "enabled", r.enabled);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "rules");

    // Transitions log (newest last)
    lua_createtable(L, static_cast<int>(st.transitions.size()), 0);
    for (std::size_t i = 0; i < st.transitions.size(); ++i) {
        const auto& t = st.transitions[i];
        lua_createtable(L, 0, 4);
        set_int(L, "timeMs", static_cast<lua_Integer>(t.timestamp_ms));
        set_str(L, "from", combat::combat_state_name(t.from_state));
        set_str(L, "to", combat::combat_state_name(t.to_state));
        set_str(L, "reason", t.reason);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "transitions");

    // Action log
    lua_createtable(L, static_cast<int>(st.actions.size()), 0);
    for (std::size_t i = 0; i < st.actions.size(); ++i) {
        const auto& a = st.actions[i];
        lua_createtable(L, 0, 4);
        set_int(L, "timeMs", static_cast<lua_Integer>(a.timestamp_ms));
        set_bool(L, "dryRun", a.dry_run);
        set_str(L, "category", a.category);
        set_str(L, "detail", a.detail);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "actions");

    return 1;
}

int CombatDo(lua_State* L) {
    UiHost& h = host_of(L);
    const char* action_c = luaL_checkstring(L, 1);
    const char* arg_c = luaL_optstring(L, 2, "");
    if (plugin_code()) {
        lua_pushboolean(L, 0);
        lua_pushliteral(L, "Combat controller is player-controlled");
        return 2;
    }
    if (!h.combat.command) {
        lua_pushboolean(L, 0);
        lua_pushliteral(L, "Combat controller unavailable");
        return 2;
    }

    auto& msg = plugin::lua_keep<std::string>(L);
    msg.clear();
    bool ok = false;
    plugin::cxx_guard(L, [&] {
        ok = h.combat.command(action_c ? std::string_view(action_c) : std::string_view{},
                              arg_c ? std::string_view(arg_c) : std::string_view{},
                              msg);
    });
    lua_pushboolean(L, ok ? 1 : 0);
    if (!msg.empty()) {
        lua_pushlstring(L, msg.data(), msg.size());
        return 2;
    }
    return 1;
}

int CommanderStatus(lua_State* L) {
    UiHost& h = host_of(L);
    if (plugin_code() || !h.commander.status) {
        lua_createtable(L, 0, 5);
        set_bool(L, "available", false);
        set_bool(L, "busConnected", false);
        set_bool(L, "autoHeadlessOthers", true);
        lua_newtable(L);
        lua_setfield(L, -2, "peers");
        lua_newtable(L);
        lua_setfield(L, -2, "trackedCommands");
        return 1;
    }

    auto& snap = plugin::lua_keep<UiHost::CommanderStatusSnapshot>(L);
    plugin::cxx_guard(L, [&] { snap = h.commander.status(); });

    lua_createtable(L, 0, 10);
    set_bool(L, "available", true);
    set_str(L, "sessionId", snap.session_id);
    set_str(L, "characterName", snap.character_name);
    set_str(L, "executionState", snap.execution_state);
    set_bool(L, "busConnected", snap.bus_connected);
    set_bool(L, "autoHeadlessOthers", snap.auto_headless_others);
    set_str(L, "activeProfile", snap.active_profile_id);
    push_string_array(L, "availableProfiles", snap.available_profiles);

    lua_createtable(L, static_cast<int>(snap.peers.size()), 0);
    for (std::size_t i = 0; i < snap.peers.size(); ++i) {
        const auto& p = snap.peers[i];
        lua_createtable(L, 0, 18);
        set_str(L, "sessionId", p.session_id);
        set_str(L, "characterName", p.character_name);
        set_str(L, "serverName", p.server_name);
        set_str(L, "zoneShort", p.zone_short);
        set_str(L, "classShort", p.class_short);
        set_int(L, "level", p.level);
        set_int(L, "hpPct", p.hp_pct);
        set_int(L, "manaPct", p.mana_pct);
        set_int(L, "endPct", p.end_pct);
        set_bool(L, "online", p.online);
        set_str(L, "connectionState", p.connection_state);
        set_str(L, "executionState", execution_state_name(p.exec_state));
        set_bool(L, "isLive", p.exec_state == ExecutionState::ForegroundFull);
        set_bool(L, "isSelf", p.session_id == snap.session_id);
        set_str(L, "combatState", p.combat_state);
        set_str(L, "scriptName", p.script_name);
        set_str(L, "scriptState", p.script_state);
        set_str(L, "activeProfile", p.active_profile);
        set_str(L, "followTarget", p.follow_target);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "peers");

    lua_createtable(L, static_cast<int>(snap.tracked_commands.size()), 0);
    for (std::size_t i = 0; i < snap.tracked_commands.size(); ++i) {
        const auto& c = snap.tracked_commands[i];
        std::size_t ack_ok = 0;
        std::size_t ack_fail = 0;
        std::string last_msg = c.summary;
        for (const auto& a : c.acks) {
            if (a.ok) ++ack_ok;
            else ++ack_fail;
            if (!a.message.empty()) last_msg = a.message;
        }
        lua_createtable(L, 0, 9);
        set_int(L, "seq", static_cast<lua_Integer>(c.seq));
        set_str(L, "target", c.target);
        set_str(L, "command", c.command);
        set_str(L, "status", ipc::command_track_status_name(c.status));
        set_int(L, "ackCount", static_cast<lua_Integer>(ack_ok));
        set_int(L, "failCount", static_cast<lua_Integer>(ack_fail));
        set_str(L, "detail", last_msg);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "trackedCommands");

    return 1;
}

int CommanderDo(lua_State* L) {
    UiHost& h = host_of(L);
    const char* action_c = luaL_checkstring(L, 1);
    const char* target_c = luaL_optstring(L, 2, "");
    const char* arg_c = luaL_optstring(L, 3, "");
    if (plugin_code()) {
        lua_pushboolean(L, 0);
        lua_pushliteral(L, "Commander controls are player-controlled");
        return 2;
    }
    if (!h.commander.command) {
        lua_pushboolean(L, 0);
        lua_pushliteral(L, "Commander bridge unavailable");
        return 2;
    }
    auto& msg = plugin::lua_keep<std::string>(L);
    msg.clear();
    bool ok = false;
    plugin::cxx_guard(L, [&] {
        ok = h.commander.command(action_c ? std::string_view(action_c) : std::string_view{},
                                 target_c ? std::string_view(target_c) : std::string_view{},
                                 arg_c ? std::string_view(arg_c) : std::string_view{},
                                 msg);
    });
    lua_pushboolean(L, ok ? 1 : 0);
    if (!msg.empty()) {
        lua_pushlstring(L, msg.data(), msg.size());
        return 2;
    }
    return 1;
}

} // namespace

void register_combat_api(lua_State* L, UiHost& host) {
    for (auto [name, fn] : std::initializer_list<std::pair<const char*, lua_CFunction>>{
             {"CombatStatus", CombatStatus},
             {"CombatDo", CombatDo},
             {"CommanderStatus", CommanderStatus},
             {"CommanderDo", CommanderDo},
         }) {
        lua_pushlightuserdata(L, &host);
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -2, name);
    }
}

} // namespace hailstone::app
