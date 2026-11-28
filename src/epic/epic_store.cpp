#include "epic/epic_store.h"

#include <algorithm>
#include <format>
#include <sstream>

#include "core/ascii_case.h"
#include "core/atomic_file.h"
#include "core/json_value.h"

namespace hailstone::epic {
namespace {

using core::JsonArray;
using core::JsonObject;
using core::JsonValue;

void write_str_array(std::ostringstream& out, const std::vector<std::string>& items) {
    out << "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out << ", ";
        out << core::json_escape(items[i]);
    }
    out << "]";
}

void read_str_array(const JsonValue* v, std::vector<std::string>& out) {
    out.clear();
    if (!v || !v->is_array()) return;
    for (const auto& elem : std::get<JsonArray>(v->data)) {
        std::string s = elem.as_string();
        if (!s.empty()) out.push_back(std::move(s));
    }
}

void write_item_records(std::ostringstream& out, const std::vector<ItemOwnershipRecord>& items) {
    out << "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& r = items[i];
        if (i > 0) out << ", ";
        out << "{\"item_id\": " << r.item_id
            << ", \"item_name\": " << core::json_escape(r.item_name)
            << ", \"owned_qty\": " << r.owned_qty
            << ", \"equipped_qty\": " << r.equipped_qty
            << ", \"bank_qty\": " << r.bank_qty
            << ", \"turned_in_qty\": " << r.turned_in_qty
            << ", \"required_qty\": " << r.required_qty
            << ", \"missing_qty\": " << r.missing_qty
            << ", \"optional\": " << (r.optional ? "true" : "false")
            << ", \"obsolete\": " << (r.obsolete ? "true" : "false")
            << ", \"stage_id\": " << core::json_escape(r.stage_id)
            << "}";
    }
    out << "]";
}

void read_item_records(const JsonValue* v, std::vector<ItemOwnershipRecord>& out) {
    out.clear();
    if (!v || !v->is_array()) return;
    for (const auto& elem : std::get<JsonArray>(v->data)) {
        if (!elem.is_object()) continue;
        ItemOwnershipRecord r;
        if (const auto* f = elem.find_ci("item_id")) r.item_id = f->as_u32(0);
        if (const auto* f = elem.find_ci("item_name")) r.item_name = f->as_string();
        if (const auto* f = elem.find_ci("owned_qty")) r.owned_qty = f->as_int(0);
        if (const auto* f = elem.find_ci("equipped_qty")) r.equipped_qty = f->as_int(0);
        if (const auto* f = elem.find_ci("bank_qty")) r.bank_qty = f->as_int(0);
        if (const auto* f = elem.find_ci("turned_in_qty")) r.turned_in_qty = f->as_int(0);
        if (const auto* f = elem.find_ci("required_qty")) r.required_qty = f->as_int(0);
        if (const auto* f = elem.find_ci("missing_qty")) r.missing_qty = f->as_int(0);
        if (const auto* f = elem.find_ci("optional")) r.optional = f->as_bool(false);
        if (const auto* f = elem.find_ci("obsolete")) r.obsolete = f->as_bool(false);
        if (const auto* f = elem.find_ci("stage_id")) r.stage_id = f->as_string();
        out.push_back(std::move(r));
    }
}

void write_audit_history(std::ostringstream& out, const std::vector<AuditEntry>& items) {
    out << "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& a = items[i];
        if (i > 0) out << ", ";
        out << "{\"timestamp_ms\": " << a.timestamp_ms
            << ", \"actor\": " << core::json_escape(a.actor)
            << ", \"action\": " << core::json_escape(a.action)
            << ", \"previous_state\": " << core::json_escape(a.previous_state)
            << ", \"new_state\": " << core::json_escape(a.new_state)
            << ", \"reason\": " << core::json_escape(a.reason)
            << ", \"source\": " << core::json_escape(signal_source_name(a.source))
            << ", \"confidence\": " << core::json_escape(confidence_level_name(a.confidence))
            << ", \"previous_stage_id\": " << core::json_escape(a.previous_stage_id)
            << ", \"previous_completed_stages\": ";
        write_str_array(out, a.previous_completed_stages);
        out << ", \"previous_completed_epic\": " << (a.previous_completed_epic ? "true" : "false")
            << ", \"rollbackable\": " << (a.rollbackable ? "true" : "false")
            << "}";
    }
    out << "]";
}

void read_audit_history(const JsonValue* v, std::vector<AuditEntry>& out) {
    out.clear();
    if (!v || !v->is_array()) return;
    for (const auto& elem : std::get<JsonArray>(v->data)) {
        if (!elem.is_object()) continue;
        AuditEntry a;
        if (const auto* f = elem.find_ci("timestamp_ms")) a.timestamp_ms = f->as_u64(0);
        if (const auto* f = elem.find_ci("actor")) a.actor = f->as_string();
        if (const auto* f = elem.find_ci("action")) a.action = f->as_string();
        if (const auto* f = elem.find_ci("previous_state")) a.previous_state = f->as_string();
        if (const auto* f = elem.find_ci("new_state")) a.new_state = f->as_string();
        if (const auto* f = elem.find_ci("reason")) a.reason = f->as_string();
        if (const auto* f = elem.find_ci("source")) a.source = parse_signal_source(f->as_string());
        if (const auto* f = elem.find_ci("confidence")) a.confidence = parse_confidence_level(f->as_string());
        if (const auto* f = elem.find_ci("previous_stage_id")) a.previous_stage_id = f->as_string();
        read_str_array(elem.find_ci("previous_completed_stages"), a.previous_completed_stages);
        if (const auto* f = elem.find_ci("previous_completed_epic")) a.previous_completed_epic = f->as_bool(false);
        if (const auto* f = elem.find_ci("rollbackable")) a.rollbackable = f->as_bool(false);
        out.push_back(std::move(a));
    }
}

void write_int_map(std::ostringstream& out, const std::map<std::string, int, std::less<>>& m) {
    out << "{";
    bool first = true;
    for (const auto& [k, v] : m) {
        if (!first) out << ", ";
        first = false;
        out << core::json_escape(k) << ": " << v;
    }
    out << "}";
}

void read_int_map(const JsonValue* v, std::map<std::string, int, std::less<>>& out) {
    out.clear();
    if (!v || !v->is_object()) return;
    for (const auto& [k, val] : std::get<JsonObject>(v->data)) {
        out[k] = val.as_int(50);
    }
}

void write_str_map(std::ostringstream& out, const std::map<std::string, std::string, std::less<>>& m) {
    out << "{";
    bool first = true;
    for (const auto& [k, v] : m) {
        if (!first) out << ", ";
        first = false;
        out << core::json_escape(k) << ": " << core::json_escape(v);
    }
    out << "}";
}

void read_str_map(const JsonValue* v, std::map<std::string, std::string, std::less<>>& out) {
    out.clear();
    if (!v || !v->is_object()) return;
    for (const auto& [k, val] : std::get<JsonObject>(v->data)) {
        std::string s = val.as_string();
        if (!s.empty()) out[k] = std::move(s);
    }
}

void parse_policy(const JsonValue* v, EpicLootPolicy& p) {
    if (!v || !v->is_object()) return;
    if (const auto* f = v->find_ci("emergency_disable")) p.emergency_disable = f->as_bool(false);
    if (const auto* f = v->find_ci("epic_loot_enabled")) p.epic_loot_enabled = f->as_bool(true);
    if (const auto* f = v->find_ci("dry_run")) p.dry_run = f->as_bool(false);
    if (const auto* f = v->find_ci("loot_only_active_stage")) p.loot_only_active_stage = f->as_bool(false);
    if (const auto* f = v->find_ci("loot_future_stage_items")) p.loot_future_stage_items = f->as_bool(true);
    if (const auto* f = v->find_ci("loot_for_other_characters")) p.loot_for_other_characters = f->as_bool(true);
    if (const auto* f = v->find_ci("stop_after_completion")) p.stop_after_completion = f->as_bool(true);
    if (const auto* f = v->find_ci("allow_obsolete_items")) p.allow_obsolete_items = f->as_bool(false);
    if (const auto* f = v->find_ci("require_confirm_no_drop")) p.require_confirm_no_drop = f->as_bool(true);
    if (const auto* f = v->find_ci("allow_manual_conflict_approval")) p.allow_manual_conflict_approval = f->as_bool(true);
    if (const auto* f = v->find_ci("shared_raid_inventory_tracking")) p.shared_raid_inventory_tracking = f->as_bool(true);
    if (const auto* f = v->find_ci("conflict_rule")) p.conflict_rule = parse_conflict_rule(f->as_string());
    if (const auto* f = v->find_ci("main_character")) p.main_character = f->as_string();
    read_int_map(v->find_ci("character_priorities"), p.character_priorities);
    read_int_map(v->find_ci("group_priorities"), p.group_priorities);
    read_int_map(v->find_ci("class_priorities"), p.class_priorities);
    read_str_map(v->find_ci("reserved_items"), p.reserved_items);
    if (const auto* gp = v->find_ci("group_policies"); gp && gp->is_object()) {
        p.group_policies.clear();
        for (const auto& [gname, gv] : std::get<JsonObject>(gp->data)) {
            if (!gv.is_object()) continue;
            GroupLootPolicy g;
            g.group_name = gname;
            if (const auto* f = gv.find_ci("epic_loot_enabled")) g.epic_loot_enabled = f->as_bool(true);
            if (const auto* f = gv.find_ci("loot_only_active_stage")) g.loot_only_active_stage = f->as_bool(false);
            if (const auto* f = gv.find_ci("loot_future_stage_items")) g.loot_future_stage_items = f->as_bool(true);
            if (const auto* f = gv.find_ci("loot_for_other_characters")) g.loot_for_other_characters = f->as_bool(true);
            if (const auto* f = gv.find_ci("stop_after_completion")) g.stop_after_completion = f->as_bool(true);
            if (const auto* f = gv.find_ci("priority")) g.priority = f->as_int(50);
            p.group_policies[gname] = std::move(g);
        }
    }
}

CharacterEpicProfile parse_profile(const JsonValue& v) {
    CharacterEpicProfile p;
    if (const auto* f = v.find_ci("character")) p.character = f->as_string();
    if (const auto* f = v.find_ci("class_short")) p.class_short = f->as_string();
    if (const auto* f = v.find_ci("server")) p.server = f->as_string();
    if (const auto* f = v.find_ci("group_name")) p.group_name = f->as_string();
    // Never load password/token/secret fields; only non-secret account label.
    if (const auto* f = v.find_ci("account")) p.account = f->as_string();
    if (const auto* f = v.find_ci("epic_id")) p.epic_id = f->as_string();
    if (const auto* f = v.find_ci("epic_family")) p.epic_family = f->as_string();
    if (const auto* f = v.find_ci("epic_variant")) p.epic_variant = f->as_string();
    if (const auto* f = v.find_ci("state")) p.state = parse_progress_state(f->as_string());
    if (const auto* f = v.find_ci("current_stage_id")) p.current_stage_id = f->as_string();
    read_str_array(v.find_ci("completed_stages"), p.completed_stages);
    read_item_records(v.find_ci("required_items"), p.required_items);
    read_item_records(v.find_ci("owned_items"), p.owned_items);
    read_item_records(v.find_ci("turned_in_items"), p.turned_in_items);
    read_item_records(v.find_ci("missing_items"), p.missing_items);
    read_item_records(v.find_ci("optional_or_obsolete_items"), p.optional_or_obsolete_items);
    if (const auto* f = v.find_ci("has_completed_epic")) p.has_completed_epic = f->as_bool(false);
    if (const auto* f = v.find_ci("allow_future_epic_loot")) p.allow_future_epic_loot = f->as_bool(true);
    if (const auto* f = v.find_ci("needs_review")) p.needs_review = f->as_bool(false);
    if (const auto* f = v.find_ci("review_reason")) p.review_reason = f->as_string();
    if (const auto* f = v.find_ci("last_scan_ms")) p.last_scan_ms = f->as_u64(0);
    if (const auto* f = v.find_ci("source")) p.source = parse_signal_source(f->as_string());
    if (const auto* f = v.find_ci("confidence")) p.confidence = parse_confidence_level(f->as_string());
    read_str_array(v.find_ci("progress_reasons"), p.progress_reasons);
    if (const auto* f = v.find_ci("manual_override_locked")) p.manual_override_locked = f->as_bool(false);
    if (const auto* f = v.find_ci("total_stages")) p.total_stages = f->as_int(0);
    if (const auto* po = v.find_ci("policy_override"); po && po->is_object()) {
        auto& o = p.policy_override;
        if (const auto* f = po->find_ci("has_override")) o.has_override = f->as_bool(false);
        if (const auto* f = po->find_ci("epic_loot_enabled")) o.epic_loot_enabled = f->as_bool(true);
        if (const auto* f = po->find_ci("loot_only_active_stage")) o.loot_only_active_stage = f->as_bool(false);
        if (const auto* f = po->find_ci("loot_future_stage_items")) o.loot_future_stage_items = f->as_bool(true);
        if (const auto* f = po->find_ci("stop_after_completion")) o.stop_after_completion = f->as_bool(true);
        if (const auto* f = po->find_ci("allow_obsolete_items")) o.allow_obsolete_items = f->as_bool(false);
        if (const auto* f = po->find_ci("require_confirm_no_drop")) o.require_confirm_no_drop = f->as_bool(true);
        if (const auto* f = po->find_ci("priority")) o.priority = f->as_int(50);
    }
    read_audit_history(v.find_ci("audit_history"), p.audit_history);
    return p;
}

} // namespace

void EpicProgressStore::clear() {
    schema_version_ = kEpicSchemaVersion;
    policy_ = EpicLootPolicy{};
    profiles_.clear();
}

CharacterEpicProfile* EpicProgressStore::find_profile(const CharacterEpicKey& key) {
    for (auto& p : profiles_) {
        if (p.key().matches(key)) return &p;
    }
    return nullptr;
}

const CharacterEpicProfile* EpicProgressStore::find_profile(const CharacterEpicKey& key) const {
    for (const auto& p : profiles_) {
        if (p.key().matches(key)) return &p;
    }
    return nullptr;
}

std::vector<CharacterEpicProfile*> EpicProgressStore::find_profiles_for_character(std::string_view server,
                                                                                  std::string_view character) {
    std::vector<CharacterEpicProfile*> out;
    for (auto& p : profiles_) {
        if (iequals(p.server, server) && iequals(p.character, character)) {
            out.push_back(&p);
        }
    }
    return out;
}

void EpicProgressStore::upsert_profile(CharacterEpicProfile profile) {
    const auto key = profile.key();
    for (auto& existing : profiles_) {
        if (existing.key().matches(key)) {
            existing = std::move(profile);
            return;
        }
    }
    profiles_.push_back(std::move(profile));
}

bool EpicProgressStore::remove_profile(const CharacterEpicKey& key) {
    auto it = std::remove_if(profiles_.begin(), profiles_.end(), [&](const CharacterEpicProfile& p) {
        return p.key().matches(key);
    });
    if (it == profiles_.end()) return false;
    profiles_.erase(it, profiles_.end());
    return true;
}

void EpicProgressStore::append_audit(CharacterEpicProfile& profile, AuditEntry entry, std::size_t max_entries) {
    profile.audit_history.push_back(std::move(entry));
    if (max_entries > 0 && profile.audit_history.size() > max_entries) {
        const std::size_t excess = profile.audit_history.size() - max_entries;
        profile.audit_history.erase(profile.audit_history.begin(),
                                      profile.audit_history.begin() + static_cast<std::ptrdiff_t>(excess));
    }
}

bool EpicProgressStore::rollback_last_manual_change(CharacterEpicProfile& profile, std::uint64_t now_ms, std::string* reason) {
    for (auto it = profile.audit_history.rbegin(); it != profile.audit_history.rend(); ++it) {
        if (!it->rollbackable) continue;
        const std::string prev_state_str = progress_state_name(profile.state);
        profile.state = parse_progress_state(it->previous_state, EpicProgressState::Unknown);
        profile.current_stage_id = it->previous_stage_id;
        profile.completed_stages = it->previous_completed_stages;
        profile.has_completed_epic = it->previous_completed_epic;
        profile.manual_override_locked = false;
        it->rollbackable = false;

        AuditEntry rb;
        rb.timestamp_ms = now_ms;
        rb.actor = "user";
        rb.action = "rollback_manual_change";
        rb.previous_state = prev_state_str;
        rb.new_state = progress_state_name(profile.state);
        rb.reason = "Rolled back manual override: " + it->action;
        rb.source = SignalSource::ManualOverride;
        rb.confidence = ConfidenceLevel::Medium;
        if (reason) *reason = rb.reason;
        append_audit(profile, std::move(rb));
        return true;
    }
    if (reason) *reason = "No rollbackable manual change found";
    return false;
}

bool EpicProgressStore::import_json(std::string_view json_text, std::string* error, bool merge) {
    JsonValue root;
    std::string err;
    if (!core::parse_json(json_text, root, err)) {
        if (error) *error = err;
        return false;
    }
    if (!root.is_object()) {
        if (error) *error = "Epic database JSON root must be an object";
        return false;
    }
    int version = 1;
    if (const auto* sv = root.find_ci("schema_version")) version = sv->as_int(1);
    if (version <= 0 || version > kEpicSchemaVersion) {
        if (error) *error = std::format("Unsupported epic database schema_version {}", version);
        return false;
    }
    schema_version_ = version;
    if (!merge) {
        profiles_.clear();
        policy_ = EpicLootPolicy{};
    }
    if (const auto* pol = root.find_ci("policy")) {
        parse_policy(pol, policy_);
    }
    if (const auto* profs = root.find_ci("profiles"); profs && profs->is_array()) {
        for (const auto& pv : std::get<JsonArray>(profs->data)) {
            if (!pv.is_object()) continue;
            CharacterEpicProfile p = parse_profile(pv);
            if (!p.character.empty() && !p.server.empty()) {
                upsert_profile(std::move(p));
            }
        }
    }
    return true;
}

std::string EpicProgressStore::export_json() const {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema_version\": " << kEpicSchemaVersion << ",\n";
    out << "  \"policy\": {\n";
    out << "    \"emergency_disable\": " << (policy_.emergency_disable ? "true" : "false") << ",\n";
    out << "    \"epic_loot_enabled\": " << (policy_.epic_loot_enabled ? "true" : "false") << ",\n";
    out << "    \"dry_run\": " << (policy_.dry_run ? "true" : "false") << ",\n";
    out << "    \"loot_only_active_stage\": " << (policy_.loot_only_active_stage ? "true" : "false") << ",\n";
    out << "    \"loot_future_stage_items\": " << (policy_.loot_future_stage_items ? "true" : "false") << ",\n";
    out << "    \"loot_for_other_characters\": " << (policy_.loot_for_other_characters ? "true" : "false") << ",\n";
    out << "    \"stop_after_completion\": " << (policy_.stop_after_completion ? "true" : "false") << ",\n";
    out << "    \"allow_obsolete_items\": " << (policy_.allow_obsolete_items ? "true" : "false") << ",\n";
    out << "    \"require_confirm_no_drop\": " << (policy_.require_confirm_no_drop ? "true" : "false") << ",\n";
    out << "    \"allow_manual_conflict_approval\": " << (policy_.allow_manual_conflict_approval ? "true" : "false") << ",\n";
    out << "    \"shared_raid_inventory_tracking\": " << (policy_.shared_raid_inventory_tracking ? "true" : "false") << ",\n";
    out << "    \"conflict_rule\": " << core::json_escape(conflict_rule_name(policy_.conflict_rule)) << ",\n";
    out << "    \"main_character\": " << core::json_escape(policy_.main_character) << ",\n";
    out << "    \"character_priorities\": "; write_int_map(out, policy_.character_priorities); out << ",\n";
    out << "    \"group_priorities\": "; write_int_map(out, policy_.group_priorities); out << ",\n";
    out << "    \"class_priorities\": "; write_int_map(out, policy_.class_priorities); out << ",\n";
    out << "    \"reserved_items\": "; write_str_map(out, policy_.reserved_items); out << ",\n";
    out << "    \"group_policies\": {";
    bool first_gp = true;
    for (const auto& [gname, g] : policy_.group_policies) {
        if (!first_gp) out << ", ";
        first_gp = false;
        out << core::json_escape(gname) << ": {"
            << "\"epic_loot_enabled\": " << (g.epic_loot_enabled ? "true" : "false")
            << ", \"loot_only_active_stage\": " << (g.loot_only_active_stage ? "true" : "false")
            << ", \"loot_future_stage_items\": " << (g.loot_future_stage_items ? "true" : "false")
            << ", \"loot_for_other_characters\": " << (g.loot_for_other_characters ? "true" : "false")
            << ", \"stop_after_completion\": " << (g.stop_after_completion ? "true" : "false")
            << ", \"priority\": " << g.priority
            << "}";
    }
    out << "}\n";
    out << "  },\n";

    out << "  \"profiles\": [\n";
    for (std::size_t i = 0; i < profiles_.size(); ++i) {
        const auto& p = profiles_[i];
        out << "    {\n";
        out << "      \"character\": " << core::json_escape(p.character) << ",\n";
        out << "      \"class_short\": " << core::json_escape(p.class_short) << ",\n";
        out << "      \"server\": " << core::json_escape(p.server) << ",\n";
        out << "      \"group_name\": " << core::json_escape(p.group_name) << ",\n";
        out << "      \"account\": " << core::json_escape(p.account) << ",\n";
        out << "      \"epic_id\": " << core::json_escape(p.epic_id) << ",\n";
        out << "      \"epic_family\": " << core::json_escape(p.epic_family) << ",\n";
        out << "      \"epic_variant\": " << core::json_escape(p.epic_variant) << ",\n";
        out << "      \"state\": " << core::json_escape(progress_state_name(p.state)) << ",\n";
        out << "      \"current_stage_id\": " << core::json_escape(p.current_stage_id) << ",\n";
        out << "      \"total_stages\": " << p.total_stages << ",\n";
        out << "      \"completed_stages\": "; write_str_array(out, p.completed_stages); out << ",\n";
        out << "      \"required_items\": "; write_item_records(out, p.required_items); out << ",\n";
        out << "      \"owned_items\": "; write_item_records(out, p.owned_items); out << ",\n";
        out << "      \"turned_in_items\": "; write_item_records(out, p.turned_in_items); out << ",\n";
        out << "      \"missing_items\": "; write_item_records(out, p.missing_items); out << ",\n";
        out << "      \"optional_or_obsolete_items\": "; write_item_records(out, p.optional_or_obsolete_items); out << ",\n";
        out << "      \"has_completed_epic\": " << (p.has_completed_epic ? "true" : "false") << ",\n";
        out << "      \"allow_future_epic_loot\": " << (p.allow_future_epic_loot ? "true" : "false") << ",\n";
        out << "      \"needs_review\": " << (p.needs_review ? "true" : "false") << ",\n";
        out << "      \"review_reason\": " << core::json_escape(p.review_reason) << ",\n";
        out << "      \"last_scan_ms\": " << p.last_scan_ms << ",\n";
        out << "      \"source\": " << core::json_escape(signal_source_name(p.source)) << ",\n";
        out << "      \"confidence\": " << core::json_escape(confidence_level_name(p.confidence)) << ",\n";
        out << "      \"progress_reasons\": "; write_str_array(out, p.progress_reasons); out << ",\n";
        out << "      \"manual_override_locked\": " << (p.manual_override_locked ? "true" : "false") << ",\n";
        out << "      \"policy_override\": {"
            << "\"has_override\": " << (p.policy_override.has_override ? "true" : "false")
            << ", \"epic_loot_enabled\": " << (p.policy_override.epic_loot_enabled ? "true" : "false")
            << ", \"loot_only_active_stage\": " << (p.policy_override.loot_only_active_stage ? "true" : "false")
            << ", \"loot_future_stage_items\": " << (p.policy_override.loot_future_stage_items ? "true" : "false")
            << ", \"stop_after_completion\": " << (p.policy_override.stop_after_completion ? "true" : "false")
            << ", \"allow_obsolete_items\": " << (p.policy_override.allow_obsolete_items ? "true" : "false")
            << ", \"require_confirm_no_drop\": " << (p.policy_override.require_confirm_no_drop ? "true" : "false")
            << ", \"priority\": " << p.policy_override.priority
            << "},\n";
        out << "      \"audit_history\": "; write_audit_history(out, p.audit_history); out << "\n";
        out << "    }";
        if (i + 1 < profiles_.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

bool EpicProgressStore::load(const std::filesystem::path& path, std::string* error) {
    auto text = hailstone::read_text_file(path);
    if (!text) {
        if (error) *error = "Epic database file not found: " + path.string();
        return false;
    }
    return import_json(*text, error, false);
}

bool EpicProgressStore::save(const std::filesystem::path& path, std::string* error) const {
    if (!hailstone::write_file_atomic(path, export_json())) {
        if (error) *error = "Failed to save epic database: " + path.string();
        return false;
    }
    return true;
}

bool EpicProgressStore::backup_to(const std::filesystem::path& backup_path, std::string* error) const {
    return save(backup_path, error);
}

bool EpicProgressStore::restore_from(const std::filesystem::path& backup_path, std::string* error) {
    return load(backup_path, error);
}

} // namespace hailstone::epic
