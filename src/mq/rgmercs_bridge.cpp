#include "mq/rgmercs_bridge.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_set>

#include <sqlite3.h>

#include "core/ini_file.h"
#include "core/log.h"
#include "platform/local_time.h"

namespace hailstone::mq {

namespace fs = std::filesystem;

namespace {

std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool iequals_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

std::string trim_ascii(std::string_view s) {
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::string unquote_ascii(std::string_view s) {
    std::string t = trim_ascii(s);
    if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') || (t.front() == '\'' && t.back() == '\''))) {
        return t.substr(1, t.size() - 2);
    }
    return t;
}

bool contains_icase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    const std::string h = to_lower_ascii(haystack);
    const std::string n = to_lower_ascii(needle);
    return h.find(n) != std::string::npos;
}

std::string quote_sqlite_ident(std::string_view ident) {
    std::string out = "\"";
    for (char c : ident) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += '"';
    return out;
}

bool is_db_extension(const fs::path& p) {
    const std::string ext = to_lower_ascii(p.extension().string());
    return ext == ".db" || ext == ".sqlite" || ext == ".sqlite3";
}

bool is_lua_config_extension(const fs::path& p) {
    return to_lower_ascii(p.extension().string()) == ".lua";
}

std::string normalize_token(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return out;
}

} // namespace

RGMercsBridge::RGMercsBridge() {
    std::lock_guard<std::mutex> lock(mutex_);
    init_defaults_locked();
}

void RGMercsBridge::init_defaults_locked() {
    settings_.clear();
    modules_.clear();

    auto add_bool = [&](std::string key, std::string cat, bool def, std::string desc) {
        SettingEntry e;
        e.key = std::move(key);
        e.category = std::move(cat);
        e.type = "bool";
        e.default_value = def ? "true" : "false";
        e.value = e.default_value;
        e.description = std::move(desc);
        settings_.push_back(std::move(e));
    };
    auto add_int = [&](std::string key, std::string cat, int def, int mn, int mx, std::string desc) {
        SettingEntry e;
        e.key = std::move(key);
        e.category = std::move(cat);
        e.type = "int";
        e.default_value = std::to_string(def);
        e.value = e.default_value;
        e.min_val = static_cast<double>(mn);
        e.max_val = static_cast<double>(mx);
        e.description = std::move(desc);
        settings_.push_back(std::move(e));
    };
    auto add_str = [&](std::string key, std::string cat, std::string def, std::string desc) {
        SettingEntry e;
        e.key = std::move(key);
        e.category = std::move(cat);
        e.type = "string";
        e.default_value = std::move(def);
        e.value = e.default_value;
        e.description = std::move(desc);
        settings_.push_back(std::move(e));
    };
    auto add_choice = [&](std::string key, std::string cat, std::string def, std::vector<std::string> choices,
                          std::string desc) {
        SettingEntry e;
        e.key = std::move(key);
        e.category = std::move(cat);
        e.type = "choice";
        e.default_value = std::move(def);
        e.value = e.default_value;
        e.choices = std::move(choices);
        e.description = std::move(desc);
        settings_.push_back(std::move(e));
    };

    // Combat
    add_bool("DoMelee", "Combat", true, "Engage enemies in melee combat when in range");
    add_bool("AutoAttack", "Combat", true, "Automatically toggle attack on valid combat targets");
    add_bool("StayOnTarget", "Combat", true, "Stick to and face target during melee combat");
    add_str("StickHow", "Combat", "snaproll", "Stick command options (e.g. snaproll, !front, moveback)");
    add_choice("CombatMode", "Combat", "Auto", {"Auto", "Melee", "Caster", "Tank", "Healer", "Hybrid"},
               "Primary combat behavior profile");
    add_bool("BurnNamed", "Combat", true, "Trigger burn abilities automatically on named NPCs");
    add_bool("BurnAlways", "Combat", false, "Use burn cooldowns whenever ready in combat");
    add_int("BurnCount", "Combat", 3, 1, 20, "Hostile XTarget count threshold to trigger AE/burn");
    add_bool("UseAbilities", "Combat", true, "Use combat disciplines, skills, and abilities");
    add_bool("UseAA", "Combat", true, "Use Alternate Advancement combat abilities");

    // Targeting
    add_bool("DoAssist", "Targeting", true, "Assist the designated Main Assist target");
    add_choice("MASlot", "Targeting", "GroupMA", {"GroupMA", "RaidMA", "Tank", "Named", "Custom"},
               "Source used to determine the Main Assist");
    add_str("CustomMA", "Targeting", "", "Explicit character name when MASlot is set to Custom");
    add_int("AssistRange", "Targeting", 100, 10, 400, "Maximum range to acquire Main Assist targets");
    add_bool("AutoTarget", "Targeting", true, "Automatically select a hostile target if none selected");
    add_int("AutoEngagePct", "Targeting", 99, 1, 100, "Target HP percentage threshold before engaging");
    add_int("AutoAssistAt", "Targeting", 98, 1, 100, "Target HP percentage threshold before assisting");
    add_bool("SwitchWithMA", "Targeting", true, "Switch targets immediately if the Main Assist switches");
    add_bool("PriorityTargetFirst", "Targeting", true, "Prioritize named or high-threat targets on XTarget");

    // Healing
    add_bool("DoHeals", "Healing", true, "Enable automated healing routines");
    add_int("MaxHealPct", "Healing", 85, 1, 100, "Maximum target HP percentage eligible for heals");
    add_int("HealPct", "Healing", 75, 1, 99, "HP percentage to begin standard single-target heals");
    add_int("EmergencyHealPct", "Healing", 40, 1, 99, "HP percentage to trigger fast/emergency heals");
    add_int("GroupHealPct", "Healing", 75, 1, 99, "HP percentage threshold for group heal evaluation");
    add_int("GroupHealCount", "Healing", 2, 1, 6, "Injured group members required before group healing");
    add_int("PetHealPct", "Healing", 50, 0, 99, "Pet HP percentage threshold (0 disables pet heals)");
    add_bool("UseHoTs", "Healing", true, "Maintain Heal-over-Time spells on injured targets");
    add_bool("DoCures", "Healing", true, "Automatically cure detrimental poison/disease/curse effects");
    add_bool("DoRez", "Healing", true, "Automatically resurrect fallen group or raid members");
    add_bool("RezInCombat", "Healing", false, "Allow resurrection casts while in active combat");

    // Buffs
    add_bool("DoBuffs", "Buffs", true, "Maintain configured self, group, and pet buffs");
    add_bool("BuffInCombat", "Buffs", false, "Allow buff refreshes during combat");
    add_bool("DoSelfBuffs", "Buffs", true, "Maintain self-only spells and aura buffs");
    add_bool("DoGroupBuffs", "Buffs", true, "Cast group and beneficial target buffs on allies");
    add_bool("DoPetBuffs", "Buffs", true, "Keep group and summoned pets buffed");
    add_int("BuffCheckInterval", "Buffs", 5, 1, 60, "Seconds between downtime buff checks");
    add_int("BuffManaPct", "Buffs", 40, 0, 100, "Minimum mana percentage required to cast downtime buffs");
    add_bool("ClickyBuffs", "Buffs", true, "Use worn/inventory clicky buff items");

    // Debuffs
    add_bool("DoDebuffs", "Debuffs", true, "Cast configured debuffs on active targets");
    add_bool("DoMez", "Debuffs", false, "Crowd-control unengaged XTarget adds with Mez");
    add_int("MezMinLevel", "Debuffs", 1, 1, 125, "Minimum mob level to mez");
    add_int("MezAECount", "Debuffs", 3, 1, 10, "Unmezzed add count before attempting AE Mez");
    add_bool("DoSlow", "Debuffs", true, "Cast slow debuffs on engaged NPCs");
    add_int("SlowPct", "Debuffs", 95, 1, 100, "Target HP percentage below which to cast Slow");
    add_bool("DoSnare", "Debuffs", true, "Cast snare on fleeing or low-HP targets");
    add_int("SnarePct", "Debuffs", 30, 1, 100, "Target HP percentage below which to apply Snare");
    add_bool("DoTash", "Debuffs", true, "Apply magic resistance debuffs (Tash/Resists)");
    add_bool("DoMalo", "Debuffs", true, "Apply elemental/malo resistance debuffs");

    // Movement
    add_bool("ChaseOn", "Movement", false, "Follow the configured chase target when out of combat");
    add_str("ChaseTarget", "Movement", "", "Character name to follow when ChaseOn is enabled");
    add_int("ChaseDistance", "Movement", 25, 5, 200, "Distance beyond which movement begins toward chase target");
    add_int("ChaseStopDistance", "Movement", 15, 5, 150, "Distance at which chase movement stops");
    add_bool("CampHard", "Movement", false, "Hold camp anchor strictly and return when idle");
    add_bool("ReturnToCamp", "Movement", true, "Return to camp spot after combat ends");
    add_int("CampRadius", "Movement", 30, 5, 200, "Allowed leash radius around camp anchor");
    add_bool("UseNav", "Movement", true, "Prefer Hailstone nav mesh routing for movement");
    add_int("PullRange", "Movement", 150, 10, 500, "Maximum range to pull targets back to camp");
    add_int("NavBreakDist", "Movement", 200, 20, 1000, "Maximum distance before breaking nav follow");

    // Loot
    add_bool("DoLoot", "Loot", false, "Enable automated corpse looting after combat");
    add_bool("LootInCombat", "Loot", false, "Allow looting while still on XTarget aggro");
    add_int("LootRange", "Loot", 75, 10, 300, "Maximum radius to move to lootable corpses");
    add_int("MinSellPrice", "Loot", 0, 0, 100000, "Minimum platinum value to loot non-quest items");
    add_int("SaveBagSlots", "Loot", 1, 0, 20, "Minimum free inventory bag slots to preserve");
    add_bool("LootNoDrop", "Loot", false, "Prompt/loot No-Drop items automatically when eligible");

    // Class settings
    add_bool("ShowClassModule", "Class", true, "Enable active class rotation and ability module");
    add_choice("ClassMode", "Class", "Default", {"Default", "DPS", "Tank", "Healer", "Puller", "Assist"},
               "Class module specialization mode");
    add_bool("UseMercs", "Class", true, "Coordinate with mercenary AI/stance when active");
    add_bool("StandWhenDone", "Class", true, "Stand automatically at full mana/endurance");
    add_int("MedPct", "Class", 80, 0, 100, "Mana percentage threshold to meditate out of combat");
    add_int("EndMedPct", "Class", 80, 0, 100, "Endurance percentage threshold to rest out of combat");
    add_str("ClassPreset", "Class", "", "Optional named class configuration preset");

    // Core modules matching standard RG Mercs workflow
    const std::array<const char*, 9> kDefaultModules = {
        "Main", "Combat", "Targeting", "Healing", "Buffs", "Debuffs", "Movement", "Loot", "Class",
    };
    for (const char* m : kDefaultModules) {
        ModuleState ms;
        ms.name = m;
        ms.enabled = true;
        ms.status = "Idle";
        modules_.push_back(std::move(ms));
    }
}

bool RGMercsBridge::is_under_root(const fs::path& root, const fs::path& candidate, bool allow_equal) {
    if (root.empty() || candidate.empty()) return false;
    std::error_code ec;
    fs::path canon_root = fs::weakly_canonical(root, ec);
    if (ec) return false;
    fs::path resolved = candidate.is_relative() ? (canon_root / candidate) : candidate;
    fs::path canon_cand = fs::weakly_canonical(resolved, ec);
    if (ec) return false;
    if (canon_cand == canon_root) return allow_equal;
    const fs::path rel = canon_cand.lexically_relative(canon_root);
    if (rel.empty() || rel == "." || *rel.begin() == "..") return false;
    return true;
}

std::string RGMercsBridge::infer_category(std::string_view section_or_table, std::string_view key) {
    const std::string s = to_lower_ascii(section_or_table);
    const std::string k = to_lower_ascii(key);
    auto match = [](std::string_view src, std::initializer_list<const char*> words) {
        for (const char* w : words) {
            if (src.find(w) != std::string_view::npos) return true;
        }
        return false;
    };
    if (iequals_ascii(section_or_table, "Combat") || match(k, {"melee", "attack", "stick", "burn", "combat"}))
        return "Combat";
    if (iequals_ascii(section_or_table, "Targeting") ||
        match(k, {"assist", "target", "maslot", "customma", "engage"}))
        return "Targeting";
    if (iequals_ascii(section_or_table, "Healing") ||
        match(k, {"heal", "cure", "rez", "hot", "emergency"}))
        return "Healing";
    if (iequals_ascii(section_or_table, "Buffs") || match(k, {"buff", "clicky", "aura"}))
        return "Buffs";
    if (iequals_ascii(section_or_table, "Debuffs") ||
        match(k, {"debuff", "mez", "slow", "snare", "tash", "malo", "cripple"}))
        return "Debuffs";
    if (iequals_ascii(section_or_table, "Movement") ||
        match(k, {"chase", "camp", "nav", "follow", "pull"}))
        return "Movement";
    if (iequals_ascii(section_or_table, "Loot") || match(k, {"loot", "sell", "bagslot", "corpse"}))
        return "Loot";
    if (iequals_ascii(section_or_table, "General") || match(s, {"general", "ui"}))
        return "General";
    return "Class";
}

std::string RGMercsBridge::infer_type(std::string_view value) {
    const std::string t = to_lower_ascii(unquote_ascii(value));
    if (t == "true" || t == "false") return "bool";
    if (!t.empty()) {
        char* end_int = nullptr;
        (void)std::strtoll(t.c_str(), &end_int, 10);
        if (end_int && end_int != t.c_str() && *end_int == '\0') return "int";
        char* end_dbl = nullptr;
        const double d = std::strtod(t.c_str(), &end_dbl);
        if (end_dbl && end_dbl != t.c_str() && *end_dbl == '\0' && std::isfinite(d)) return "float";
    }
    return "string";
}

namespace {
std::string sanitize_numeric_token(std::string_view raw) {
    std::string t = unquote_ascii(raw);
    // Strip thousands separators and common trailing unit suffixes (%, s, ms).
    std::string cleaned;
    cleaned.reserve(t.size());
    for (char c : t) {
        if (c == ',' || c == '_') continue;
        cleaned.push_back(c);
    }
    t = trim_ascii(cleaned);
    const std::string low = to_lower_ascii(t);
    if (low == "true" || low == "yes" || low == "on" || low == "enabled") return "1";
    if (low == "false" || low == "no" || low == "off" || low == "disabled") return "0";
    if (t.ends_with("%")) {
        t.pop_back();
    } else if (low.ends_with("ms") && t.size() > 2) {
        t.resize(t.size() - 2);
    } else if (low.ends_with("s") && t.size() > 1 &&
               std::isdigit(static_cast<unsigned char>(t[t.size() - 2]))) {
        t.pop_back();
    }
    return trim_ascii(t);
}

std::string strip_lua_inline_comment(std::string_view line) {
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\' && (in_single || in_double)) {
            escaped = true;
            continue;
        }
        if (c == '\'' && !in_double) {
            in_single = !in_single;
        } else if (c == '"' && !in_single) {
            in_double = !in_double;
        } else if (!in_single && !in_double && c == '-' && i + 1 < line.size() && line[i + 1] == '-') {
            return std::string(line.substr(0, i));
        }
    }
    return std::string(line);
}
} // namespace

std::string RGMercsBridge::normalize_status_name(std::string_view raw) {
    const std::string low = to_lower_ascii(trim_ascii(raw));
    if (low.empty() || low == "exited" || low == "stopped" || low == "idle") return "stopped";
    if (low == "starting" || low == "initializing") return "starting";
    if (low == "running" || low == "active") return "running";
    if (low == "paused") return "paused";
    if (low == "stopping") return "stopping";
    if (low == "reloading" || low == "restarting") return "reloading";
    if (low == "error" || low == "failed") return "error";
    return low;
}

std::string RGMercsBridge::normalize_value(std::string_view type, std::string_view raw,
                                           std::string_view fallback,
                                           double min_val, double max_val,
                                           bool clamp_range, bool* out_converted_ok) {
    if (out_converted_ok) *out_converted_ok = true;
    const std::string t = unquote_ascii(raw);
    if (type == "bool") {
        const std::string low = to_lower_ascii(t);
        if (low == "1" || low == "true" || low == "yes" || low == "on" || low == "enabled") return "true";
        if (low == "0" || low == "false" || low == "no" || low == "off" || low == "disabled") return "false";
        if (out_converted_ok) *out_converted_ok = false;
        return fallback.empty() ? "false" : std::string(fallback);
    }
    if (type == "int") {
        const std::string num = sanitize_numeric_token(t);
        if (!num.empty()) {
            char* end = nullptr;
            const long long v = std::strtoll(num.c_str(), &end, 10);
            if (end != num.c_str() && *end == '\0') {
                double dv = static_cast<double>(v);
                if (clamp_range && max_val > min_val) dv = std::clamp(dv, min_val, max_val);
                return std::to_string(static_cast<long long>(std::llround(dv)));
            }
            char* end_f = nullptr;
            const double d = std::strtod(num.c_str(), &end_f);
            if (end_f != num.c_str() && *end_f == '\0' && std::isfinite(d)) {
                double dv = d;
                if (clamp_range && max_val > min_val) dv = std::clamp(dv, min_val, max_val);
                return std::to_string(static_cast<long long>(std::llround(dv)));
            }
        }
        if (out_converted_ok) *out_converted_ok = false;
        return fallback.empty() ? "0" : std::string(fallback);
    }
    if (type == "float") {
        const std::string num = sanitize_numeric_token(t);
        if (!num.empty()) {
            char* end = nullptr;
            const double d = std::strtod(num.c_str(), &end);
            if (end != num.c_str() && *end == '\0' && std::isfinite(d)) {
                double dv = d;
                if (clamp_range && max_val > min_val) dv = std::clamp(dv, min_val, max_val);
                return std::format("{:.4g}", dv);
            }
        }
        if (out_converted_ok) *out_converted_ok = false;
        return fallback.empty() ? "0" : std::string(fallback);
    }
    return t;
}

std::string RGMercsBridge::current_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    if (auto tm = platform::local_time(tt)) {
        return std::format("{:02d}:{:02d}:{:02d}", tm->tm_hour, tm->tm_min, tm->tm_sec);
    }
    return "--:--:--";
}

RGMercsBridge::SettingEntry* RGMercsBridge::find_setting_locked(std::string_view key) {
    for (auto& s : settings_) {
        if (iequals_ascii(s.key, key)) return &s;
    }
    return nullptr;
}

const RGMercsBridge::SettingEntry* RGMercsBridge::find_setting_locked(std::string_view key) const {
    for (const auto& s : settings_) {
        if (iequals_ascii(s.key, key)) return &s;
    }
    return nullptr;
}

RGMercsBridge::ModuleState* RGMercsBridge::find_module_locked(std::string_view name) {
    for (auto& m : modules_) {
        if (iequals_ascii(m.name, name)) return &m;
    }
    return nullptr;
}

void RGMercsBridge::attach_character(const fs::path& data_dir, const fs::path& character_dir,
                                     std::string character_name, std::string server_name, std::string class_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (data_dir_ == data_dir && character_dir_ == character_dir && character_name_ == character_name &&
        server_name_ == server_name && class_name_ == class_name && !settings_path_.empty()) {
        refresh_sources_locked();
        return;
    }
    data_dir_ = data_dir;
    character_dir_ = character_dir;
    character_name_ = std::move(character_name);
    server_name_ = std::move(server_name);
    class_name_ = std::move(class_name);
    imported_from_.clear();
    imported_count_ = 0;
    dirty_ = false;
    pending_changes_.clear();
    init_defaults_locked();

    fs::path chosen;
    if (!data_dir_.empty()) {
        if (!character_dir_.empty() && is_under_root(data_dir_, character_dir_, false)) {
            chosen = character_dir_ / "rgmercs_settings.ini";
        } else {
            chosen = data_dir_ / "scripts" / "config" / "rgmercs_hailstone.ini";
        }
    }
    settings_path_ = std::move(chosen);
    refresh_sources_locked();

    std::error_code ec;
    const bool has_existing = !settings_path_.empty() && fs::is_regular_file(settings_path_, ec);
    if (has_existing) {
        load_settings_locked();
        append_log_locked("info", std::format("Loaded RG Mercs settings for {} ({}).",
                                              character_name_.empty() ? "character" : character_name_,
                                              settings_path_.filename().string()));
    } else if (!available_dbs_.empty()) {
        // First time on this character with an existing RG Mercs DB/config: perform safe one-time read-only import.
        const ImportResult res = import_from_path_locked(fs::path(available_dbs_.front()));
        if (res.ok) {
            append_log_locked("info", std::format("Auto-imported {} RG Mercs settings from {}.",
                                                  res.imported_count, res.source));
        }
    }
}

void RGMercsBridge::clear_character() {
    std::lock_guard<std::mutex> lock(mutex_);
    character_dir_.clear();
    settings_path_.clear();
    character_name_.clear();
    server_name_.clear();
    class_name_.clear();
    imported_from_.clear();
    imported_count_ = 0;
    runtime_state_.clear();
    dirty_ = false;
    pending_changes_.clear();
    init_defaults_locked();
}

void RGMercsBridge::refresh_sources_locked() {
    available_dbs_.clear();
    if (data_dir_.empty()) return;

    const fs::path cfg_root = data_dir_ / "scripts" / "config";
    std::error_code ec;
    if (!fs::is_directory(cfg_root, ec)) return;

    struct Candidate {
        fs::path path;
        int score = 0;
    };
    std::vector<Candidate> found;

    const std::string char_tok = normalize_token(character_name_);
    const std::string srv_tok = normalize_token(server_name_);

    fs::recursive_directory_iterator it(cfg_root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it.depth() > 4) {
            it.disable_recursion_pending();
            continue;
        }
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        const fs::path& p = it->path();
        if (!is_under_root(data_dir_, p, false)) continue;
        if (!is_db_extension(p) && !is_lua_config_extension(p)) continue;

        const std::string name_low = to_lower_ascii(p.generic_string());
        const std::string name_tok = normalize_token(p.stem().string() + p.parent_path().filename().string());
        // Never treat class_configs/* scripts as user settings databases; skip unrelated files in config/ root.
        if (contains_icase(name_low, "/class_configs/")) continue;
        const bool in_rg = contains_icase(name_low, "rgmercs");
        if (!in_rg) continue;

        int score = is_db_extension(p) ? 100 : 20;
        if (!char_tok.empty() && name_tok.find(char_tok) != std::string::npos) score += 200;
        if (!srv_tok.empty() && name_tok.find(srv_tok) != std::string::npos) score += 50;
        found.push_back({p, score});
    }

    std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.path.generic_string() < b.path.generic_string();
    });
    for (const auto& c : found) {
        available_dbs_.push_back(c.path.string());
    }
}

std::vector<fs::path> RGMercsBridge::discover_import_sources() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<fs::path> out;
    out.reserve(available_dbs_.size());
    for (const auto& s : available_dbs_) out.emplace_back(s);
    return out;
}

bool RGMercsBridge::load_settings() {
    std::lock_guard<std::mutex> lock(mutex_);
    return load_settings_locked();
}

bool RGMercsBridge::load_settings_locked() {
    if (settings_path_.empty() || !is_under_root(data_dir_, settings_path_, false)) return false;
    IniFile ini;
    if (!ini.load(settings_path_)) return false;

    if (auto s = ini.get("Meta", "ImportedFrom")) imported_from_ = *s;
    imported_count_ = ini.get_int("Meta", "ImportedCount", imported_count_);
    if (auto sup = ini.get("General", "SuppressLegacyScriptUI")) {
        suppress_script_ui_ = to_lower_ascii(*sup) != "false" && *sup != "0";
    }

    for (const std::string& sec : ini.sections()) {
        if (iequals_ascii(sec, "Meta")) continue;
        if (iequals_ascii(sec, "Modules")) {
            for (const std::string& key : ini.keys(sec)) {
                const bool en = ini.get_int(sec, key, 1) != 0 ||
                                iequals_ascii(ini.get_string(sec, key, "true"), "true");
                if (ModuleState* ms = find_module_locked(key)) {
                    ms->enabled = en;
                    if (!en && ms->status == "Idle") ms->status = "Disabled";
                } else {
                    ModuleState added;
                    added.name = key;
                    added.enabled = en;
                    added.status = en ? "Idle" : "Disabled";
                    modules_.push_back(std::move(added));
                }
            }
            continue;
        }
        if (iequals_ascii(sec, "General")) continue;
        for (const std::string& key : ini.keys(sec)) {
            const std::string raw = ini.get_string(sec, key, "");
            if (SettingEntry* existing = find_setting_locked(key)) {
                existing->value = normalize_value(existing->type, raw);
            } else {
                SettingEntry added;
                added.key = key;
                added.category = sec;
                added.type = infer_type(raw);
                added.default_value = normalize_value(added.type, raw);
                added.value = added.default_value;
                added.description = "Loaded character setting";
                settings_.push_back(std::move(added));
            }
        }
    }
    dirty_ = false;
    return true;
}

bool RGMercsBridge::save_settings() {
    std::lock_guard<std::mutex> lock(mutex_);
    return save_settings_locked();
}

bool RGMercsBridge::save_settings_locked() {
    if (settings_path_.empty() || data_dir_.empty()) return false;
    if (!is_under_root(data_dir_, settings_path_, false)) {
        log::warn("rgmercs: refusing to write settings outside Hailstone data directory: {}", settings_path_.string());
        return false;
    }

    std::error_code ec;
    fs::create_directories(settings_path_.parent_path(), ec);
    if (ec) return false;

    IniFile ini;
    ini.set("Meta", "Character", character_name_);
    ini.set("Meta", "Server", server_name_);
    ini.set("Meta", "Class", class_name_);
    if (!imported_from_.empty()) {
        ini.set("Meta", "ImportedFrom", imported_from_);
        ini.set_int("Meta", "ImportedCount", imported_count_);
    }

    ini.set("General", "SuppressLegacyScriptUI", suppress_script_ui_ ? "true" : "false");

    for (const auto& m : modules_) {
        ini.set("Modules", m.name, m.enabled ? "true" : "false");
    }

    for (const auto& s : settings_) {
        ini.set(s.category.empty() ? "Class" : s.category, s.key, s.value);
    }

    if (!ini.save(settings_path_)) return false;
    dirty_ = false;
    return true;
}

RGMercsBridge::ImportResult RGMercsBridge::import_best_source() {
    std::lock_guard<std::mutex> lock(mutex_);
    refresh_sources_locked();
    if (available_dbs_.empty()) {
        return {false, 0, {}, "No RG Mercs configuration database found in Hailstone data directory"};
    }
    return import_from_path_locked(fs::path(available_dbs_.front()));
}

RGMercsBridge::ImportResult RGMercsBridge::import_from_path(const fs::path& source) {
    std::lock_guard<std::mutex> lock(mutex_);
    return import_from_path_locked(source);
}

RGMercsBridge::ImportResult RGMercsBridge::import_from_path_locked(const fs::path& source) {
    ImportResult res;
    res.source = source.string();

    if (data_dir_.empty() || !is_under_root(data_dir_, source, false)) {
        res.error = "Import path must stay inside Hailstone data directory";
        return res;
    }
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) {
        res.error = "Import source file does not exist";
        return res;
    }

    int count = 0;
    auto apply_pair = [&](std::string_view sec_hint, std::string_view raw_key, std::string_view raw_val) {
        const std::string key = trim_ascii(raw_key);
        if (key.empty() || iequals_ascii(key, "id") || iequals_ascii(key, "character") ||
            iequals_ascii(key, "server"))
            return;
        const std::string val = trim_ascii(raw_val);
        if (SettingEntry* existing = find_setting_locked(key)) {
            bool converted = true;
            const bool clamp = (existing->type == "int" || existing->type == "float") &&
                               existing->max_val > existing->min_val;
            existing->value = normalize_value(existing->type, val, existing->default_value,
                                              existing->min_val, existing->max_val, clamp, &converted);
            if (!converted) {
                const std::string warn = std::format(
                    "Kept default '{}' for {} '{}' (unparseable imported value '{}')",
                    existing->default_value, existing->type, existing->key, val);
                res.warnings.push_back(warn);
                append_log_locked("warn", warn);
            } else {
                existing->imported = true;
                ++count;
            }
        } else {
            SettingEntry entry;
            entry.key = key;
            entry.category = infer_category(sec_hint, key);
            entry.type = infer_type(val);
            entry.default_value = normalize_value(entry.type, val);
            entry.value = entry.default_value;
            entry.description = "Imported from RG Mercs configuration";
            entry.imported = true;
            settings_.push_back(std::move(entry));
            ++count;
        }
    };

    auto read_sqlite_val = [](sqlite3_stmt* stmt, int col, std::string& out) -> bool {
        const int col_type = sqlite3_column_type(stmt, col);
        switch (col_type) {
        case SQLITE_NULL:
        case SQLITE_BLOB:
            return false;
        case SQLITE_INTEGER:
            out = std::to_string(sqlite3_column_int64(stmt, col));
            return true;
        case SQLITE_FLOAT: {
            const double d = sqlite3_column_double(stmt, col);
            if (!std::isfinite(d)) return false;
            out = std::format("{:.6g}", d);
            return true;
        }
        case SQLITE_TEXT:
        default: {
            const unsigned char* txt = sqlite3_column_text(stmt, col);
            const int bytes = sqlite3_column_bytes(stmt, col);
            if (!txt) return false;
            out.assign(reinterpret_cast<const char*>(txt), static_cast<std::size_t>(std::max(0, bytes)));
            return true;
        }
        }
    };

    if (is_db_extension(source)) {
        sqlite3* db = nullptr;
        const int rc = sqlite3_open_v2(source.string().c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
        if (rc != SQLITE_OK || !db) {
            res.error = db ? sqlite3_errmsg(db) : "Cannot open SQLite database";
            if (db) sqlite3_close(db);
            return res;
        }

        std::vector<std::string> tables;
        sqlite3_stmt* tbl_stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%';", -1,
                               &tbl_stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(tbl_stmt) == SQLITE_ROW) {
                const unsigned char* txt = sqlite3_column_text(tbl_stmt, 0);
                if (txt) tables.emplace_back(reinterpret_cast<const char*>(txt));
            }
        }
        if (tbl_stmt) sqlite3_finalize(tbl_stmt);

        const std::string target_char_tok = normalize_token(character_name_);

        for (const std::string& table : tables) {
            const std::string qtable = quote_sqlite_ident(table);
            const std::string query = "SELECT * FROM " + qtable + ";";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
                if (stmt) sqlite3_finalize(stmt);
                continue;
            }
            const int ncols = sqlite3_column_count(stmt);
            int key_col = -1;
            int val_col = -1;
            int sec_col = -1;
            int char_col = -1;
            for (int c = 0; c < ncols; ++c) {
                const char* cname_raw = sqlite3_column_name(stmt, c);
                const std::string cname = cname_raw ? to_lower_ascii(cname_raw) : std::string();
                if (key_col < 0 &&
                    (cname == "key" || cname == "name" || cname == "setting" || cname == "setting_name" ||
                     cname == "var" || cname == "option")) {
                    key_col = c;
                } else if (val_col < 0 &&
                           (cname == "value" || cname == "val" || cname == "setting_value" || cname == "data")) {
                    val_col = c;
                } else if (sec_col < 0 && (cname == "category" || cname == "section" || cname == "module")) {
                    sec_col = c;
                } else if (char_col < 0 && (cname == "character" || cname == "char_name" || cname == "char")) {
                    char_col = c;
                }
            }

            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (char_col >= 0 && !character_name_.empty()) {
                    std::string row_char;
                    if (read_sqlite_val(stmt, char_col, row_char) && !row_char.empty()) {
                        const bool exact = iequals_ascii(row_char, character_name_);
                        const bool norm_match = !target_char_tok.empty() &&
                                                normalize_token(row_char) == target_char_tok;
                        if (!exact && !norm_match && !iequals_ascii(row_char, "default")) {
                            continue;
                        }
                    }
                }
                if (key_col >= 0 && val_col >= 0) {
                    std::string ktxt;
                    std::string vtxt;
                    std::string stxt;
                    if (read_sqlite_val(stmt, key_col, ktxt) && read_sqlite_val(stmt, val_col, vtxt)) {
                        const std::string sec =
                            (sec_col >= 0 && read_sqlite_val(stmt, sec_col, stxt) && !stxt.empty()) ? stxt : table;
                        apply_pair(sec, ktxt, vtxt);
                    }
                } else if (ncols > 0) {
                    // Wide settings table: each column represents a setting name.
                    for (int c = 0; c < ncols; ++c) {
                        if (c == char_col || c == sec_col) continue;
                        const char* cname_raw = sqlite3_column_name(stmt, c);
                        std::string vtxt;
                        if (cname_raw && read_sqlite_val(stmt, c, vtxt)) {
                            apply_pair(table, cname_raw, vtxt);
                        }
                    }
                }
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    } else if (is_lua_config_extension(source)) {
        // Safe data-only parser for RG Mercs pickled Lua setting tables (never executes Lua code).
        std::ifstream in(source, std::ios::binary);
        if (!in) {
            res.error = "Cannot read Lua settings file";
            return res;
        }
        std::string line;
        std::vector<std::string> sec_stack = {"Class"};
        while (std::getline(in, line)) {
            const std::string stripped = strip_lua_inline_comment(line);
            const std::string trimmed = trim_ascii(stripped);
            if (trimmed.empty()) continue;
            if (trimmed == "}" || trimmed == "},") {
                if (sec_stack.size() > 1) sec_stack.pop_back();
                continue;
            }
            const std::size_t eq = trimmed.find('=');
            if (eq == std::string::npos) continue;
            std::string lhs = trim_ascii(trimmed.substr(0, eq));
            std::string rhs = trim_ascii(trimmed.substr(eq + 1));
            if (!rhs.empty() && rhs.back() == ',') rhs.pop_back();
            rhs = trim_ascii(rhs);
            if (lhs.size() >= 2 && lhs.front() == '[' && lhs.back() == ']') {
                lhs = unquote_ascii(lhs.substr(1, lhs.size() - 2));
            }
            if (rhs == "{" || rhs.starts_with("function")) {
                if (!lhs.empty()) sec_stack.push_back(lhs);
                continue;
            }
            if (rhs == "}" || rhs == "nil") continue;
            apply_pair(sec_stack.back(), lhs, unquote_ascii(rhs));
        }
    } else {
        res.error = "Unsupported configuration format";
        return res;
    }

    imported_from_ = source.filename().string();
    imported_count_ = count;
    dirty_ = true;
    save_settings_locked();

    res.ok = true;
    res.imported_count = count;
    res.source = imported_from_;
    return res;
}

bool RGMercsBridge::set_setting(std::string_view key, std::string_view value, std::string_view category_hint,
                                bool queue_for_script) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (key.empty()) return false;
    std::string norm;
    if (SettingEntry* existing = find_setting_locked(key)) {
        const bool clamp = (existing->type == "int" || existing->type == "float") &&
                           existing->max_val > existing->min_val;
        norm = normalize_value(existing->type, value, existing->value,
                               existing->min_val, existing->max_val, clamp);
        if (existing->value == norm) return true;
        existing->value = norm;
    } else {
        SettingEntry added;
        added.key = std::string(key);
        added.category = category_hint.empty() ? infer_category({}, key) : std::string(category_hint);
        added.type = infer_type(value);
        norm = normalize_value(added.type, value);
        added.default_value = norm;
        added.value = norm;
        added.description = "Custom / module setting";
        settings_.push_back(std::move(added));
    }

    dirty_ = true;
    if (queue_for_script) {
        if (pending_changes_.size() < 256) pending_changes_.emplace_back(std::string(key), norm);
    }
    save_settings_locked();
    return true;
}

std::optional<RGMercsBridge::SettingEntry> RGMercsBridge::get_setting(std::string_view key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const SettingEntry* e = find_setting_locked(key)) return *e;
    return std::nullopt;
}

std::vector<RGMercsBridge::SettingEntry> RGMercsBridge::settings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

void RGMercsBridge::reset_to_defaults() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : settings_) {
        s.value = s.default_value;
        s.imported = false;
        if (pending_changes_.size() < 256) pending_changes_.emplace_back(s.key, s.value);
    }
    dirty_ = true;
    save_settings_locked();
}

void RGMercsBridge::set_module(std::string_view name, bool enabled, std::string_view status, std::string_view detail,
                               bool queue_for_script) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (name.empty()) return;
    ModuleState* ms = find_module_locked(name);
    if (!ms) {
        modules_.push_back(ModuleState{std::string(name), enabled, {}, {}});
        ms = &modules_.back();
    }
    ms->enabled = enabled;
    if (!status.empty()) {
        ms->status = std::string(status);
    } else {
        ms->status = enabled ? "Idle" : "Disabled";
    }
    if (!detail.empty() || status == "Idle" || status == "Running") ms->detail = std::string(detail);

    if (queue_for_script && pending_changes_.size() < 256) {
        pending_changes_.emplace_back("module:" + ms->name, enabled ? "true" : "false");
    }
    dirty_ = true;
    save_settings_locked();
}

std::vector<RGMercsBridge::ModuleState> RGMercsBridge::modules() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return modules_;
}

void RGMercsBridge::record_transition_locked(std::string_view to_state, std::string_view reason) {
    const std::string next = normalize_status_name(to_state);
    if (next == script_status_ && reason.empty()) return;
    while (transitions_.size() >= kMaxTransitions) transitions_.pop_front();
    transitions_.push_back(LifecycleTransition{
        current_timestamp(),
        script_status_.empty() ? std::string("stopped") : script_status_,
        next,
        std::string(reason),
    });
    script_status_ = next;
}

void RGMercsBridge::set_script_status(std::string_view status, std::string_view reason_or_error) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string next = normalize_status_name(status);
    // Critical lifecycle rule: never overwrite an observed error state with a trailing "stopped"/"exited"
    // teardown notification; error stays visible until explicitly recovered via starting/reloading/clear_error.
    if (script_status_ == "error" && next == "stopped") {
        return;
    }
    if (next == "starting" || next == "reloading") {
        last_error_.clear();
    }
    record_transition_locked(next, reason_or_error);
    if (next == "error" && !reason_or_error.empty()) {
        last_error_ = std::string(reason_or_error);
        ++error_count_;
        append_log_locked("error", reason_or_error);
    } else if (!reason_or_error.empty()) {
        append_log_locked("info", std::format("Lifecycle {} -> {}: {}",
                                              transitions_.back().from_state, next, reason_or_error));
    }
}

void RGMercsBridge::set_runtime_state(std::string_view state) {
    std::lock_guard<std::mutex> lock(mutex_);
    runtime_state_ = std::string(state);
}

void RGMercsBridge::set_script_error(std::string_view error_msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error_msg.empty()) return;
    record_transition_locked("error", error_msg);
    last_error_ = std::string(error_msg);
    ++error_count_;
    append_log_locked("error", error_msg);
}

void RGMercsBridge::clear_error() {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_.clear();
    if (script_status_ == "error") {
        record_transition_locked("stopped", "Error dismissed by user");
    }
}

std::vector<RGMercsBridge::LifecycleTransition> RGMercsBridge::transitions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {transitions_.begin(), transitions_.end()};
}

void RGMercsBridge::append_log(std::string_view level, std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex_);
    append_log_locked(level, message);
}

void RGMercsBridge::append_log_locked(std::string_view level, std::string_view message) {
    if (message.empty()) return;
    std::string lvl = level.empty() ? std::string("info") : to_lower_ascii(level);
    if (lvl != "info" && lvl != "warn" && lvl != "error") lvl = "info";
    if (lvl == "info") {
        if (contains_icase(message, "error") || contains_icase(message, "failed") || contains_icase(message, "attempt to"))
            lvl = "error";
        else if (contains_icase(message, "warn"))
            lvl = "warn";
    }
    if (lvl == "error") {
        last_error_ = std::string(message);
        ++error_count_;
    }
    while (logs_.size() >= kMaxLogs) logs_.pop_front();
    logs_.push_back(LogEntry{current_timestamp(), std::move(lvl), std::string(message)});
}

void RGMercsBridge::clear_logs() {
    std::lock_guard<std::mutex> lock(mutex_);
    logs_.clear();
    last_error_.clear();
    error_count_ = 0;
}

std::vector<RGMercsBridge::LogEntry> RGMercsBridge::logs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {logs_.begin(), logs_.end()};
}

bool RGMercsBridge::suppress_script_ui() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return suppress_script_ui_;
}

void RGMercsBridge::set_suppress_script_ui(bool suppress) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (suppress_script_ui_ == suppress) return;
    suppress_script_ui_ = suppress;
    dirty_ = true;
    save_settings_locked();
}

std::vector<std::pair<std::string, std::string>> RGMercsBridge::take_pending_changes() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto out = std::move(pending_changes_);
    pending_changes_.clear();
    return out;
}

RGMercsBridge::Snapshot RGMercsBridge::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot s;
    s.character = character_name_;
    s.server = server_name_;
    s.class_name = class_name_;
    s.settings_path = settings_path_.string();
    s.imported_from = imported_from_;
    s.imported_count = imported_count_;
    s.script_status = script_status_;
    s.runtime_state = runtime_state_;
    s.last_error = last_error_;
    s.error_count = error_count_;
    s.suppress_script_ui = suppress_script_ui_;
    s.dirty = dirty_;
    s.available_dbs = available_dbs_;
    s.modules = modules_;
    s.settings = settings_;
    s.logs.assign(logs_.begin(), logs_.end());
    s.transitions.assign(transitions_.begin(), transitions_.end());
    return s;
}

} // namespace hailstone::mq
