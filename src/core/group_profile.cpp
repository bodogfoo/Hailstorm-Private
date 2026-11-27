#include "core/group_profile.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>
#include <variant>

#include "core/ini_file.h"
#include "core/mq_login_import.h"

namespace hailstone::core {

namespace fs = std::filesystem;

namespace {

std::string trim_copy(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

std::string lower_copy(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

bool parse_bool_str(std::string_view raw, bool fallback) {
    const std::string t = lower_copy(trim_copy(raw));
    if (t.empty()) return fallback;
    if (t == "1" || t == "true" || t == "yes" || t == "on") return true;
    if (t == "0" || t == "false" || t == "no" || t == "off") return false;
    return fallback;
}

std::optional<int> parse_strict_int(std::string_view s) {
    const std::string t = trim_copy(s);
    if (t.empty()) return std::nullopt;
    char* end = nullptr;
    const long v = std::strtol(t.c_str(), &end, 10);
    if (!end || *end != '\0') return std::nullopt;
    return static_cast<int>(v);
}

std::string sanitize_path_component(std::string_view raw, std::string_view fallback = "default") {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char uc : raw) {
        if (std::isalnum(uc) || uc == '_' || uc == '-' || uc == '.') {
            out.push_back(static_cast<char>(uc));
        } else if (std::isspace(uc)) {
            out.push_back('_');
        }
    }
    while (!out.empty() && (out.front() == '.' || out.front() == '_')) out.erase(out.begin());
    while (!out.empty() && (out.back() == '.' || out.back() == '_')) out.pop_back();
    if (out.empty() || out == "." || out == "..") return std::string(fallback);
    return out;
}

// Strict password/secret key filter so plaintext secrets are never retained, exported, or echoed.
bool is_secret_key(std::string_view key) {
    const std::string k = lower_copy(trim_copy(key));
    if (k == "password" || k == "passwd" || k == "pass" || k == "pwd" ||
        k == "master_password" || k == "master_pass" || k == "secret" ||
        k == "token" || k == "ticket" || k == "session_ticket" || k == "dpapi" ||
        k == "password_blob" || k == "encrypted_password") {
        return true;
    }
    if (k.ends_with("_password") || k.ends_with("_passwd") || k.ends_with("_pass") ||
        k.ends_with("_secret") || k.ends_with("_token") || k.ends_with("_ticket") ||
        k.ends_with("_blob")) {
        return true;
    }
    return false;
}

bool has_dotdot_component(const fs::path& p) {
    for (const auto& part : p) {
        if (part == "..") return true;
    }
    return false;
}

bool read_file_text(const fs::path& path, std::string& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open profile file: " + path.string();
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

void write_atomic_text(const fs::path& path, std::string_view content, std::string* error, bool& ok) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), ec);
    }
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "failed to write " + path.string();
            ok = false;
            return;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) {
            if (error) *error = "failed to flush " + path.string();
            ok = false;
            return;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        if (error) *error = "failed to finalize profile file: " + path.string();
        ok = false;
        return;
    }
    ok = true;
}

int parse_slot_section_number(std::string_view sec, int fallback) {
    const std::string low = lower_copy(sec);
    if (low.starts_with("slot")) {
        if (auto n = parse_strict_int(low.substr(4)); n && *n >= 1 && *n <= 24) return *n;
    }
    return fallback;
}

void finalize_profile(GroupProfile& p, const fs::path& path) {
    p.source_path = path;
    if (p.id.empty()) {
        p.id = sanitize_group_profile_id(path.stem().string());
    } else {
        p.id = sanitize_group_profile_id(p.id);
    }
    if (p.name.empty()) p.name = p.id;
    p.bg_tick_hz = std::clamp(p.bg_tick_hz, 10, 60);
    if (p.gpu_mode.empty()) p.gpu_mode = "cached";

    std::set<int> used_nums;
    for (auto& s : p.slots) {
        if (s.slot_number >= 1 && s.slot_number <= 24) used_nums.insert(s.slot_number);
    }
    int next_num = 1;
    for (std::size_t i = 0; i < p.slots.size(); ++i) {
        auto& s = p.slots[i];
        if (s.slot_number < 1 || s.slot_number > 24) {
            while (next_num <= 24 && used_nums.contains(next_num)) ++next_num;
            s.slot_number = std::min(next_num, 24);
            used_nums.insert(s.slot_number);
        }
        if (s.launch_order <= 0) s.launch_order = s.slot_number;
        if (s.session_id.empty()) s.session_id = format_slot_id(s.slot_number);
        if (s.server_id.empty()) s.server_id = p.default_server_id;
        if (s.world.empty()) s.world = p.default_world;
        s.bg_tick_hz = std::clamp(s.bg_tick_hz > 0 ? s.bg_tick_hz : p.bg_tick_hz, 10, 60);
        if (s.gpu_mode.empty()) s.gpu_mode = p.gpu_mode;
    }

    std::stable_sort(p.slots.begin(), p.slots.end(), [](const GroupSlot& a, const GroupSlot& b) {
        if (a.launch_order != b.launch_order) return a.launch_order < b.launch_order;
        return a.slot_number < b.slot_number;
    });
    if (p.slots.size() > 24) p.slots.resize(24);
}

// ---------------------------------------------------------------- Minimal strict JSON parser & emitter
struct JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue, std::less<>>;

struct JsonValue {
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject>;
    Storage data{nullptr};

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(data); }
    bool is_bool() const { return std::holds_alternative<bool>(data); }
    bool is_number() const { return std::holds_alternative<double>(data); }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_array() const { return std::holds_alternative<JsonArray>(data); }
    bool is_object() const { return std::holds_alternative<JsonObject>(data); }

    std::string as_string(std::string_view fallback = {}) const {
        if (const auto* s = std::get_if<std::string>(&data)) return *s;
        if (const auto* b = std::get_if<bool>(&data)) return *b ? "true" : "false";
        if (const auto* d = std::get_if<double>(&data)) return std::to_string(static_cast<long long>(*d));
        return std::string(fallback);
    }
    bool as_bool(bool fallback) const {
        if (const auto* b = std::get_if<bool>(&data)) return *b;
        if (const auto* d = std::get_if<double>(&data)) return *d != 0.0;
        if (const auto* s = std::get_if<std::string>(&data)) return parse_bool_str(*s, fallback);
        return fallback;
    }
    int as_int(int fallback) const {
        if (const auto* d = std::get_if<double>(&data)) return static_cast<int>(*d);
        if (const auto* b = std::get_if<bool>(&data)) return *b ? 1 : 0;
        if (const auto* s = std::get_if<std::string>(&data)) {
            if (auto n = parse_strict_int(*s)) return *n;
        }
        return fallback;
    }
    const JsonValue* find_ci(std::string_view key) const {
        const auto* obj = std::get_if<JsonObject>(&data);
        if (!obj) return nullptr;
        for (const auto& [k, v] : *obj) {
            if (ieq(k, key)) return &v;
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view src) : s_(src) {}

    bool parse(JsonValue& out, std::string& err) {
        // Skip optional UTF-8 BOM
        if (s_.size() >= 3 &&
            static_cast<unsigned char>(s_[0]) == 0xEF &&
            static_cast<unsigned char>(s_[1]) == 0xBB &&
            static_cast<unsigned char>(s_[2]) == 0xBF) {
            pos_ = 3;
        }
        skip_ws();
        if (!parse_value(out, err)) return false;
        skip_ws();
        if (pos_ != s_.size()) {
            err = std::format("unexpected trailing JSON character at offset {}", pos_);
            return false;
        }
        return true;
    }

private:
    std::string_view s_;
    std::size_t pos_ = 0;

    void skip_ws() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    bool parse_value(JsonValue& out, std::string& err) {
        skip_ws();
        if (pos_ >= s_.size()) {
            err = "unexpected end of JSON";
            return false;
        }
        const char ch = s_[pos_];
        if (ch == '{') return parse_object(out, err);
        if (ch == '[') return parse_array(out, err);
        if (ch == '"') {
            std::string str;
            if (!parse_string(str, err)) return false;
            out.data = std::move(str);
            return true;
        }
        if (ch == 't' && s_.substr(pos_, 4) == "true") {
            pos_ += 4;
            out.data = true;
            return true;
        }
        if (ch == 'f' && s_.substr(pos_, 5) == "false") {
            pos_ += 5;
            out.data = false;
            return true;
        }
        if (ch == 'n' && s_.substr(pos_, 4) == "null") {
            pos_ += 4;
            out.data = nullptr;
            return true;
        }
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) {
            return parse_number(out, err);
        }
        err = std::format("invalid JSON token at offset {}", pos_);
        return false;
    }

    bool parse_string(std::string& out, std::string& err) {
        if (pos_ >= s_.size() || s_[pos_] != '"') {
            err = "expected '\"'";
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= s_.size()) {
                    err = "unterminated JSON escape";
                    return false;
                }
                const char esc = s_[pos_++];
                switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > s_.size()) {
                        err = "incomplete \\uXXXX escape in JSON";
                        return false;
                    }
                    unsigned int cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = s_[pos_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned int>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned int>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned int>(h - 'A' + 10);
                        else {
                            err = "invalid hex in JSON \\u escape";
                            return false;
                        }
                    }
                    if (cp < 0x80) out.push_back(static_cast<char>(cp));
                    else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default:
                    err = "unknown JSON escape sequence";
                    return false;
                }
            } else {
                out.push_back(c);
            }
        }
        err = "unterminated JSON string";
        return false;
    }

    bool parse_number(JsonValue& out, std::string& err) {
        const char* begin = s_.data() + pos_;
        char* end = nullptr;
        const double v = std::strtod(begin, &end);
        if (end == begin) {
            err = "invalid JSON number";
            return false;
        }
        pos_ = static_cast<std::size_t>(end - s_.data());
        out.data = v;
        return true;
    }

    bool parse_array(JsonValue& out, std::string& err) {
        ++pos_; // skip '['
        JsonArray arr;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            out.data = std::move(arr);
            return true;
        }
        while (true) {
            JsonValue elem;
            if (!parse_value(elem, err)) return false;
            arr.push_back(std::move(elem));
            skip_ws();
            if (pos_ >= s_.size()) {
                err = "unterminated JSON array";
                return false;
            }
            if (s_[pos_] == ']') {
                ++pos_;
                out.data = std::move(arr);
                return true;
            }
            if (s_[pos_] != ',') {
                err = "expected ',' or ']' in JSON array";
                return false;
            }
            ++pos_;
        }
    }

    bool parse_object(JsonValue& out, std::string& err) {
        ++pos_; // skip '{'
        JsonObject obj;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            out.data = std::move(obj);
            return true;
        }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(key, err)) return false;
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') {
                err = "expected ':' after JSON key";
                return false;
            }
            ++pos_;
            JsonValue val;
            if (!parse_value(val, err)) return false;
            if (!is_secret_key(key)) {
                obj[std::move(key)] = std::move(val);
            }
            skip_ws();
            if (pos_ >= s_.size()) {
                err = "unterminated JSON object";
                return false;
            }
            if (s_[pos_] == '}') {
                ++pos_;
                out.data = std::move(obj);
                return true;
            }
            if (s_[pos_] != ',') {
                err = "expected ',' or '}' in JSON object";
                return false;
            }
            ++pos_;
        }
    }
};

std::string json_escape(std::string_view s) {
    std::string out = "\"";
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) out += std::format("\\u{:04x}", static_cast<unsigned int>(c));
            else out.push_back(static_cast<char>(c));
            break;
        }
    }
    out.push_back('"');
    return out;
}

bool load_ini_profile(const fs::path& path, GroupProfile& out, std::string* error) {
    IniFile ini;
    if (!ini.load(path)) {
        if (error) *error = "cannot read INI profile: " + path.string();
        return false;
    }

    auto read_meta = [&](std::string_view sec, std::string_view key, std::string_view fallback = {}) -> std::string {
        if (is_secret_key(key)) return std::string(fallback);
        return ini.get_string(sec, key, fallback);
    };

    // Accept both [profile] (preferred for profiles/*.ini) and [global] (hailstone24_roster.ini compatibility).
    auto meta_str = [&](std::string_view key, std::string_view alt = {}) -> std::string {
        if (auto v = ini.get("profile", key)) return *v;
        if (!alt.empty()) {
            if (auto v = ini.get("profile", alt)) return *v;
        }
        if (auto v = ini.get("global", key)) return *v;
        if (!alt.empty()) {
            if (auto v = ini.get("global", alt)) return *v;
        }
        return {};
    };
    auto meta_int = [&](std::string_view key, int fallback) -> int {
        if (auto v = ini.get("profile", key)) {
            if (auto n = parse_strict_int(*v)) return *n;
        }
        if (auto v = ini.get("global", key)) {
            if (auto n = parse_strict_int(*v)) return *n;
        }
        return fallback;
    };
    auto meta_bool = [&](std::string_view key, bool fallback) -> bool {
        if (auto v = ini.get("profile", key)) return parse_bool_str(*v, fallback);
        if (auto v = ini.get("global", key)) return parse_bool_str(*v, fallback);
        return fallback;
    };

    out.id = meta_str("id");
    out.name = meta_str("name");
    out.description = meta_str("description");
    out.preset_kind = meta_str("preset_kind", "preset");
    out.default_server_id = meta_str("default_server", "server");
    if (out.default_server_id.empty()) out.default_server_id = meta_str("server_id");
    out.default_world = meta_str("default_world", "world");
    out.auto_login = meta_bool("auto_login", true);
    out.auto_kick = meta_bool("auto_kick", true);
    out.bg_tick_hz = meta_int("bg_tick_hz", 25);
    out.gpu_mode = meta_str("gpu_mode");

    int seq = 1;
    for (const std::string& sec : ini.sections()) {
        const std::string low = lower_copy(sec);
        if (!low.starts_with("slot") && !low.starts_with("character")) continue;

        GroupSlot s;
        const int parsed_sec_num = parse_slot_section_number(sec, seq);
        s.slot_number = std::clamp(ini.get_int(sec, "slot_number", ini.get_int(sec, "slot", parsed_sec_num)), 1, 24);
        s.launch_order = ini.get_int(sec, "launch_order", ini.get_int(sec, "order", ini.get_int(sec, "startup_order", seq)));
        s.enabled = parse_bool_str(ini.get_string(sec, "enabled", "1"), true);
        s.session_id = ini.get_string(sec, "session_id");
        s.character = ini.get_string(sec, "character");
        s.account = ini.get_string(sec, "account");
        s.server_id = ini.get_string(sec, "server_id");
        if (s.server_id.empty()) s.server_id = ini.get_string(sec, "server");
        s.world = ini.get_string(sec, "world");
        s.class_role = ini.get_string(sec, "class_role");
        if (s.class_role.empty()) s.class_role = ini.get_string(sec, "class");
        if (s.class_role.empty()) s.class_role = ini.get_string(sec, "role");
        s.settings_path = ini.get_string(sec, "settings_path");
        if (s.settings_path.empty()) s.settings_path = ini.get_string(sec, "settings");
        s.group_leader = ini.get_string(sec, "group_leader");
        s.follow_target = ini.get_string(sec, "follow_target");
        if (s.follow_target.empty()) s.follow_target = ini.get_string(sec, "follow");
        s.combat_profile = ini.get_string(sec, "combat_profile");
        s.script_module = ini.get_string(sec, "script_module");
        if (s.script_module.empty()) s.script_module = ini.get_string(sec, "script");
        s.script_settings = ini.get_string(sec, "script_settings");
        s.spell_loadout = ini.get_string(sec, "spell_loadout");
        if (s.spell_loadout.empty()) s.spell_loadout = ini.get_string(sec, "spells");
        s.auto_login = parse_bool_str(ini.get_string(sec, "auto_login", out.auto_login ? "1" : "0"), out.auto_login);
        if (auto h = ini.get(sec, "headless")) {
            s.headless = parse_bool_str(*h, out.slots.empty() ? false : true);
        } else if (auto st = ini.get(sec, "state")) {
            s.headless = (*st != "foreground" && *st != "live");
        } else {
            s.headless = !out.slots.empty();
        }
        s.auto_kick = parse_bool_str(ini.get_string(sec, "auto_kick", out.auto_kick ? "1" : "0"), out.auto_kick);
        s.bg_tick_hz = ini.get_int(sec, "bg_tick_hz", out.bg_tick_hz);
        s.gpu_mode = ini.get_string(sec, "gpu_mode", out.gpu_mode);
        (void)read_meta;

        if (!s.account.empty() || !s.character.empty() || !s.class_role.empty()) {
            out.slots.push_back(std::move(s));
        }
        ++seq;
    }

    // A group profile's leader is the persistent follow authority.  Keep the
    // leader itself free of a follow target, and default every other slot to
    // following that leader unless the profile explicitly overrides it.
    for (auto& slot : out.slots) {
        if (slot.group_leader.empty()) continue;
        if (ieq(slot.character, slot.group_leader)) {
            slot.follow_target.clear();
        } else if (slot.follow_target.empty()) {
            slot.follow_target = slot.group_leader;
        }
    }

    finalize_profile(out, path);
    return true;
}

bool load_json_profile(const fs::path& path, GroupProfile& out, std::string* error) {
    std::string text;
    if (!read_file_text(path, text, error)) return false;

    JsonValue root;
    std::string parse_err;
    JsonParser parser(text);
    if (!parser.parse(root, parse_err)) {
        if (error) *error = path.string() + ": " + parse_err;
        return false;
    }
    if (!root.is_object()) {
        if (error) *error = path.string() + ": root JSON value must be an object";
        return false;
    }

    auto str_field = [](const JsonValue& obj, std::string_view k1, std::string_view k2 = {}) -> std::string {
        if (const auto* v = obj.find_ci(k1)) return v->as_string();
        if (!k2.empty()) {
            if (const auto* v = obj.find_ci(k2)) return v->as_string();
        }
        return {};
    };
    auto int_field = [](const JsonValue& obj, std::string_view k1, std::string_view k2, int fallback) -> int {
        if (const auto* v = obj.find_ci(k1)) return v->as_int(fallback);
        if (!k2.empty()) {
            if (const auto* v = obj.find_ci(k2)) return v->as_int(fallback);
        }
        return fallback;
    };
    auto bool_field = [](const JsonValue& obj, std::string_view k1, bool fallback) -> bool {
        if (const auto* v = obj.find_ci(k1)) return v->as_bool(fallback);
        return fallback;
    };

    out.id = str_field(root, "id");
    out.name = str_field(root, "name");
    out.description = str_field(root, "description");
    out.preset_kind = str_field(root, "preset_kind", "preset");
    out.default_server_id = str_field(root, "default_server", "server_id");
    if (out.default_server_id.empty()) out.default_server_id = str_field(root, "server");
    out.default_world = str_field(root, "default_world", "world");
    out.auto_login = bool_field(root, "auto_login", true);
    out.auto_kick = bool_field(root, "auto_kick", true);
    out.bg_tick_hz = int_field(root, "bg_tick_hz", {}, 25);
    out.gpu_mode = str_field(root, "gpu_mode");

    const JsonValue* arr_val = root.find_ci("slots");
    if (!arr_val) arr_val = root.find_ci("characters");
    if (arr_val && arr_val->is_array()) {
        const auto& arr = std::get<JsonArray>(arr_val->data);
        int seq = 1;
        for (const auto& item : arr) {
            if (!item.is_object()) continue;
            GroupSlot s;
            s.slot_number = std::clamp(int_field(item, "slot_number", "slot", seq), 1, 24);
            s.launch_order = int_field(item, "launch_order", "order", int_field(item, "startup_order", {}, seq));
            s.enabled = bool_field(item, "enabled", true);
            s.session_id = str_field(item, "session_id");
            s.character = str_field(item, "character");
            s.account = str_field(item, "account");
            s.server_id = str_field(item, "server_id", "server");
            s.world = str_field(item, "world");
            s.class_role = str_field(item, "class_role", "class");
            if (s.class_role.empty()) s.class_role = str_field(item, "role");
            s.settings_path = str_field(item, "settings_path", "settings");
            s.group_leader = str_field(item, "group_leader");
            s.follow_target = str_field(item, "follow_target", "follow");
            s.combat_profile = str_field(item, "combat_profile");
            s.script_module = str_field(item, "script_module", "script");
            s.script_settings = str_field(item, "script_settings");
            s.spell_loadout = str_field(item, "spell_loadout", "spells");
            s.auto_login = bool_field(item, "auto_login", out.auto_login);
            s.headless = bool_field(item, "headless", !out.slots.empty());
            s.auto_kick = bool_field(item, "auto_kick", out.auto_kick);
            s.bg_tick_hz = int_field(item, "bg_tick_hz", {}, out.bg_tick_hz);
            s.gpu_mode = str_field(item, "gpu_mode");
            if (!s.account.empty() || !s.character.empty() || !s.class_role.empty()) {
                out.slots.push_back(std::move(s));
            }
            ++seq;
        }
    }

    finalize_profile(out, path);
    return true;
}

std::string normalize_settings_for_storage(std::string_view raw) {
    if (raw.empty()) return {};
    const fs::path p(raw);
    if (has_dotdot_component(p)) return {};
    if (p.is_absolute()) {
        return p.lexically_normal().generic_string();
    }
    return p.lexically_normal().generic_string();
}

} // namespace

std::string format_slot_id(int slot_number) {
    return std::format("slot{:02d}", std::clamp(slot_number, 1, 24));
}

std::string sanitize_group_profile_id(std::string_view raw) {
    std::string out;
    bool prev_dash = false;
    for (unsigned char uc : raw) {
        if (std::isalnum(uc)) {
            out.push_back(static_cast<char>(std::tolower(uc)));
            prev_dash = false;
        } else if (uc == '_' || uc == '-' || uc == ' ') {
            const char sep = (uc == '_') ? '_' : '-';
            if (!out.empty() && !prev_dash) {
                out.push_back(sep);
                prev_dash = true;
            }
        }
    }
    while (!out.empty() && (out.back() == '-' || out.back() == '_')) out.pop_back();
    if (out.empty()) return "default";
    return out;
}

std::optional<fs::path> find_group_profile_file(const fs::path& profiles_dir, std::string_view name_or_path) {
    const std::string trimmed = trim_copy(name_or_path);
    if (trimmed.empty()) return std::nullopt;

    std::error_code ec;
    const fs::path candidate(trimmed);
    if (candidate.is_absolute() || trimmed.find('/') != std::string::npos || trimmed.find('\\') != std::string::npos) {
        if (has_dotdot_component(candidate)) return std::nullopt;
        if (fs::is_regular_file(candidate, ec)) return candidate;
        if (!candidate.is_absolute() && !profiles_dir.empty()) {
            const fs::path rel = profiles_dir / candidate;
            if (!has_dotdot_component(rel.lexically_relative(profiles_dir)) && fs::is_regular_file(rel, ec)) {
                return rel;
            }
        }
        return std::nullopt;
    }

    if (profiles_dir.empty() || !fs::is_directory(profiles_dir, ec)) return std::nullopt;

    // 1. Direct filename matches (.ini or .json)
    const std::string slug = sanitize_group_profile_id(trimmed);
    for (const std::string& stem : {trimmed, slug}) {
        for (const char* ext : {".ini", ".json", ""}) {
            const fs::path p = profiles_dir / (stem + ext);
            if (fs::is_regular_file(p, ec)) return p;
        }
    }

    // 2. Match by loaded profile.id or profile.name (case-insensitive)
    const auto all = list_group_profiles(profiles_dir);
    for (const auto& p : all) {
        if (ieq(p.id, trimmed) || ieq(p.name, trimmed) || ieq(p.id, slug)) {
            return p.source_path;
        }
    }
    return std::nullopt;
}

std::vector<GroupProfile> list_group_profiles(const fs::path& profiles_dir) {
    std::vector<GroupProfile> out;
    if (profiles_dir.empty()) return out;
    std::error_code ec;
    if (!fs::is_directory(profiles_dir, ec)) return out;

    for (auto it = fs::directory_iterator(profiles_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code f_ec;
        if (!it->is_regular_file(f_ec)) continue;
        const fs::path p = it->path();
        const std::string ext = lower_copy(p.extension().string());
        if (ext != ".ini" && ext != ".json") continue;
        GroupProfile gp;
        if (load_group_profile(p, gp)) {
            out.push_back(std::move(gp));
        }
    }

    std::sort(out.begin(), out.end(), [](const GroupProfile& a, const GroupProfile& b) {
        const std::string la = lower_copy(a.name);
        const std::string lb = lower_copy(b.name);
        if (la != lb) return la < lb;
        return a.id < b.id;
    });
    return out;
}

bool load_group_profile(const fs::path& path, GroupProfile& out, std::string* error) {
    out = GroupProfile{};
    const std::string ext = lower_copy(path.extension().string());
    if (ext == ".json") return load_json_profile(path, out, error);
    return load_ini_profile(path, out, error);
}

bool save_group_profile_ini(const GroupProfile& profile, const fs::path& path, std::string* error) {
    GroupProfile norm = profile;
    finalize_profile(norm, path);

    std::ostringstream out;
    out << "# Hailstone Group Profile (NO PASSWORDS STORED HERE)\n";
    out << "# Credentials are stored exclusively in Hailstone's system keyring.\n\n";
    out << "[profile]\n";
    out << "id=" << norm.id << "\n";
    out << "name=" << norm.name << "\n";
    if (!norm.description.empty()) out << "description=" << norm.description << "\n";
    if (!norm.preset_kind.empty()) out << "preset_kind=" << norm.preset_kind << "\n";
    if (!norm.default_server_id.empty()) out << "default_server=" << norm.default_server_id << "\n";
    if (!norm.default_world.empty()) out << "default_world=" << norm.default_world << "\n";
    out << "auto_login=" << (norm.auto_login ? 1 : 0) << "\n";
    out << "auto_kick=" << (norm.auto_kick ? 1 : 0) << "\n";
    out << "bg_tick_hz=" << norm.bg_tick_hz << "\n";
    out << "gpu_mode=" << norm.gpu_mode << "\n";

    for (const GroupSlot& s : norm.slots) {
        const std::string sec = format_slot_id(s.slot_number);
        out << "\n[" << sec << "]\n";
        out << "slot_number=" << s.slot_number << "\n";
        out << "launch_order=" << s.launch_order << "\n";
        out << "enabled=" << (s.enabled ? 1 : 0) << "\n";
        out << "session_id=" << s.session_id << "\n";
        if (!s.character.empty()) out << "character=" << s.character << "\n";
        if (!s.account.empty()) out << "account=" << s.account << "\n";
        if (!s.server_id.empty()) out << "server_id=" << s.server_id << "\n";
        if (!s.world.empty()) out << "world=" << s.world << "\n";
        if (!s.class_role.empty()) out << "class_role=" << s.class_role << "\n";
        if (!s.settings_path.empty()) {
            out << "settings_path=" << normalize_settings_for_storage(s.settings_path) << "\n";
        }
        if (!s.group_leader.empty()) out << "group_leader=" << s.group_leader << "\n";
        if (!s.follow_target.empty()) out << "follow_target=" << s.follow_target << "\n";
        if (!s.combat_profile.empty()) out << "combat_profile=" << s.combat_profile << "\n";
        if (!s.script_module.empty()) out << "script_module=" << s.script_module << "\n";
        if (!s.script_settings.empty()) out << "script_settings=" << s.script_settings << "\n";
        if (!s.spell_loadout.empty()) out << "spell_loadout=" << s.spell_loadout << "\n";
        out << "auto_login=" << (s.auto_login ? 1 : 0) << "\n";
        out << "headless=" << (s.headless ? 1 : 0) << "\n";
        out << "auto_kick=" << (s.auto_kick ? 1 : 0) << "\n";
        out << "bg_tick_hz=" << s.bg_tick_hz << "\n";
        out << "gpu_mode=" << s.gpu_mode << "\n";
    }

    bool ok = false;
    write_atomic_text(path, out.str(), error, ok);
    return ok;
}

bool save_group_profile_json(const GroupProfile& profile, const fs::path& path, std::string* error) {
    GroupProfile norm = profile;
    finalize_profile(norm, path);

    std::ostringstream out;
    out << "{\n";
    out << "  \"id\": " << json_escape(norm.id) << ",\n";
    out << "  \"name\": " << json_escape(norm.name) << ",\n";
    out << "  \"description\": " << json_escape(norm.description) << ",\n";
    out << "  \"preset_kind\": " << json_escape(norm.preset_kind) << ",\n";
    out << "  \"default_server\": " << json_escape(norm.default_server_id) << ",\n";
    out << "  \"default_world\": " << json_escape(norm.default_world) << ",\n";
    out << "  \"auto_login\": " << (norm.auto_login ? "true" : "false") << ",\n";
    out << "  \"auto_kick\": " << (norm.auto_kick ? "true" : "false") << ",\n";
    out << "  \"bg_tick_hz\": " << norm.bg_tick_hz << ",\n";
    out << "  \"gpu_mode\": " << json_escape(norm.gpu_mode) << ",\n";
    out << "  \"slots\": [\n";
    for (std::size_t i = 0; i < norm.slots.size(); ++i) {
        const GroupSlot& s = norm.slots[i];
        out << "    {\n";
        out << "      \"slot_number\": " << s.slot_number << ",\n";
        out << "      \"launch_order\": " << s.launch_order << ",\n";
        out << "      \"enabled\": " << (s.enabled ? "true" : "false") << ",\n";
        out << "      \"session_id\": " << json_escape(s.session_id) << ",\n";
        out << "      \"character\": " << json_escape(s.character) << ",\n";
        out << "      \"account\": " << json_escape(s.account) << ",\n";
        out << "      \"server_id\": " << json_escape(s.server_id) << ",\n";
        out << "      \"world\": " << json_escape(s.world) << ",\n";
        out << "      \"class_role\": " << json_escape(s.class_role) << ",\n";
        out << "      \"settings_path\": " << json_escape(normalize_settings_for_storage(s.settings_path)) << ",\n";
        out << "      \"group_leader\": " << json_escape(s.group_leader) << ",\n";
        out << "      \"follow_target\": " << json_escape(s.follow_target) << ",\n";
        out << "      \"combat_profile\": " << json_escape(s.combat_profile) << ",\n";
        out << "      \"script_module\": " << json_escape(s.script_module) << ",\n";
        out << "      \"script_settings\": " << json_escape(s.script_settings) << ",\n";
        out << "      \"spell_loadout\": " << json_escape(s.spell_loadout) << ",\n";
        out << "      \"auto_login\": " << (s.auto_login ? "true" : "false") << ",\n";
        out << "      \"headless\": " << (s.headless ? "true" : "false") << ",\n";
        out << "      \"auto_kick\": " << (s.auto_kick ? "true" : "false") << ",\n";
        out << "      \"bg_tick_hz\": " << s.bg_tick_hz << ",\n";
        out << "      \"gpu_mode\": " << json_escape(s.gpu_mode) << "\n";
        out << "    }" << (i + 1 < norm.slots.size() ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";

    bool ok = false;
    write_atomic_text(path, out.str(), error, ok);
    return ok;
}

const GroupSlot* resolve_group_slot(const GroupProfile& profile, std::string_view slot_or_name) {
    if (profile.slots.empty()) return nullptr;
    const std::string key = trim_copy(slot_or_name);
    if (key.empty()) {
        for (const auto& s : profile.slots) {
            if (s.enabled) return &s;
        }
        return &profile.slots.front();
    }

    if (auto num = parse_strict_int(key)) {
        for (const auto& s : profile.slots) {
            if (s.slot_number == *num) return &s;
        }
    }
    for (const auto& s : profile.slots) {
        if (!s.session_id.empty() && ieq(s.session_id, key)) return &s;
    }
    for (const auto& s : profile.slots) {
        if (!s.character.empty() && ieq(s.character, key)) return &s;
    }
    for (const auto& s : profile.slots) {
        if (!s.account.empty() && ieq(s.account, key)) return &s;
    }
    return nullptr;
}

std::optional<fs::path> resolve_safe_settings_path(const fs::path& data_dir, const fs::path& requested) {
    if (data_dir.empty() || requested.empty()) return std::nullopt;
    if (has_dotdot_component(requested)) return std::nullopt;

    std::error_code ec;
    const fs::path base_norm = data_dir.lexically_normal();
    fs::path candidate = requested.is_absolute() ? requested.lexically_normal() : (base_norm / requested).lexically_normal();

    if (has_dotdot_component(candidate)) return std::nullopt;
    const fs::path rel = candidate.lexically_relative(base_norm);
    if (rel.empty() || rel == "." || has_dotdot_component(rel) || rel.is_absolute()) {
        return std::nullopt;
    }

    if (fs::exists(base_norm, ec)) {
        const fs::path base_canon = fs::weakly_canonical(base_norm, ec);
        if (!ec) {
            const fs::path cand_canon = fs::weakly_canonical(candidate, ec);
            if (!ec) {
                const fs::path canon_rel = cand_canon.lexically_relative(base_canon);
                if (canon_rel.empty() || canon_rel == "." || has_dotdot_component(canon_rel) ||
                    canon_rel.is_absolute()) {
                    return std::nullopt;
                }
            }
        }
    }
    return candidate;
}

std::string default_slot_settings_relpath(std::string_view server_id, std::string_view world,
                                          std::string_view character) {
    const std::string srv = sanitize_path_component(server_id, "default");
    const std::string wrd = sanitize_path_component(world, "default");
    const std::string ch = sanitize_path_component(character, "character");
    return (fs::path("servers") / srv / "characters" / wrd / ch / "hailstone.ini").generic_string();
}

std::optional<fs::path> resolve_effective_settings_path(const fs::path& data_dir,
                                                        const GroupProfile& profile,
                                                        const GroupSlot& slot) {
    if (!slot.settings_path.empty()) {
        if (auto safe = resolve_safe_settings_path(data_dir, slot.settings_path)) {
            return safe;
        }
        return std::nullopt;
    }
    const std::string srv = !slot.server_id.empty() ? slot.server_id : profile.default_server_id;
    const std::string wrd = !slot.world.empty() ? slot.world : profile.default_world;
    const std::string ch = !slot.character.empty() ? slot.character : (!slot.account.empty() ? slot.account : slot.session_id);
    if (srv.empty() && wrd.empty() && ch.empty()) return std::nullopt;
    return resolve_safe_settings_path(data_dir, default_slot_settings_relpath(srv, wrd, ch));
}

bool resolve_pre_autologin_profile(const fs::path& profiles_dir,
                                   const fs::path& data_dir,
                                   std::string_view profile_arg,
                                   std::string_view slot_arg,
                                   std::string_view configured_group_profile,
                                   PreLoginProfileState& state,
                                   GroupProfile* out_profile,
                                   GroupSlot* out_slot,
                                   std::string* out_error) {
    const std::string requested_profile = trim_copy(profile_arg);
    const std::string active_name =
        !requested_profile.empty()
            ? requested_profile
            : ((state.auto_login && state.server.empty() && state.account.empty() && state.character.empty())
                   ? trim_copy(configured_group_profile)
                   : std::string());

    if (active_name.empty()) {
        if (!state.settings_path.empty()) {
            auto safe = resolve_safe_settings_path(data_dir, state.settings_path);
            if (!safe) {
                if (out_error) *out_error = "Unsafe --settings-path outside Hailstone data directory.";
                state.settings_path.clear();
                return false;
            }
            state.settings_path = safe->string();
        }
        return true;
    }

    const auto path = find_group_profile_file(profiles_dir, active_name);
    if (!path) {
        if (!requested_profile.empty()) {
            if (out_error) *out_error = "Group profile not found: " + requested_profile;
            return false;
        }
        return true;
    }

    GroupProfile gp;
    std::string load_err;
    if (!load_group_profile(*path, gp, &load_err)) {
        if (out_error) *out_error = load_err;
        return false;
    }
    if (gp.slots.empty()) {
        if (out_error) *out_error = "Group profile '" + gp.name + "' defines no slots.";
        return false;
    }

    const GroupSlot* slot = resolve_group_slot(gp, !slot_arg.empty() ? slot_arg : state.slot);
    if (!slot) {
        if (out_error) *out_error = "Slot '" + std::string(slot_arg) + "' not found in profile '" + gp.name + "'.";
        return false;
    }

    state.profile = gp.id;
    state.slot = slot->session_id;
    if (state.server.empty()) state.server = !slot->server_id.empty() ? slot->server_id : gp.default_server_id;
    if (state.world.empty()) state.world = !slot->world.empty() ? slot->world : gp.default_world;
    if (state.account.empty()) state.account = slot->account;
    if (state.character.empty()) state.character = slot->character;
    if (state.session_id.empty()) state.session_id = slot->session_id;
    if (state.gpu_mode.empty()) state.gpu_mode = slot->gpu_mode;
    if (state.follow_target.empty()) state.follow_target = slot->follow_target;
    if (state.combat_profile.empty()) state.combat_profile = slot->combat_profile;
    if (state.script_module.empty()) state.script_module = slot->script_module;
    if (state.spell_loadout.empty()) state.spell_loadout = slot->spell_loadout;
    if (state.bg_tick_hz <= 0) state.bg_tick_hz = slot->bg_tick_hz;

    state.auto_login = state.auto_login || slot->auto_login;
    state.auto_kick = state.auto_kick || slot->auto_kick;
    if (!requested_profile.empty() || !slot_arg.empty()) {
        state.headless_bg = state.headless_bg || slot->headless;
    }

    if (!state.settings_path.empty()) {
        auto safe = resolve_safe_settings_path(data_dir, state.settings_path);
        if (!safe) {
            if (out_error) *out_error = "Unsafe settings_path outside Hailstone data directory.";
            state.settings_path.clear();
            return false;
        }
        state.settings_path = safe->string();
    } else if (auto eff = resolve_effective_settings_path(data_dir, gp, *slot)) {
        state.settings_path = eff->string();
    } else if (!slot->settings_path.empty()) {
        if (out_error) *out_error = "Slot settings_path escapes Hailstone data directory: " + slot->settings_path;
        return false;
    }

    if (out_profile) *out_profile = gp;
    if (out_slot) *out_slot = *slot;
    return true;
}

namespace {
std::string infer_combat_profile_from_class(std::string_view cls) {
    const std::string c = lower_copy(trim_copy(cls));
    if (c == "war" || c == "warrior" || c == "pal" || c == "paladin" || c == "shd" || c == "sk" ||
        c == "shadowknight" || c == "tank") {
        return "Tank";
    }
    if (c == "clr" || c == "cleric" || c == "dru" || c == "druid" || c == "shm" || c == "shaman" ||
        c == "healer") {
        return "Healer";
    }
    if (c == "wiz" || c == "wizard" || c == "mag" || c == "mage" || c == "magician" || c == "nec" ||
        c == "necromancer" || c == "enc" || c == "enchanter" || c == "caster") {
        return "CasterDPS";
    }
    if (c == "brd" || c == "bard") return "Support";
    if (c == "mnk" || c == "monk") return "Puller";
    return "MeleeDPS";
}

std::string infer_spell_loadout_from_class(std::string_view cls) {
    const std::string c = lower_copy(trim_copy(cls));
    if (c == "war" || c == "warrior" || c == "rog" || c == "rogue" || c == "mnk" || c == "monk" ||
        c == "ber" || c == "berserker") {
        return "melee_default";
    }
    if (c == "clr" || c == "cleric" || c == "dru" || c == "druid" || c == "shm" || c == "shaman" ||
        c == "healer") {
        return "healer_group";
    }
    if (c == "pal" || c == "paladin" || c == "shd" || c == "sk" || c == "tank") return "tank_aggro";
    if (c == "wiz" || c == "mag" || c == "nec" || c == "enc" || c == "caster") return "caster_nuke";
    return "default";
}
} // namespace

std::vector<std::string> default_group_profile_preset_names() {
    return {"Main Tank", "Caster Group", "Melee Group", "Raid", "Solo"};
}

GroupProfile make_named_group_profile_preset(std::string_view preset_name,
                                             std::string_view server_id,
                                             std::string_view world) {
    const std::string low = lower_copy(trim_copy(preset_name));
    GroupProfile gp;
    gp.default_server_id = std::string(server_id);
    gp.default_world = std::string(world);
    gp.auto_login = true;
    gp.auto_kick = true;
    gp.bg_tick_hz = 25;
    gp.gpu_mode = "cached";

    auto add_slot = [&](int num, std::string_view role, std::string_view combat_prof,
                        std::string_view spells, bool headless, std::string_view follow,
                        std::string_view script_mod = "rgmercs") {
        GroupSlot s;
        s.slot_number = num;
        s.launch_order = num;
        s.enabled = true;
        s.session_id = format_slot_id(num);
        s.server_id = gp.default_server_id;
        s.world = gp.default_world;
        s.class_role = std::string(role);
        s.combat_profile = std::string(combat_prof);
        s.spell_loadout = std::string(spells);
        s.follow_target = std::string(follow);
        s.script_module = std::string(script_mod);
        s.headless = headless;
        s.auto_login = true;
        s.auto_kick = true;
        s.bg_tick_hz = gp.bg_tick_hz;
        s.gpu_mode = gp.gpu_mode;
        gp.slots.push_back(std::move(s));
    };

    if (low == "main tank" || low == "main-tank" || low == "main_tank" || low == "tank") {
        gp.id = "main-tank";
        gp.name = "Main Tank";
        gp.preset_kind = "Main Tank";
        gp.description = "Main tank lead with dedicated cleric, slow/buff support, and melee/caster DPS.";
        add_slot(1, "WAR", "Tank", "melee_default", false, "");
        add_slot(2, "CLR", "Healer", "healer_group", true, "slot01");
        add_slot(3, "SHM", "Healer", "healer_group", true, "slot01");
        add_slot(4, "BRD", "Support", "support_songs", true, "slot01");
        add_slot(5, "ROG", "MeleeDPS", "melee_default", true, "slot01");
        add_slot(6, "WIZ", "CasterDPS", "caster_nuke", true, "slot01");
    } else if (low == "caster group" || low == "caster-group" || low == "caster_group" || low == "caster") {
        gp.id = "caster-group";
        gp.name = "Caster Group";
        gp.preset_kind = "Caster Group";
        gp.description = "Caster-focused 6-box with tank anchor, enchanter mana regen, healer, and nuke/pet DPS.";
        add_slot(1, "SHD", "Tank", "tank_aggro", false, "");
        add_slot(2, "CLR", "Healer", "healer_group", true, "slot01");
        add_slot(3, "ENC", "Support", "caster_cc", true, "slot01");
        add_slot(4, "WIZ", "CasterDPS", "caster_nuke", true, "slot01");
        add_slot(5, "MAG", "CasterDPS", "caster_nuke", true, "slot01");
        add_slot(6, "NEC", "CasterDPS", "caster_dot", true, "slot01");
    } else if (low == "melee group" || low == "melee-group" || low == "melee_group" || low == "melee") {
        gp.id = "melee-group";
        gp.name = "Melee Group";
        gp.preset_kind = "Melee Group";
        gp.description = "High-DPS melee 6-box with warrior tank, rogue/monk/berserker DPS, bard, and shaman.";
        add_slot(1, "WAR", "Tank", "melee_default", false, "");
        add_slot(2, "ROG", "MeleeDPS", "melee_default", true, "slot01");
        add_slot(3, "MNK", "Puller", "melee_default", true, "slot01");
        add_slot(4, "BER", "MeleeDPS", "melee_default", true, "slot01");
        add_slot(5, "BRD", "Support", "support_songs", true, "slot01");
        add_slot(6, "SHM", "Healer", "healer_group", true, "slot01");
    } else if (low == "raid") {
        gp.id = "raid";
        gp.name = "Raid";
        gp.preset_kind = "Raid";
        gp.description = "18-box multi-group raid roster with tank, melee, and caster wings.";
        gp.bg_tick_hz = 20;
        const std::vector<std::tuple<const char*, const char*, const char*>> roles = {
            {"WAR", "Tank", "melee_default"},      {"CLR", "Healer", "healer_group"},
            {"SHM", "Healer", "healer_group"},     {"BRD", "Support", "support_songs"},
            {"PAL", "Tank", "tank_aggro"},         {"ROG", "MeleeDPS", "melee_default"},
            {"SHD", "Tank", "tank_aggro"},         {"MNK", "Puller", "melee_default"},
            {"BER", "MeleeDPS", "melee_default"},  {"ROG", "MeleeDPS", "melee_default"},
            {"BST", "MeleeDPS", "melee_default"},  {"DRU", "Healer", "healer_group"},
            {"ENC", "Support", "caster_cc"},       {"WIZ", "CasterDPS", "caster_nuke"},
            {"WIZ", "CasterDPS", "caster_nuke"},   {"MAG", "CasterDPS", "caster_nuke"},
            {"NEC", "CasterDPS", "caster_dot"},    {"CLR", "Healer", "healer_group"},
        };
        for (std::size_t i = 0; i < roles.size(); ++i) {
            const auto& [cls, prof, spells] = roles[i];
            add_slot(static_cast<int>(i + 1), cls, prof, spells, i != 0, i == 0 ? "" : "slot01");
        }
    } else {
        gp.id = "solo";
        gp.name = "Solo";
        gp.preset_kind = "Solo";
        gp.description = "Single foreground client with native combat controller.";
        add_slot(1, "ROG", "Solo", "default", false, "", "");
    }
    return gp;
}

std::vector<fs::path> export_group_profiles_from_mq(const std::vector<MqImportedAccount>& records,
                                                    std::string_view server_id,
                                                    std::string_view fallback_world,
                                                    std::string_view default_profile_name,
                                                    const fs::path& profiles_dir,
                                                    std::vector<std::string>* warnings) {
    std::vector<fs::path> written;
    if (profiles_dir.empty() || records.empty()) return written;

    // Group imported MQ records by profile_group (defaulting to default_profile_name / "default").
    struct Bucket {
        std::string display_name;
        std::vector<MqImportedAccount> entries;
    };
    std::map<std::string, Bucket, std::less<>> buckets;
    for (const auto& rec : records) {
        if (rec.account.empty()) continue;
        std::string gname = trim_copy(rec.profile_group);
        if (gname.empty()) gname = default_profile_name.empty() ? std::string("default") : std::string(default_profile_name);
        const std::string key = sanitize_group_profile_id(gname);
        auto& b = buckets[key];
        if (b.display_name.empty()) b.display_name = gname;
        // Strip password copy unconditionally when constructing group profile buckets.
        MqImportedAccount clean = rec;
        clean.password.clear();
        b.entries.push_back(std::move(clean));
    }

    for (auto& [id, bucket] : buckets) {
        std::stable_sort(bucket.entries.begin(), bucket.entries.end(),
                         [](const MqImportedAccount& a, const MqImportedAccount& b) {
                             if (a.sort_order != b.sort_order) return a.sort_order < b.sort_order;
                             return a.account < b.account;
                         });

        GroupProfile gp;
        gp.id = id;
        gp.name = bucket.display_name;
        for (const auto& preset : default_group_profile_preset_names()) {
            if (ieq(preset, gp.name)) {
                gp.preset_kind = preset;
                break;
            }
        }
        gp.description = "Imported from MacroQuest AutoLogin (keyring credentials only)";
        gp.default_server_id = std::string(server_id);
        gp.default_world = std::string(fallback_world);
        gp.auto_login = true;
        gp.auto_kick = true;

        std::set<std::string> seen;
        int slot_idx = 0;
        for (const auto& r : bucket.entries) {
            const std::string uniq = lower_copy(r.account) + "|" + lower_copy(r.character);
            if (!seen.insert(uniq).second) continue;
            if (slot_idx >= 24) break;
            ++slot_idx;

            GroupSlot s;
            s.slot_number = slot_idx;
            s.launch_order = r.sort_order > 0 ? r.sort_order : slot_idx;
            s.enabled = r.enabled;
            s.session_id = format_slot_id(slot_idx);
            s.character = r.character;
            s.account = r.account;
            s.server_id = std::string(server_id);
            s.world = !r.server_name.empty() ? r.server_name : std::string(fallback_world);
            s.class_role = r.character_class;
            s.combat_profile = infer_combat_profile_from_class(r.character_class);
            s.spell_loadout = infer_spell_loadout_from_class(r.character_class);
            s.settings_path = default_slot_settings_relpath(s.server_id, s.world, s.character);
            s.auto_login = true;
            s.headless = (slot_idx != 1);
            s.auto_kick = true;
            if (!gp.slots.empty()) {
                s.group_leader = gp.slots.front().character;
                s.follow_target = gp.slots.front().character;
            } else {
                s.group_leader = s.character;
            }
            gp.slots.push_back(std::move(s));
        }

        const fs::path out_path = profiles_dir / (id + ".ini");
        std::string err;
        if (save_group_profile_ini(gp, out_path, &err)) {
            written.push_back(out_path);
        } else if (warnings) {
            warnings->push_back("Failed to write group profile '" + id + "': " + err);
        }
    }

    return written;
}

bool import_mq_profiles_from_ini(const fs::path& mq_ini_path,
                                 std::string_view server_id,
                                 std::string_view fallback_world,
                                 const fs::path& profiles_dir,
                                 std::vector<fs::path>& out_written,
                                 std::vector<std::string>* warnings) {
    out_written.clear();
    IniFile ini;
    if (!ini.load(mq_ini_path)) {
        if (warnings) warnings->push_back("Cannot read MacroQuest profile INI: " + mq_ini_path.string());
        return false;
    }

    // Group sections into profile buckets. Supports:
    //   [Profile:<Name>] / [Group:<Name>] / [<ProfileName>] with MemberN / Character / Account / Role / Follow / Script / Spells
    //   or flat [SessionN] / [SlotN] sections with optional Profile=<Name>.
    std::map<std::string, GroupProfile, std::less<>> profiles;
    auto get_or_create = [&](const std::string& display_name) -> GroupProfile& {
        const std::string id = sanitize_group_profile_id(display_name);
        auto& gp = profiles[id];
        if (gp.id.empty()) {
            gp.id = id;
            gp.name = display_name;
            gp.description = "Imported from MacroQuest profile INI (credentials stay in keyring)";
            gp.default_server_id = std::string(server_id);
            gp.default_world = std::string(fallback_world);
            for (const auto& preset : default_group_profile_preset_names()) {
                if (ieq(preset, display_name)) {
                    gp.preset_kind = preset;
                    break;
                }
            }
        }
        return gp;
    };

    auto safe_get = [&](std::string_view sec, std::string_view key) -> std::string {
        if (is_secret_key(key)) return {};
        return trim_copy(ini.get_string(sec, key));
    };

    std::vector<std::string> declared_groups;
    for (const std::string& key : ini.keys("Profiles")) {
        const std::string val = trim_copy(ini.get_string("Profiles", key));
        if (!val.empty() && val.find('^') == std::string::npos) declared_groups.push_back(val);
    }
    auto resolve_declared_group = [&](std::string_view sec) -> std::string {
        for (const auto& g : declared_groups) {
            if (ieq(sec, g)) return g;
            if (sec.size() > g.size() + 1 && ieq(sec.substr(0, g.size()), g) &&
                (sec[g.size()] == '_' || sec[g.size()] == '-')) {
                return g;
            }
        }
        return {};
    };

    for (const std::string& sec : ini.sections()) {
        if (ieq(sec, "Settings") || ieq(sec, "Profiles")) continue;
        // Check if this section defines a single character/slot entry.
        const std::string ch = safe_get(sec, "Character");
        std::string acct = safe_get(sec, "Account");
        if (acct.empty()) acct = safe_get(sec, "StationName");
        if (!ch.empty() || !acct.empty()) {
            std::string group_name = safe_get(sec, "Profile");
            if (group_name.empty()) group_name = safe_get(sec, "Group");
            if (group_name.empty()) group_name = resolve_declared_group(sec);
            if (group_name.empty()) group_name = mq_ini_path.stem().string();

            auto& gp = get_or_create(group_name);
            const int idx = static_cast<int>(gp.slots.size()) + 1;
            if (idx > 24) continue;

            GroupSlot s;
            s.slot_number = std::clamp(ini.get_int(sec, "Slot", idx), 1, 24);
            s.launch_order = ini.get_int(sec, "StartupOrder", ini.get_int(sec, "Order", idx));
            s.enabled = parse_bool_str(safe_get(sec, "Enabled"), true);
            s.session_id = format_slot_id(s.slot_number);
            s.character = ch;
            s.account = acct;
            s.server_id = std::string(server_id);
            const std::string srv = safe_get(sec, "Server");
            s.world = !srv.empty() ? srv : std::string(fallback_world);
            s.class_role = safe_get(sec, "Class");
            if (s.class_role.empty()) s.class_role = safe_get(sec, "Role");
            s.follow_target = safe_get(sec, "FollowTarget");
            if (s.follow_target.empty()) s.follow_target = safe_get(sec, "Follow");
            s.combat_profile = safe_get(sec, "CombatProfile");
            if (s.combat_profile.empty()) s.combat_profile = infer_combat_profile_from_class(s.class_role);
            s.script_module = safe_get(sec, "ScriptModule");
            if (s.script_module.empty()) s.script_module = safe_get(sec, "Script");
            s.script_settings = safe_get(sec, "ScriptSettings");
            s.spell_loadout = safe_get(sec, "SpellLoadout");
            if (s.spell_loadout.empty()) s.spell_loadout = safe_get(sec, "Spells");
            if (s.spell_loadout.empty()) s.spell_loadout = infer_spell_loadout_from_class(s.class_role);
            s.settings_path = default_slot_settings_relpath(s.server_id, s.world, s.character);
            const std::string headless_str = safe_get(sec, "Headless");
            if (!headless_str.empty()) {
                s.headless = parse_bool_str(headless_str, idx != 1);
            } else {
                const std::string live_str = safe_get(sec, "Live");
                s.headless = !live_str.empty() ? !parse_bool_str(live_str, idx == 1) : (idx != 1);
            }
            s.group_leader = safe_get(sec, "GroupLeader");
            if (s.group_leader.empty()) {
                s.group_leader = gp.slots.empty() ? s.character : gp.slots.front().character;
            }
            gp.slots.push_back(std::move(s));
        }
    }

    for (auto& [id, gp] : profiles) {
        if (gp.slots.empty()) continue;
        const fs::path out_path = profiles_dir / (id + ".ini");
        std::string err;
        if (save_group_profile_ini(gp, out_path, &err)) {
            out_written.push_back(out_path);
        } else if (warnings) {
            warnings->push_back("Failed to write imported profile '" + id + "': " + err);
        }
    }
    return !out_written.empty();
}

} // namespace hailstone::core
