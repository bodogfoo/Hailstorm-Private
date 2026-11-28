#pragma once

// Minimal strict JSON value, parser, and string escaper for Hailstone configuration
// and data-driven quest/profile stores.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace hailstone::core {

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

    std::string as_string(std::string_view fallback = {}) const;
    bool as_bool(bool fallback) const;
    int as_int(int fallback) const;
    std::uint32_t as_u32(std::uint32_t fallback) const;
    std::uint64_t as_u64(std::uint64_t fallback) const;
    double as_double(double fallback) const;

    const JsonValue* find_ci(std::string_view key) const;
    const JsonValue* find(std::string_view key) const;
};

bool parse_json(std::string_view src, JsonValue& out, std::string& err);
std::string json_escape(std::string_view s);

} // namespace hailstone::core
