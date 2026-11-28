#include "app/lua_ui_epic.h"

#include <string>
#include <string_view>
#include <vector>

#include <lua.hpp>

#include "app/lua_ui.h"
#include "epic/epic_types.h"
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

void push_str_array(lua_State* L, const char* key, const std::vector<std::string>& items) {
    lua_createtable(L, static_cast<int>(items.size()), 0);
    for (std::size_t i = 0; i < items.size(); ++i) {
        lua_pushlstring(L, items[i].data(), items[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, key);
}

void push_item_records(lua_State* L, const char* key, const std::vector<epic::ItemOwnershipRecord>& items) {
    lua_createtable(L, static_cast<int>(items.size()), 0);
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& r = items[i];
        lua_createtable(L, 0, 9);
        set_int(L, "itemId", static_cast<lua_Integer>(r.item_id));
        set_str(L, "itemName", r.item_name);
        set_int(L, "ownedQty", r.owned_qty);
        set_int(L, "equippedQty", r.equipped_qty);
        set_int(L, "bankQty", r.bank_qty);
        set_int(L, "turnedInQty", r.turned_in_qty);
        set_int(L, "requiredQty", r.required_qty);
        set_int(L, "missingQty", r.missing_qty);
        set_bool(L, "optional", r.optional);
        set_bool(L, "obsolete", r.obsolete);
        set_str(L, "stageId", r.stage_id);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, key);
}

int EpicStatus(lua_State* L) {
    UiHost& h = host_of(L);
    if (plugin_code() || !h.epic.profiles || !h.epic.policy) {
        lua_createtable(L, 0, 2);
        set_bool(L, "available", false);
        return 1;
    }

    auto& profiles = plugin::lua_keep<std::vector<epic::CharacterEpicProfile>>(L);
    auto& defs = plugin::lua_keep<std::vector<epic::EpicDefinition>>(L);
    auto& pol = plugin::lua_keep<epic::EpicLootPolicy>(L);
    plugin::cxx_guard(L, [&] {
        profiles = h.epic.profiles();
        defs = h.epic.definitions ? h.epic.definitions() : std::vector<epic::EpicDefinition>{};
        pol = h.epic.policy();
    });

    lua_createtable(L, 0, 6);
    set_bool(L, "available", true);
    set_bool(L, "emergencyDisable", pol.emergency_disable);
    set_bool(L, "dryRun", pol.dry_run);

    // Policy
    lua_createtable(L, 0, 14);
    set_bool(L, "emergencyDisable", pol.emergency_disable);
    set_bool(L, "epicLootEnabled", pol.epic_loot_enabled);
    set_bool(L, "dryRun", pol.dry_run);
    set_bool(L, "lootOnlyActiveStage", pol.loot_only_active_stage);
    set_bool(L, "lootFutureStageItems", pol.loot_future_stage_items);
    set_bool(L, "lootForOtherCharacters", pol.loot_for_other_characters);
    set_bool(L, "stopAfterCompletion", pol.stop_after_completion);
    set_bool(L, "allowObsoleteItems", pol.allow_obsolete_items);
    set_bool(L, "requireConfirmNoDrop", pol.require_confirm_no_drop);
    set_bool(L, "allowManualConflictApproval", pol.allow_manual_conflict_approval);
    set_bool(L, "sharedRaidInventoryTracking", pol.shared_raid_inventory_tracking);
    set_str(L, "conflictRule", epic::conflict_rule_name(pol.conflict_rule));
    set_str(L, "mainCharacter", pol.main_character);

    lua_createtable(L, 0, static_cast<int>(pol.character_priorities.size()));
    for (const auto& [ch, prio] : pol.character_priorities) {
        set_int(L, ch.c_str(), prio);
    }
    lua_setfield(L, -2, "characterPriorities");

    lua_createtable(L, 0, static_cast<int>(pol.reserved_items.size()));
    for (const auto& [item_k, ch] : pol.reserved_items) {
        set_str(L, item_k.c_str(), ch);
    }
    lua_setfield(L, -2, "reservedItems");
    lua_setfield(L, -2, "policy");

    // Profiles
    lua_createtable(L, static_cast<int>(profiles.size()), 0);
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        const auto& p = profiles[i];
        lua_createtable(L, 0, 26);
        set_str(L, "storageKey", p.key().storage_key());
        set_str(L, "character", p.character);
        set_str(L, "classShort", p.class_short);
        set_str(L, "server", p.server);
        set_str(L, "groupName", p.group_name);
        set_str(L, "account", p.account);
        set_str(L, "epicId", p.epic_id);
        set_str(L, "epicFamily", p.epic_family);
        set_str(L, "epicVariant", p.epic_variant);
        set_str(L, "state", epic::progress_state_name(p.state));
        set_str(L, "currentStageId", p.current_stage_id);
        set_int(L, "completedStageCount", static_cast<lua_Integer>(p.completed_stages.size()));
        set_int(L, "totalStages", p.total_stages);
        set_num(L, "progressPct", p.progress_percent());
        set_bool(L, "hasCompletedEpic", p.has_completed_epic);
        set_bool(L, "allowFutureEpicLoot", p.allow_future_epic_loot);
        set_bool(L, "needsReview", p.needs_review);
        set_str(L, "reviewReason", p.review_reason);
        set_num(L, "lastScanMs", static_cast<double>(p.last_scan_ms));
        set_str(L, "source", epic::signal_source_name(p.source));
        set_str(L, "confidence", epic::confidence_level_name(p.confidence));
        set_bool(L, "manualOverrideLocked", p.manual_override_locked);
        push_str_array(L, "completedStages", p.completed_stages);
        push_str_array(L, "progressReasons", p.progress_reasons);
        push_item_records(L, "requiredItems", p.required_items);
        push_item_records(L, "ownedItems", p.owned_items);
        push_item_records(L, "turnedInItems", p.turned_in_items);
        push_item_records(L, "missingItems", p.missing_items);
        push_item_records(L, "obsoleteItems", p.optional_or_obsolete_items);

        lua_createtable(L, static_cast<int>(p.audit_history.size()), 0);
        for (std::size_t j = 0; j < p.audit_history.size(); ++j) {
            const auto& a = p.audit_history[j];
            lua_createtable(L, 0, 8);
            set_num(L, "timestampMs", static_cast<double>(a.timestamp_ms));
            set_str(L, "actor", a.actor);
            set_str(L, "action", a.action);
            set_str(L, "previousState", a.previous_state);
            set_str(L, "newState", a.new_state);
            set_str(L, "reason", a.reason);
            set_bool(L, "rollbackable", a.rollbackable);
            lua_rawseti(L, -2, static_cast<lua_Integer>(j + 1));
        }
        lua_setfield(L, -2, "auditHistory");

        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "profiles");

    // Definitions summary
    lua_createtable(L, static_cast<int>(defs.size()), 0);
    for (std::size_t i = 0; i < defs.size(); ++i) {
        const auto& d = defs[i];
        lua_createtable(L, 0, 6);
        set_str(L, "epicId", d.epic_id);
        set_str(L, "family", d.family);
        set_str(L, "variantName", d.variant_name);
        push_str_array(L, "classes", d.class_restrictions);
        lua_createtable(L, static_cast<int>(d.stages.size()), 0);
        for (std::size_t j = 0; j < d.stages.size(); ++j) {
            lua_createtable(L, 0, 3);
            set_str(L, "stageId", d.stages[j].stage_id);
            set_str(L, "name", d.stages[j].name);
            set_int(L, "order", d.stages[j].order);
            lua_rawseti(L, -2, static_cast<lua_Integer>(j + 1));
        }
        lua_setfield(L, -2, "stages");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "definitions");

    return 1;
}

int EpicItems(lua_State* L) {
    UiHost& h = host_of(L);
    if (plugin_code() || !h.epic.item_needs) {
        lua_createtable(L, 0, 0);
        return 1;
    }
    auto& items = plugin::lua_keep<std::vector<epic::LootDecisionEngine::ItemNeedSummary>>(L);
    plugin::cxx_guard(L, [&] { items = h.epic.item_needs(); });

    lua_createtable(L, static_cast<int>(items.size()), 0);
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        lua_createtable(L, 0, 10);
        set_int(L, "itemId", static_cast<lua_Integer>(it.item_id));
        set_str(L, "itemName", it.item_name);
        set_str(L, "sourceZone", it.source_zone);
        set_int(L, "totalRequired", it.total_required);
        set_int(L, "totalOwned", it.total_owned);
        set_int(L, "totalMissing", it.total_missing);
        set_int(L, "topPriority", it.top_priority);
        set_bool(L, "hasConflict", it.has_conflict);
        push_str_array(L, "needingCharacters", it.needing_characters);
        set_str(L, "recommendedAction", it.recommended_action);
        set_str(L, "reason", it.reason);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int EpicAudit(lua_State* L) {
    UiHost& h = host_of(L);
    if (plugin_code() || !h.epic.diagnostics) {
        lua_createtable(L, 0, 0);
        return 1;
    }
    auto& diags = plugin::lua_keep<std::vector<epic::EpicDiagnosticEntry>>(L);
    plugin::cxx_guard(L, [&] { diags = h.epic.diagnostics(); });

    lua_createtable(L, static_cast<int>(diags.size()), 0);
    for (std::size_t i = 0; i < diags.size(); ++i) {
        const auto& d = diags[i];
        lua_createtable(L, 0, 14);
        set_num(L, "timestampMs", static_cast<double>(d.timestamp_ms));
        set_str(L, "character", d.character);
        set_str(L, "classShort", d.class_short);
        set_str(L, "server", d.server);
        set_str(L, "groupName", d.group_name);
        set_str(L, "epicId", d.epic_id);
        set_str(L, "variant", d.variant);
        set_str(L, "stage", d.stage);
        set_int(L, "itemId", static_cast<lua_Integer>(d.item_id));
        set_str(L, "itemName", d.item_name);
        set_str(L, "decision", epic::loot_outcome_name(d.decision));
        set_str(L, "recipient", d.recipient);
        set_str(L, "reason", d.reason);
        set_str(L, "confidence", epic::confidence_level_name(d.confidence));
        set_str(L, "source", epic::signal_source_name(d.source));
        set_bool(L, "dryRun", d.dry_run);
        set_bool(L, "success", d.success);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int EpicDo(lua_State* L) {
    UiHost& h = host_of(L);
    if (plugin_code() || !h.epic.command) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, plugin_code() ? "Restricted" : "Epic system unavailable");
        return 2;
    }
    const char* action = luaL_checkstring(L, 1);
    const char* arg1 = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
    const char* arg2 = lua_isstring(L, 3) ? lua_tostring(L, 3) : "";
    const char* arg3 = lua_isstring(L, 4) ? lua_tostring(L, 4) : "";
    auto& msg = plugin::lua_keep<std::string>(L);
    msg.clear();
    bool ok = false;
    plugin::cxx_guard(L, [&] {
        ok = h.epic.command(action, arg1, arg2, arg3, msg);
    });
    lua_pushboolean(L, ok ? 1 : 0);
    lua_pushlstring(L, msg.data(), msg.size());
    return 2;
}

int EpicExport(lua_State* L) {
    UiHost& h = host_of(L);
    if (plugin_code() || !h.epic.export_json) {
        lua_pushstring(L, "");
        return 1;
    }
    auto& json = plugin::lua_keep<std::string>(L);
    plugin::cxx_guard(L, [&] { json = h.epic.export_json(); });
    lua_pushlstring(L, json.data(), json.size());
    return 1;
}

int EpicImport(lua_State* L) {
    UiHost& h = host_of(L);
    if (plugin_code() || !h.epic.import_json) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, plugin_code() ? "Restricted" : "Epic import unavailable");
        return 2;
    }
    std::size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    const bool merge = lua_isboolean(L, 2) ? (lua_toboolean(L, 2) != 0) : true;
    auto& msg = plugin::lua_keep<std::string>(L);
    msg.clear();
    bool ok = false;
    plugin::cxx_guard(L, [&] {
        ok = h.epic.import_json(std::string_view(data, len), merge, msg);
    });
    lua_pushboolean(L, ok ? 1 : 0);
    lua_pushlstring(L, msg.data(), msg.size());
    return 2;
}

} // namespace

void register_epic_api(lua_State* L, UiHost& host) {
    const luaL_Reg funcs[] = {
        {"EpicStatus", EpicStatus},
        {"EpicItems", EpicItems},
        {"EpicAudit", EpicAudit},
        {"EpicDo", EpicDo},
        {"EpicExport", EpicExport},
        {"EpicImport", EpicImport},
    };
    for (const auto& f : funcs) {
        lua_pushlightuserdata(L, &host);
        lua_pushcclosure(L, f.func, 1);
        lua_setfield(L, -2, f.name);
    }
}

} // namespace hailstone::app
