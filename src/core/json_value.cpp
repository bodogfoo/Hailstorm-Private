#include "core/json_value.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

#include "core/ascii_case.h"

namespace hailstone::core {
namespace {

bool parse_bool_str(std::string_view s, bool fallback) {
    if (iequals(s, "true") || iequals(s, "yes") || s == "1" || iequals(s, "on")) return true;
    if (iequals(s, "false") || iequals(s, "no") || s == "0" || iequals(s, "off")) return false;
    return fallback;
}

class JsonParser {
public:
    explicit JsonParser(std::string_view src) : s_(src) {}

    bool parse(JsonValue& out, std::string& err) {
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
            const char ch = s_[pos_++];
            if (ch == '"') return true;
            if (ch == '\\') {
                if (pos_ >= s_.size()) {
                    err = "unterminated escape in JSON string";
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
                        err = "short \\uXXXX escape";
                        return false;
                    }
                    unsigned int cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char hc = s_[pos_++];
                        cp <<= 4;
                        if (hc >= '0' && hc <= '9') cp |= static_cast<unsigned int>(hc - '0');
                        else if (hc >= 'a' && hc <= 'f') cp |= static_cast<unsigned int>(hc - 'a' + 10);
                        else if (hc >= 'A' && hc <= 'F') cp |= static_cast<unsigned int>(hc - 'A' + 10);
                        else {
                            err = "invalid hex digit in \\u escape";
                            return false;
                        }
                    }
                    if (cp < 0x80) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
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
                    err = std::format("unknown escape '\\{}'", esc);
                    return false;
                }
            } else if (static_cast<unsigned char>(ch) < 0x20) {
                err = "unescaped control character in JSON string";
                return false;
            } else {
                out.push_back(ch);
            }
        }
        err = "unterminated JSON string";
        return false;
    }

    bool parse_number(JsonValue& out, std::string& err) {
        const std::size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        if (pos_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
            err = "invalid JSON number";
            return false;
        }
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (pos_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
                err = "invalid JSON number fraction";
                return false;
            }
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (pos_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
                err = "invalid JSON number exponent";
                return false;
            }
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        }
        const std::string num_str(s_.substr(start, pos_ - start));
        double val = 0.0;
        auto [ptr, ec] = std::from_chars(num_str.data(), num_str.data() + num_str.size(), val);
        if (ec != std::errc{} || !std::isfinite(val)) {
            err = "invalid JSON number";
            return false;
        }
        out.data = val;
        return true;
    }

    bool parse_array(JsonValue& out, std::string& err) {
        ++pos_;
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
                err = std::format("expected ',' or ']' in JSON array at offset {}", pos_);
                return false;
            }
            ++pos_;
        }
    }

    bool parse_object(JsonValue& out, std::string& err) {
        ++pos_;
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
                err = std::format("expected ':' after key '{}' at offset {}", key, pos_);
                return false;
            }
            ++pos_;
            JsonValue val;
            if (!parse_value(val, err)) return false;
            obj.insert_or_assign(std::move(key), std::move(val));
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
                err = std::format("expected ',' or '}}' in JSON object at offset {}", pos_);
                return false;
            }
            ++pos_;
        }
    }
};

} // namespace

std::string JsonValue::as_string(std::string_view fallback) const {
    if (const auto* s = std::get_if<std::string>(&data)) return *s;
    if (const auto* b = std::get_if<bool>(&data)) return *b ? "true" : "false";
    if (const auto* d = std::get_if<double>(&data)) return std::to_string(static_cast<long long>(*d));
    return std::string(fallback);
}

bool JsonValue::as_bool(bool fallback) const {
    if (const auto* b = std::get_if<bool>(&data)) return *b;
    if (const auto* d = std::get_if<double>(&data)) return *d != 0.0;
    if (const auto* s = std::get_if<std::string>(&data)) return parse_bool_str(*s, fallback);
    return fallback;
}

int JsonValue::as_int(int fallback) const {
    if (const auto* d = std::get_if<double>(&data)) return static_cast<int>(*d);
    if (const auto* b = std::get_if<bool>(&data)) return *b ? 1 : 0;
    if (const auto* s = std::get_if<std::string>(&data)) {
        int val = 0;
        auto [ptr, ec] = std::from_chars(s->data(), s->data() + s->size(), val);
        if (ec == std::errc{} && ptr == s->data() + s->size()) return val;
    }
    return fallback;
}

std::uint32_t JsonValue::as_u32(std::uint32_t fallback) const {
    if (const auto* d = std::get_if<double>(&data)) {
        if (*d >= 0.0 && *d <= 4294967295.0) return static_cast<std::uint32_t>(*d);
    }
    if (const auto* s = std::get_if<std::string>(&data)) {
        std::uint32_t val = 0;
        auto [ptr, ec] = std::from_chars(s->data(), s->data() + s->size(), val);
        if (ec == std::errc{} && ptr == s->data() + s->size()) return val;
    }
    return fallback;
}

std::uint64_t JsonValue::as_u64(std::uint64_t fallback) const {
    if (const auto* d = std::get_if<double>(&data)) {
        if (*d >= 0.0) return static_cast<std::uint64_t>(*d);
    }
    if (const auto* s = std::get_if<std::string>(&data)) {
        std::uint64_t val = 0;
        auto [ptr, ec] = std::from_chars(s->data(), s->data() + s->size(), val);
        if (ec == std::errc{} && ptr == s->data() + s->size()) return val;
    }
    return fallback;
}

double JsonValue::as_double(double fallback) const {
    if (const auto* d = std::get_if<double>(&data)) return *d;
    if (const auto* b = std::get_if<bool>(&data)) return *b ? 1.0 : 0.0;
    return fallback;
}

const JsonValue* JsonValue::find_ci(std::string_view key) const {
    const auto* obj = std::get_if<JsonObject>(&data);
    if (!obj) return nullptr;
    for (const auto& [k, v] : *obj) {
        if (iequals(k, key)) return &v;
    }
    return nullptr;
}

const JsonValue* JsonValue::find(std::string_view key) const {
    const auto* obj = std::get_if<JsonObject>(&data);
    if (!obj) return nullptr;
    auto it = obj->find(key);
    return it == obj->end() ? nullptr : &it->second;
}

bool parse_json(std::string_view src, JsonValue& out, std::string& err) {
    JsonParser p(src);
    return p.parse(out, err);
}

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

} // namespace hailstone::core
