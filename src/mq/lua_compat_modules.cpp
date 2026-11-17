#include "mq/lua_compat_modules.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sqlite3.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <utime.h>
#endif

namespace hailstone::mq {
namespace fs = std::filesystem;

namespace {

constexpr const char* kCompatEnvKey = "hailstone.compat.env";
constexpr const char* kPreloadedKey = "hailstone.preloaded";
constexpr const char* kActorHandlersKey = "hailstone.compat.actors";
constexpr const char* kSqliteDbMeta = "hailstone.lsqlite3.db";
constexpr const char* kSqliteStmtMeta = "hailstone.lsqlite3.stmt";
constexpr const char* kSandboxFileMeta = "hailstone.io.file";

inline constexpr std::size_t kSandboxMaxFileBytes = 8u << 20;
inline constexpr std::size_t kSandboxMaxFiles = 1000;
inline constexpr std::uintmax_t kSandboxMaxBytes = 64u << 20;

struct CompatHolder {
    CompatEnv env;
};

CompatHolder* get_compat(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kCompatEnvKey);
    auto* h = static_cast<CompatHolder*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return h;
}

int compat_gc(lua_State* L) {
    auto* h = static_cast<CompatHolder*>(lua_touserdata(L, 1));
    if (h) h->~CompatHolder();
    return 0;
}

// ---------------------------------------------------------------- Sandbox path & quota guards

enum class PathAccess { Read, Write };

std::string ascii_lower_trim_part(std::string_view s) {
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.remove_suffix(1);
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool is_config_segment(const fs::path& rel) {
    if (rel.empty()) return false;
    return ascii_lower_trim_part(rel.begin()->generic_string()) == "config";
}

bool check_under_root(const fs::path& root, const fs::path& given_path, bool allow_root, bool reject_config_subdir,
                      fs::path& out) {
    if (root.empty()) return false;
    std::error_code ec;
    fs::path canon_root = fs::weakly_canonical(root, ec);
    if (ec || canon_root.empty()) canon_root = root.lexically_normal();

    fs::path cand = given_path.is_relative() ? (canon_root / given_path) : given_path;
    fs::path canon_cand = fs::weakly_canonical(cand, ec);
    if (ec) return false;

    fs::path rel = canon_cand.lexically_relative(canon_root);
    if (rel.empty() || rel == ".") {
        if (!allow_root) return false;
        out = std::move(canon_cand);
        return true;
    }
    if (rel.is_absolute()) return false;
    if (*rel.begin() == "..") return false;
    if (reject_config_subdir && is_config_segment(rel)) return false;
    out = std::move(canon_cand);
    return true;
}

std::optional<fs::path> resolve_sandbox_path(lua_State* L, std::string_view given, PathAccess access, bool allow_root,
                                             std::string& why) {
    if (given.empty() || given.find('\0') != std::string_view::npos) {
        why = "invalid path";
        return std::nullopt;
    }
    CompatHolder* h = get_compat(L);
    if (!h) {
        why = "sandbox environment unavailable";
        return std::nullopt;
    }
    if (access == PathAccess::Write && h->env.write_blocked && h->env.write_blocked()) {
        why = "write blocked while dry-running or stopping";
        return std::nullopt;
    }

    fs::path gp(given);
    fs::path resolved;
    if (check_under_root(h->env.config_dir, gp, allow_root, false, resolved)) {
        return resolved;
    }
    if (access == PathAccess::Write) {
        why = "write path is outside Hailstone mq.configDir";
        return std::nullopt;
    }

    std::vector<fs::path> ro_roots;
    if (!h->env.script_root.empty()) ro_roots.push_back(h->env.script_root);
    for (const auto& d : h->env.read_dirs) {
        if (!d.empty()) ro_roots.push_back(d);
    }

    if (gp.is_relative()) {
        // For relative reads, prefer an existing file/dir under read-only script roots if not already handled,
        // otherwise default to config_dir (so missing reads report ENOENT cleanly if confined there).
        for (const auto& r : ro_roots) {
            fs::path cand;
            std::error_code ec;
            if (check_under_root(r, gp, allow_root, true, cand) && fs::exists(cand, ec)) {
                return cand;
            }
        }
        if (check_under_root(h->env.config_dir, gp, allow_root, false, resolved)) {
            return resolved;
        }
    } else {
        for (const auto& r : ro_roots) {
            if (check_under_root(r, gp, allow_root, true, resolved)) {
                return resolved;
            }
        }
    }

    why = "path is outside Hailstone sandbox";
    return std::nullopt;
}

struct SandboxDirUsage {
    std::size_t files = 0;
    std::uintmax_t bytes = 0;
};

SandboxDirUsage sandbox_dir_usage(const fs::path& dir, const fs::path& except) {
    SandboxDirUsage u;
    std::error_code ec;
    fs::path root = fs::weakly_canonical(dir, ec);
    if (ec || root.empty()) root = dir;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && u.files <= kSandboxMaxFiles; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec) || it->path() == except) continue;
        ++u.files;
        const auto sz = it->file_size(fec);
        if (!fec) u.bytes += sz;
    }
    return u;
}

std::FILE* open_stdio_file(const fs::path& p, const char* mode) {
#if defined(_WIN32)
    std::wstring wmode;
    for (const char* q = mode; *q; ++q) wmode.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*q)));
    return ::_wfopen(p.c_str(), wmode.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

// ---------------------------------------------------------------- JSON codec (for actors)

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 4);
    out.push_back('"');
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
            if (c < 0x20) out += std::format("\\u{:04x}", c);
            else out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
    return out;
}

bool is_lua_array(lua_State* L, int idx, lua_Integer& max_idx) {
    max_idx = 0;
    lua_Integer count = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        lua_pop(L, 1);
        if (lua_type(L, -1) != LUA_TNUMBER || !lua_isinteger(L, -1)) {
            lua_pop(L, 1);
            return false;
        }
        lua_Integer k = lua_tointeger(L, -1);
        if (k <= 0) {
            lua_pop(L, 1);
            return false;
        }
        if (k > max_idx) max_idx = k;
        ++count;
    }
    return count > 0 && max_idx == count;
}

std::string to_json_impl(lua_State* L, int idx, int depth, std::unordered_set<const void*>& seen) {
    idx = lua_absindex(L, idx);
    if (depth > 32) return "null";
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
    case LUA_TNONE: return "null";
    case LUA_TBOOLEAN: return lua_toboolean(L, idx) ? "true" : "false";
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) return std::to_string(lua_tointeger(L, idx));
        {
            double d = lua_tonumber(L, idx);
            if (!std::isfinite(d)) return "null";
            return std::format("{}", d);
        }
    case LUA_TSTRING: {
        std::size_t len = 0;
        const char* p = lua_tolstring(L, idx, &len);
        return json_escape(std::string_view(p, len));
    }
    case LUA_TTABLE: {
        const void* ptr = lua_topointer(L, idx);
        if (!seen.insert(ptr).second) return "null";
        std::string out;
        lua_Integer arr_len = 0;
        if (is_lua_array(L, idx, arr_len)) {
            out.push_back('[');
            for (lua_Integer i = 1; i <= arr_len; ++i) {
                if (i > 1) out.push_back(',');
                lua_rawgeti(L, idx, i);
                out += to_json_impl(L, -1, depth + 1, seen);
                lua_pop(L, 1);
            }
            out.push_back(']');
        } else {
            out.push_back('{');
            bool first = true;
            lua_pushnil(L);
            while (lua_next(L, idx) != 0) {
                std::string k;
                if (lua_type(L, -2) == LUA_TSTRING) {
                    std::size_t len = 0;
                    const char* p = lua_tolstring(L, -2, &len);
                    k.assign(p, len);
                } else if (lua_type(L, -2) == LUA_TNUMBER) {
                    k = lua_isinteger(L, -2) ? std::to_string(lua_tointeger(L, -2))
                                             : std::format("{}", lua_tonumber(L, -2));
                } else {
                    lua_pop(L, 1);
                    continue;
                }
                if (!first) out.push_back(',');
                first = false;
                out += json_escape(k);
                out.push_back(':');
                out += to_json_impl(L, -1, depth + 1, seen);
                lua_pop(L, 1);
            }
            out.push_back('}');
        }
        seen.erase(ptr);
        return out;
    }
    default: return "null";
    }
}

struct JsonParser {
    std::string_view src;
    std::size_t pos = 0;

    void skip_ws() {
        while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) ++pos;
    }

    bool parse_string(std::string& out) {
        if (pos >= src.size() || src[pos] != '"') return false;
        ++pos;
        out.clear();
        while (pos < src.size()) {
            char c = src[pos++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos >= src.size()) return false;
                char e = src[pos++];
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos + 4 > src.size()) return false;
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = src[pos++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else return false;
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
                default: return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool parse_value(lua_State* L, int depth) {
        if (depth > 32) return false;
        skip_ws();
        if (pos >= src.size()) return false;
        char c = src[pos];
        if (c == 'n' && src.substr(pos, 4) == "null") {
            pos += 4;
            lua_pushnil(L);
            return true;
        }
        if (c == 't' && src.substr(pos, 4) == "true") {
            pos += 4;
            lua_pushboolean(L, 1);
            return true;
        }
        if (c == 'f' && src.substr(pos, 5) == "false") {
            pos += 5;
            lua_pushboolean(L, 0);
            return true;
        }
        if (c == '"') {
            std::string s;
            if (!parse_string(s)) return false;
            lua_pushlstring(L, s.data(), s.size());
            return true;
        }
        if (c == '[') {
            ++pos;
            lua_newtable(L);
            skip_ws();
            if (pos < src.size() && src[pos] == ']') {
                ++pos;
                return true;
            }
            lua_Integer idx = 1;
            while (pos < src.size()) {
                if (!parse_value(L, depth + 1)) return false;
                lua_rawseti(L, -2, idx++);
                skip_ws();
                if (pos < src.size() && src[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < src.size() && src[pos] == ']') {
                    ++pos;
                    return true;
                }
                return false;
            }
            return false;
        }
        if (c == '{') {
            ++pos;
            lua_newtable(L);
            skip_ws();
            if (pos < src.size() && src[pos] == '}') {
                ++pos;
                return true;
            }
            while (pos < src.size()) {
                skip_ws();
                std::string k;
                if (!parse_string(k)) return false;
                skip_ws();
                if (pos >= src.size() || src[pos] != ':') return false;
                ++pos;
                if (!parse_value(L, depth + 1)) return false;
                lua_pushlstring(L, k.data(), k.size());
                lua_insert(L, -2);
                lua_rawset(L, -3);
                skip_ws();
                if (pos < src.size() && src[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < src.size() && src[pos] == '}') {
                    ++pos;
                    return true;
                }
                return false;
            }
            return false;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            std::size_t start = pos;
            bool is_float = false;
            if (src[pos] == '-') ++pos;
            while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
            if (pos < src.size() && src[pos] == '.') {
                is_float = true;
                ++pos;
                while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
            }
            if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
                is_float = true;
                ++pos;
                if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) ++pos;
                while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
            }
            std::string num_str(src.substr(start, pos - start));
            char* endp = nullptr;
            if (!is_float) {
                long long v = std::strtoll(num_str.c_str(), &endp, 10);
                if (endp == num_str.c_str() + num_str.size()) {
                    lua_pushinteger(L, static_cast<lua_Integer>(v));
                    return true;
                }
            }
            double d = std::strtod(num_str.c_str(), &endp);
            if (endp == num_str.c_str() + num_str.size()) {
                lua_pushnumber(L, d);
                return true;
            }
            return false;
        }
        return false;
    }
};

// ---------------------------------------------------------------- bit32

std::uint32_t to_u32(lua_State* L, int idx) {
    return static_cast<std::uint32_t>(luaL_checkinteger(L, idx));
}

int l_bit32_band(lua_State* L) {
    std::uint32_t acc = 0xFFFFFFFFu;
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) acc &= to_u32(L, i);
    lua_pushinteger(L, static_cast<lua_Integer>(acc));
    return 1;
}

int l_bit32_bor(lua_State* L) {
    std::uint32_t acc = 0;
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) acc |= to_u32(L, i);
    lua_pushinteger(L, static_cast<lua_Integer>(acc));
    return 1;
}

int l_bit32_bxor(lua_State* L) {
    std::uint32_t acc = 0;
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) acc ^= to_u32(L, i);
    lua_pushinteger(L, static_cast<lua_Integer>(acc));
    return 1;
}

int l_bit32_bnot(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(~to_u32(L, 1)));
    return 1;
}

int l_bit32_lshift(lua_State* L) {
    std::uint32_t x = to_u32(L, 1);
    lua_Integer disp = luaL_checkinteger(L, 2);
    std::uint32_t out = 0;
    if (disp >= 0 && disp < 32) out = x << disp;
    else if (disp < 0 && disp > -32) out = x >> (-disp);
    lua_pushinteger(L, static_cast<lua_Integer>(out));
    return 1;
}

int l_bit32_rshift(lua_State* L) {
    std::uint32_t x = to_u32(L, 1);
    lua_Integer disp = luaL_checkinteger(L, 2);
    std::uint32_t out = 0;
    if (disp >= 0 && disp < 32) out = x >> disp;
    else if (disp < 0 && disp > -32) out = x << (-disp);
    lua_pushinteger(L, static_cast<lua_Integer>(out));
    return 1;
}

int l_bit32_arshift(lua_State* L) {
    std::int32_t x = static_cast<std::int32_t>(to_u32(L, 1));
    lua_Integer disp = luaL_checkinteger(L, 2);
    std::uint32_t out = 0;
    if (disp >= 0 && disp < 32) out = static_cast<std::uint32_t>(x >> disp);
    else if (disp >= 32) out = x < 0 ? 0xFFFFFFFFu : 0u;
    else if (disp > -32) out = static_cast<std::uint32_t>(x) << (-disp);
    lua_pushinteger(L, static_cast<lua_Integer>(out));
    return 1;
}

int l_bit32_btest(lua_State* L) {
    std::uint32_t acc = 0xFFFFFFFFu;
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) acc &= to_u32(L, i);
    lua_pushboolean(L, acc != 0);
    return 1;
}

int l_bit32_extract(lua_State* L) {
    std::uint32_t x = to_u32(L, 1);
    lua_Integer f = luaL_checkinteger(L, 2);
    lua_Integer w = luaL_optinteger(L, 3, 1);
    if (f < 0 || w <= 0 || f + w > 32) return luaL_error(L, "bit32.extract: field out of range");
    std::uint32_t mask = w == 32 ? 0xFFFFFFFFu : ((1u << w) - 1u);
    lua_pushinteger(L, static_cast<lua_Integer>((x >> f) & mask));
    return 1;
}

int l_bit32_replace(lua_State* L) {
    std::uint32_t x = to_u32(L, 1);
    std::uint32_t v = to_u32(L, 2);
    lua_Integer f = luaL_checkinteger(L, 3);
    lua_Integer w = luaL_optinteger(L, 4, 1);
    if (f < 0 || w <= 0 || f + w > 32) return luaL_error(L, "bit32.replace: field out of range");
    std::uint32_t mask = w == 32 ? 0xFFFFFFFFu : ((1u << w) - 1u);
    x = (x & ~(mask << f)) | ((v & mask) << f);
    lua_pushinteger(L, static_cast<lua_Integer>(x));
    return 1;
}

int luaopen_bit32(lua_State* L) {
    static const luaL_Reg kFuncs[] = {
        {"band", l_bit32_band},     {"bor", l_bit32_bor},       {"bxor", l_bit32_bxor},
        {"bnot", l_bit32_bnot},     {"lshift", l_bit32_lshift}, {"rshift", l_bit32_rshift},
        {"arshift", l_bit32_arshift}, {"btest", l_bit32_btest}, {"extract", l_bit32_extract},
        {"replace", l_bit32_replace}, {nullptr, nullptr},
    };
    luaL_newlib(L, kFuncs);
    return 1;
}

// ---------------------------------------------------------------- lsqlite3

struct LuaSqliteStmt;

struct LuaSqliteDb {
    sqlite3* db = nullptr;
    lua_State* L = nullptr;
    int update_cb_ref = LUA_NOREF;
    int update_ud_ref = LUA_NOREF;
    std::vector<LuaSqliteStmt*> stmts;

    void clear_hook() {
        if (db) sqlite3_update_hook(db, nullptr, nullptr);
        if (L) {
            if (update_cb_ref != LUA_NOREF) {
                luaL_unref(L, LUA_REGISTRYINDEX, update_cb_ref);
                update_cb_ref = LUA_NOREF;
            }
            if (update_ud_ref != LUA_NOREF) {
                luaL_unref(L, LUA_REGISTRYINDEX, update_ud_ref);
                update_ud_ref = LUA_NOREF;
            }
        }
    }
};

struct LuaSqliteStmt {
    sqlite3_stmt* stmt = nullptr;
    LuaSqliteDb* owner = nullptr;
    bool finalized = false;

    void finish() {
        if (stmt) {
            sqlite3_finalize(stmt);
            stmt = nullptr;
        }
        finalized = true;
    }
};

LuaSqliteDb* check_db(lua_State* L, int idx = 1) {
    return static_cast<LuaSqliteDb*>(luaL_checkudata(L, idx, kSqliteDbMeta));
}

LuaSqliteStmt* check_stmt(lua_State* L, int idx = 1) {
    return static_cast<LuaSqliteStmt*>(luaL_checkudata(L, idx, kSqliteStmtMeta));
}

void push_sqlite_col(lua_State* L, sqlite3_stmt* stmt, int col) {
    switch (sqlite3_column_type(stmt, col)) {
    case SQLITE_INTEGER: lua_pushinteger(L, static_cast<lua_Integer>(sqlite3_column_int64(stmt, col))); break;
    case SQLITE_FLOAT: lua_pushnumber(L, sqlite3_column_double(stmt, col)); break;
    case SQLITE_TEXT: {
        const char* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
        int bytes = sqlite3_column_bytes(stmt, col);
        lua_pushlstring(L, txt ? txt : "", static_cast<std::size_t>(bytes));
        break;
    }
    case SQLITE_BLOB: {
        const char* blob = static_cast<const char*>(sqlite3_column_blob(stmt, col));
        int bytes = sqlite3_column_bytes(stmt, col);
        lua_pushlstring(L, blob ? blob : "", static_cast<std::size_t>(bytes));
        break;
    }
    case SQLITE_NULL:
    default: lua_pushnil(L); break;
    }
}

int bind_sqlite_value(lua_State* L, sqlite3_stmt* stmt, int param_idx, int lua_idx) {
    switch (lua_type(L, lua_idx)) {
    case LUA_TNIL: return sqlite3_bind_null(stmt, param_idx);
    case LUA_TBOOLEAN: return sqlite3_bind_int(stmt, param_idx, lua_toboolean(L, lua_idx) ? 1 : 0);
    case LUA_TNUMBER:
        if (lua_isinteger(L, lua_idx))
            return sqlite3_bind_int64(stmt, param_idx, static_cast<sqlite3_int64>(lua_tointeger(L, lua_idx)));
        return sqlite3_bind_double(stmt, param_idx, lua_tonumber(L, lua_idx));
    case LUA_TSTRING: {
        std::size_t len = 0;
        const char* s = lua_tolstring(L, lua_idx, &len);
        return sqlite3_bind_text(stmt, param_idx, s, static_cast<int>(len), SQLITE_TRANSIENT);
    }
    default: return SQLITE_MISUSE;
    }
}

void sqlite_update_trampoline(void* ud, int op, const char* db_name, const char* table_name, sqlite3_int64 rowid) {
    auto* d = static_cast<LuaSqliteDb*>(ud);
    if (!d || !d->L || d->update_cb_ref == LUA_NOREF) return;
    lua_State* L = d->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, d->update_cb_ref);
    if (d->update_ud_ref != LUA_NOREF) lua_rawgeti(L, LUA_REGISTRYINDEX, d->update_ud_ref);
    else lua_pushnil(L);
    lua_pushinteger(L, op);
    lua_pushstring(L, db_name ? db_name : "");
    lua_pushstring(L, table_name ? table_name : "");
    lua_pushinteger(L, static_cast<lua_Integer>(rowid));
    if (lua_pcall(L, 5, 0, 0) != LUA_OK) {
        lua_pop(L, 1);
    }
}

int l_sqlite_stmt_step(lua_State* L) {
    auto* st = check_stmt(L);
    if (!st->stmt) return luaL_error(L, "attempt to use finalized statement");
    lua_pushinteger(L, sqlite3_step(st->stmt));
    return 1;
}

int l_sqlite_stmt_reset(lua_State* L) {
    auto* st = check_stmt(L);
    if (!st->stmt) return luaL_error(L, "attempt to use finalized statement");
    lua_pushinteger(L, sqlite3_reset(st->stmt));
    return 1;
}

int l_sqlite_stmt_finalize(lua_State* L) {
    auto* st = check_stmt(L);
    st->finish();
    lua_pushinteger(L, SQLITE_OK);
    return 1;
}

int l_sqlite_stmt_isopen(lua_State* L) {
    auto* st = check_stmt(L);
    lua_pushboolean(L, st->stmt != nullptr && !st->finalized);
    return 1;
}

int l_sqlite_stmt_bind(lua_State* L) {
    auto* st = check_stmt(L);
    if (!st->stmt) return luaL_error(L, "attempt to use finalized statement");
    int idx = static_cast<int>(luaL_checkinteger(L, 2));
    int rc = bind_sqlite_value(L, st->stmt, idx, 3);
    lua_pushinteger(L, rc);
    return 1;
}

int l_sqlite_stmt_bind_values(lua_State* L) {
    auto* st = check_stmt(L);
    if (!st->stmt) return luaL_error(L, "attempt to use finalized statement");
    int n = lua_gettop(L);
    int rc = SQLITE_OK;
    for (int i = 2; i <= n; ++i) {
        rc = bind_sqlite_value(L, st->stmt, i - 1, i);
        if (rc != SQLITE_OK) break;
    }
    lua_pushinteger(L, rc);
    return 1;
}

int l_sqlite_stmt_bind_names(lua_State* L) {
    auto* st = check_stmt(L);
    if (!st->stmt) return luaL_error(L, "attempt to use finalized statement");
    luaL_checktype(L, 2, LUA_TTABLE);
    int n = sqlite3_bind_parameter_count(st->stmt);
    int rc = SQLITE_OK;
    for (int i = 1; i <= n; ++i) {
        const char* name = sqlite3_bind_parameter_name(st->stmt, i);
        if (!name) continue;
        std::string_view sv(name);
        if (!sv.empty() && (sv.front() == ':' || sv.front() == '@' || sv.front() == '$')) sv.remove_prefix(1);
        lua_pushlstring(L, sv.data(), sv.size());
        lua_gettable(L, 2);
        rc = bind_sqlite_value(L, st->stmt, i, -1);
        lua_pop(L, 1);
        if (rc != SQLITE_OK) break;
    }
    lua_pushinteger(L, rc);
    return 1;
}

int l_sqlite_stmt_columns(lua_State* L) {
    auto* st = check_stmt(L);
    if (!st->stmt) return luaL_error(L, "attempt to use finalized statement");
    lua_pushinteger(L, sqlite3_column_count(st->stmt));
    return 1;
}

int l_sqlite_stmt_get_value(lua_State* L) {
    auto* st = check_stmt(L);
    if (!st->stmt) return luaL_error(L, "attempt to use finalized statement");
    int col = static_cast<int>(luaL_checkinteger(L, 2));
    if (col < 0 || col >= sqlite3_column_count(st->stmt)) {
        lua_pushnil(L);
        return 1;
    }
    push_sqlite_col(L, st->stmt, col);
    return 1;
}

int l_sqlite_stmt_get_values(lua_State* L) {
    auto* st = check_stmt(L);
    if (!st->stmt) return luaL_error(L, "attempt to use finalized statement");
    int cols = sqlite3_column_count(st->stmt);
    lua_createtable(L, cols, 0);
    for (int i = 0; i < cols; ++i) {
        push_sqlite_col(L, st->stmt, i);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int l_sqlite_stmt_get_named_values(lua_State* L) {
    auto* st = check_stmt(L);
    if (!st->stmt) return luaL_error(L, "attempt to use finalized statement");
    int cols = sqlite3_column_count(st->stmt);
    lua_createtable(L, 0, cols);
    for (int i = 0; i < cols; ++i) {
        const char* name = sqlite3_column_name(st->stmt, i);
        lua_pushstring(L, name ? name : "");
        push_sqlite_col(L, st->stmt, i);
        lua_rawset(L, -3);
    }
    return 1;
}

int l_sqlite_stmt_get_names(lua_State* L) {
    auto* st = check_stmt(L);
    if (!st->stmt) return luaL_error(L, "attempt to use finalized statement");
    int cols = sqlite3_column_count(st->stmt);
    lua_createtable(L, cols, 0);
    for (int i = 0; i < cols; ++i) {
        const char* name = sqlite3_column_name(st->stmt, i);
        lua_pushstring(L, name ? name : "");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int l_sqlite_stmt_nrows_iter(lua_State* L) {
    auto* st = check_stmt(L, lua_upvalueindex(1));
    if (!st->stmt) return 0;
    if (sqlite3_step(st->stmt) == SQLITE_ROW) {
        int cols = sqlite3_column_count(st->stmt);
        lua_createtable(L, 0, cols);
        for (int i = 0; i < cols; ++i) {
            const char* name = sqlite3_column_name(st->stmt, i);
            lua_pushstring(L, name ? name : "");
            push_sqlite_col(L, st->stmt, i);
            lua_rawset(L, -3);
        }
        return 1;
    }
    sqlite3_reset(st->stmt);
    return 0;
}

int l_sqlite_stmt_rows_iter(lua_State* L) {
    auto* st = check_stmt(L, lua_upvalueindex(1));
    if (!st->stmt) return 0;
    if (sqlite3_step(st->stmt) == SQLITE_ROW) {
        int cols = sqlite3_column_count(st->stmt);
        lua_createtable(L, cols, 0);
        for (int i = 0; i < cols; ++i) {
            push_sqlite_col(L, st->stmt, i);
            lua_rawseti(L, -2, i + 1);
        }
        return 1;
    }
    sqlite3_reset(st->stmt);
    return 0;
}

int l_sqlite_stmt_urows_iter(lua_State* L) {
    auto* st = check_stmt(L, lua_upvalueindex(1));
    if (!st->stmt) return 0;
    if (sqlite3_step(st->stmt) == SQLITE_ROW) {
        int cols = sqlite3_column_count(st->stmt);
        for (int i = 0; i < cols; ++i) push_sqlite_col(L, st->stmt, i);
        return cols;
    }
    sqlite3_reset(st->stmt);
    return 0;
}

int l_sqlite_stmt_nrows(lua_State* L) {
    check_stmt(L);
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, l_sqlite_stmt_nrows_iter, 1);
    return 1;
}

int l_sqlite_stmt_rows(lua_State* L) {
    check_stmt(L);
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, l_sqlite_stmt_rows_iter, 1);
    return 1;
}

int l_sqlite_stmt_urows(lua_State* L) {
    check_stmt(L);
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, l_sqlite_stmt_urows_iter, 1);
    return 1;
}

int l_sqlite_stmt_gc(lua_State* L) {
    auto* st = static_cast<LuaSqliteStmt*>(lua_touserdata(L, 1));
    if (st) {
        if (st->owner) {
            std::erase(st->owner->stmts, st);
            st->owner = nullptr;
        }
        st->finish();
    }
    return 0;
}

int l_sqlite_db_prepare(lua_State* L) {
    auto* d = check_db(L);
    if (!d->db) return luaL_error(L, "attempt to use closed sqlite3 database");
    std::size_t len = 0;
    const char* sql = luaL_checklstring(L, 2, &len);
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(d->db, sql, static_cast<int>(len), &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt) {
        lua_pushnil(L);
        lua_pushstring(L, sqlite3_errmsg(d->db));
        return 2;
    }
    auto* st = static_cast<LuaSqliteStmt*>(lua_newuserdatauv(L, sizeof(LuaSqliteStmt), 0));
    new (st) LuaSqliteStmt{stmt, d, false};
    d->stmts.push_back(st);
    luaL_setmetatable(L, kSqliteStmtMeta);
    return 1;
}

int l_sqlite_db_exec(lua_State* L) {
    auto* d = check_db(L);
    if (!d->db) return luaL_error(L, "attempt to use closed sqlite3 database");
    const char* sql = luaL_checkstring(L, 2);
    char* errmsg = nullptr;
    int rc = SQLITE_OK;
    if (lua_isfunction(L, 3)) {
        struct ExecCtx {
            lua_State* L;
            int fn_idx;
            int ud_idx;
        } ctx{L, 3, lua_gettop(L) >= 4 ? 4 : 0};
        rc = sqlite3_exec(
            d->db, sql,
            [](void* arg, int argc, char** argv, char** col_names) -> int {
                auto* c = static_cast<ExecCtx*>(arg);
                lua_pushvalue(c->L, c->fn_idx);
                if (c->ud_idx) lua_pushvalue(c->L, c->ud_idx);
                else lua_pushnil(c->L);
                lua_pushinteger(c->L, argc);
                lua_createtable(c->L, argc, 0);
                lua_createtable(c->L, argc, 0);
                for (int i = 0; i < argc; ++i) {
                    if (argv[i]) lua_pushstring(c->L, argv[i]);
                    else lua_pushnil(c->L);
                    lua_rawseti(c->L, -3, i + 1);
                    lua_pushstring(c->L, col_names[i] ? col_names[i] : "");
                    lua_rawseti(c->L, -2, i + 1);
                }
                if (lua_pcall(c->L, 4, 1, 0) != LUA_OK) {
                    lua_pop(c->L, 1);
                    return 1;
                }
                int res = lua_isinteger(c->L, -1) ? static_cast<int>(lua_tointeger(c->L, -1)) : 0;
                lua_pop(c->L, 1);
                return res;
            },
            &ctx, &errmsg);
    } else {
        rc = sqlite3_exec(d->db, sql, nullptr, nullptr, &errmsg);
    }
    if (errmsg) sqlite3_free(errmsg);
    lua_pushinteger(L, rc);
    return 1;
}

int l_sqlite_db_nrows(lua_State* L) {
    l_sqlite_db_prepare(L);
    if (lua_isnil(L, -2)) return luaL_error(L, "sqlite3.nrows: %s", lua_tostring(L, -1));
    lua_pushcclosure(L, l_sqlite_stmt_nrows_iter, 1);
    return 1;
}

int l_sqlite_db_urows(lua_State* L) {
    l_sqlite_db_prepare(L);
    if (lua_isnil(L, -2)) return luaL_error(L, "sqlite3.urows: %s", lua_tostring(L, -1));
    lua_pushcclosure(L, l_sqlite_stmt_urows_iter, 1);
    return 1;
}

int l_sqlite_db_rows(lua_State* L) {
    l_sqlite_db_prepare(L);
    if (lua_isnil(L, -2)) return luaL_error(L, "sqlite3.rows: %s", lua_tostring(L, -1));
    lua_pushcclosure(L, l_sqlite_stmt_rows_iter, 1);
    return 1;
}

int l_sqlite_db_update_hook(lua_State* L) {
    auto* d = check_db(L);
    if (!d->db) return luaL_error(L, "attempt to use closed sqlite3 database");
    d->clear_hook();
    d->L = L;
    if (lua_isfunction(L, 2)) {
        lua_pushvalue(L, 2);
        d->update_cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
            lua_pushvalue(L, 3);
            d->update_ud_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        sqlite3_update_hook(d->db, sqlite_update_trampoline, d);
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_sqlite_db_close(lua_State* L) {
    auto* d = check_db(L);
    if (d->db) {
        d->clear_hook();
        for (auto* st : d->stmts) {
            st->finish();
            st->owner = nullptr;
        }
        d->stmts.clear();
        int rc = sqlite3_close_v2(d->db);
        d->db = nullptr;
        lua_pushinteger(L, rc);
        return 1;
    }
    lua_pushinteger(L, SQLITE_OK);
    return 1;
}

int l_sqlite_db_gc(lua_State* L) {
    auto* d = static_cast<LuaSqliteDb*>(lua_touserdata(L, 1));
    if (d) {
        d->clear_hook();
        for (auto* st : d->stmts) {
            st->finish();
            st->owner = nullptr;
        }
        d->stmts.clear();
        if (d->db) {
            sqlite3_close_v2(d->db);
            d->db = nullptr;
        }
        d->~LuaSqliteDb();
    }
    return 0;
}

int l_sqlite_open_impl(lua_State* L, const char* path, int flags) {
    sqlite3* db = nullptr;
    int safe_flags = flags & ~SQLITE_OPEN_URI;
    int rc = sqlite3_open_v2(path, &db, safe_flags, nullptr);
    if (rc != SQLITE_OK || !db) {
        std::string msg = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
        if (db) sqlite3_close_v2(db);
        lua_pushnil(L);
        lua_pushinteger(L, rc);
        lua_pushstring(L, msg.c_str());
        return 3;
    }
    auto* d = static_cast<LuaSqliteDb*>(lua_newuserdatauv(L, sizeof(LuaSqliteDb), 0));
    new (d) LuaSqliteDb{};
    d->db = db;
    d->L = L;
    luaL_setmetatable(L, kSqliteDbMeta);
    return 1;
}

int l_sqlite_open(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    int flags = static_cast<int>(luaL_optinteger(L, 2, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE));
    if (std::strcmp(path, ":memory:") == 0) {
        return l_sqlite_open_impl(L, path, flags);
    }
    if ((flags & SQLITE_OPEN_URI) != 0 || std::strncmp(path, "file:", 5) == 0) {
        lua_pushnil(L);
        lua_pushinteger(L, SQLITE_PERM);
        lua_pushstring(L, "sqlite3 URI filenames are disabled in Hailstone sandbox");
        return 3;
    }
    const bool wants_write = (flags & (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) != 0;
    std::string why;
    auto resolved = resolve_sandbox_path(L, path, wants_write ? PathAccess::Write : PathAccess::Read, false, why);
    if (!resolved) {
        lua_pushnil(L);
        lua_pushinteger(L, SQLITE_PERM);
        lua_pushstring(L, why.c_str());
        return 3;
    }
    if (wants_write) {
        CompatHolder* h = get_compat(L);
        if (h && !h->env.config_dir.empty()) {
            std::error_code ec;
            fs::create_directories(h->env.config_dir, ec);
        }
    }
    std::string resolved_str = resolved->string();
    return l_sqlite_open_impl(L, resolved_str.c_str(), flags);
}

int l_sqlite_open_memory(lua_State* L) {
    return l_sqlite_open_impl(L, ":memory:", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MEMORY);
}

int luaopen_lsqlite3(lua_State* L) {
    if (luaL_newmetatable(L, kSqliteStmtMeta)) {
        static const luaL_Reg kStmtMethods[] = {
            {"step", l_sqlite_stmt_step},
            {"reset", l_sqlite_stmt_reset},
            {"finalize", l_sqlite_stmt_finalize},
            {"isopen", l_sqlite_stmt_isopen},
            {"bind", l_sqlite_stmt_bind},
            {"bind_values", l_sqlite_stmt_bind_values},
            {"bind_names", l_sqlite_stmt_bind_names},
            {"columns", l_sqlite_stmt_columns},
            {"get_value", l_sqlite_stmt_get_value},
            {"get_values", l_sqlite_stmt_get_values},
            {"get_named_values", l_sqlite_stmt_get_named_values},
            {"get_names", l_sqlite_stmt_get_names},
            {"nrows", l_sqlite_stmt_nrows},
            {"rows", l_sqlite_stmt_rows},
            {"urows", l_sqlite_stmt_urows},
            {nullptr, nullptr},
        };
        luaL_newlib(L, kStmtMethods);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_sqlite_stmt_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushliteral(L, "lsqlite3.stmt");
        lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, kSqliteDbMeta)) {
        static const luaL_Reg kDbMethods[] = {
            {"exec", l_sqlite_db_exec},
            {"prepare", l_sqlite_db_prepare},
            {"nrows", l_sqlite_db_nrows},
            {"urows", l_sqlite_db_urows},
            {"rows", l_sqlite_db_rows},
            {"update_hook", l_sqlite_db_update_hook},
            {"close", l_sqlite_db_close},
            {"isopen", [](lua_State* l) { lua_pushboolean(l, check_db(l)->db != nullptr); return 1; }},
            {"changes", [](lua_State* l) { auto* d = check_db(l); lua_pushinteger(l, d->db ? sqlite3_changes(d->db) : 0); return 1; }},
            {"total_changes", [](lua_State* l) { auto* d = check_db(l); lua_pushinteger(l, d->db ? sqlite3_total_changes(d->db) : 0); return 1; }},
            {"last_insert_rowid", [](lua_State* l) { auto* d = check_db(l); lua_pushinteger(l, d->db ? sqlite3_last_insert_rowid(d->db) : 0); return 1; }},
            {"errmsg", [](lua_State* l) { auto* d = check_db(l); lua_pushstring(l, d->db ? sqlite3_errmsg(d->db) : "closed"); return 1; }},
            {"errcode", [](lua_State* l) { auto* d = check_db(l); lua_pushinteger(l, d->db ? sqlite3_errcode(d->db) : SQLITE_MISUSE); return 1; }},
            {"busy_timeout", [](lua_State* l) { auto* d = check_db(l); int ms = static_cast<int>(luaL_checkinteger(l, 2)); lua_pushinteger(l, d->db ? sqlite3_busy_timeout(d->db, ms) : SQLITE_MISUSE); return 1; }},
            {nullptr, nullptr},
        };
        luaL_newlib(L, kDbMethods);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_sqlite_db_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushliteral(L, "lsqlite3.db");
        lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);

    static const luaL_Reg kMod[] = {
        {"open", l_sqlite_open},
        {"open_memory", l_sqlite_open_memory},
        {"version", [](lua_State* l) { lua_pushstring(l, sqlite3_libversion()); return 1; }},
        {nullptr, nullptr},
    };
    luaL_newlib(L, kMod);

    struct Const { const char* name; int val; };
    for (Const c : {
             Const{"OK", SQLITE_OK}, Const{"ERROR", SQLITE_ERROR}, Const{"PERM", SQLITE_PERM},
             Const{"ABORT", SQLITE_ABORT}, Const{"BUSY", SQLITE_BUSY}, Const{"LOCKED", SQLITE_LOCKED},
             Const{"NOMEM", SQLITE_NOMEM}, Const{"READONLY", SQLITE_READONLY}, Const{"INTERRUPT", SQLITE_INTERRUPT},
             Const{"IOERR", SQLITE_IOERR}, Const{"CORRUPT", SQLITE_CORRUPT}, Const{"NOTFOUND", SQLITE_NOTFOUND},
             Const{"FULL", SQLITE_FULL}, Const{"CANTOPEN", SQLITE_CANTOPEN}, Const{"PROTOCOL", SQLITE_PROTOCOL},
             Const{"EMPTY", SQLITE_EMPTY}, Const{"SCHEMA", SQLITE_SCHEMA}, Const{"TOOBIG", SQLITE_TOOBIG},
             Const{"CONSTRAINT", SQLITE_CONSTRAINT}, Const{"MISMATCH", SQLITE_MISMATCH}, Const{"MISUSE", SQLITE_MISUSE},
             Const{"NOLFS", SQLITE_NOLFS}, Const{"AUTH", SQLITE_AUTH}, Const{"FORMAT", SQLITE_FORMAT},
             Const{"RANGE", SQLITE_RANGE}, Const{"NOTADB", SQLITE_NOTADB},
             Const{"ROW", SQLITE_ROW}, Const{"DONE", SQLITE_DONE},
             Const{"OPEN_READONLY", SQLITE_OPEN_READONLY}, Const{"OPEN_READWRITE", SQLITE_OPEN_READWRITE},
             Const{"OPEN_CREATE", SQLITE_OPEN_CREATE}, Const{"OPEN_NOMUTEX", SQLITE_OPEN_NOMUTEX},
             Const{"OPEN_FULLMUTEX", SQLITE_OPEN_FULLMUTEX}, Const{"OPEN_SHAREDCACHE", SQLITE_OPEN_SHAREDCACHE},
             Const{"OPEN_PRIVATECACHE", SQLITE_OPEN_PRIVATECACHE},
             Const{"INSERT", SQLITE_INSERT}, Const{"UPDATE", SQLITE_UPDATE}, Const{"DELETE", SQLITE_DELETE},
         }) {
        lua_pushinteger(L, c.val);
        lua_setfield(L, -2, c.name);
    }
    return 1;
}

// ---------------------------------------------------------------- lfs (sandbox-confined)

int l_lfs_attributes(lua_State* L) {
    const char* raw_path = luaL_checkstring(L, 1);
    const char* attr = luaL_optstring(L, 2, nullptr);
    std::string why;
    auto resolved = resolve_sandbox_path(L, raw_path, PathAccess::Read, true, why);
    if (!resolved) {
        lua_pushnil(L);
        lua_pushstring(L, why.c_str());
        return 2;
    }
    const fs::path& path = *resolved;
    std::error_code ec;
    fs::file_status st = fs::status(path, ec);
    if (ec || !fs::exists(st)) {
        lua_pushnil(L);
        lua_pushstring(L, ec ? ec.message().c_str() : "cannot obtain information from file");
        return 2;
    }
    const char* mode = "other";
    if (fs::is_regular_file(st)) mode = "file";
    else if (fs::is_directory(st)) mode = "directory";
    else if (fs::is_symlink(st)) mode = "link";

    std::uintmax_t sz = fs::is_regular_file(st) ? fs::file_size(path, ec) : 0;
    std::int64_t mtime = 0;
#if !defined(_WIN32)
    struct stat sb{};
    if (::stat(path.c_str(), &sb) == 0) mtime = static_cast<std::int64_t>(sb.st_mtime);
#endif

    if (attr) {
        if (std::strcmp(attr, "mode") == 0) lua_pushstring(L, mode);
        else if (std::strcmp(attr, "size") == 0) lua_pushinteger(L, static_cast<lua_Integer>(sz));
        else if (std::strcmp(attr, "modification") == 0) lua_pushinteger(L, static_cast<lua_Integer>(mtime));
        else if (std::strcmp(attr, "access") == 0) lua_pushinteger(L, static_cast<lua_Integer>(mtime));
        else if (std::strcmp(attr, "change") == 0) lua_pushinteger(L, static_cast<lua_Integer>(mtime));
        else lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);
    lua_pushstring(L, mode);
    lua_setfield(L, -2, "mode");
    lua_pushinteger(L, static_cast<lua_Integer>(sz));
    lua_setfield(L, -2, "size");
    lua_pushinteger(L, static_cast<lua_Integer>(mtime));
    lua_setfield(L, -2, "modification");
    lua_pushinteger(L, static_cast<lua_Integer>(mtime));
    lua_setfield(L, -2, "access");
    lua_pushinteger(L, static_cast<lua_Integer>(mtime));
    lua_setfield(L, -2, "change");
    return 1;
}

struct LfsDirIter {
    std::vector<std::string> entries;
    std::size_t idx = 0;
};

int l_lfs_dir_next(lua_State* L) {
    auto* it = static_cast<LfsDirIter*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!it || it->idx >= it->entries.size()) return 0;
    const std::string& name = it->entries[it->idx++];
    lua_pushlstring(L, name.data(), name.size());
    return 1;
}

int l_lfs_dir_gc(lua_State* L) {
    auto* it = static_cast<LfsDirIter*>(lua_touserdata(L, 1));
    if (it) it->~LfsDirIter();
    return 0;
}

int l_lfs_dir(lua_State* L) {
    const char* raw_path = luaL_checkstring(L, 1);
    std::string why;
    auto resolved = resolve_sandbox_path(L, raw_path, PathAccess::Read, true, why);
    if (!resolved) return luaL_error(L, "cannot open %s: %s", raw_path, why.c_str());

    std::error_code ec;
    fs::directory_iterator dit(*resolved, ec);
    if (ec) return luaL_error(L, "cannot open %s: %s", raw_path, ec.message().c_str());

    auto* it = static_cast<LfsDirIter*>(lua_newuserdatauv(L, sizeof(LfsDirIter), 0));
    new (it) LfsDirIter{};
    it->entries.emplace_back(".");
    it->entries.emplace_back("..");
    for (const auto& e : dit) {
        it->entries.push_back(e.path().filename().string());
    }
    lua_newtable(L);
    lua_pushcfunction(L, l_lfs_dir_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushliteral(L, "lfs.dir");
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    lua_pushcclosure(L, l_lfs_dir_next, 1);
    return 1;
}

int l_lfs_mkdir(lua_State* L) {
    const char* raw_path = luaL_checkstring(L, 1);
    // RG Mercs calls mkdir while walking an already-created config path. Treat
    // an existing directory as a successful no-op before resolving the write
    // operation; no filesystem mutation occurs in this case.
    std::error_code existing_ec;
    if (fs::is_directory(fs::path(raw_path), existing_ec)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    std::string why;
    auto resolved = resolve_sandbox_path(L, raw_path, PathAccess::Write, true, why);
    if (!resolved) {
        lua_pushnil(L);
        lua_pushstring(L, why.c_str());
        return 2;
    }
    CompatHolder* h = get_compat(L);
    if (h && !h->env.config_dir.empty()) {
        std::error_code cec;
        fs::create_directories(h->env.config_dir, cec);
    }
    std::error_code ec;
    bool ok = fs::create_directory(*resolved, ec);
    if (ok) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, ec ? ec.message().c_str() : "File exists");
    return 2;
}

int l_lfs_rmdir(lua_State* L) {
    const char* raw_path = luaL_checkstring(L, 1);
    std::string why;
    auto resolved = resolve_sandbox_path(L, raw_path, PathAccess::Write, false, why);
    if (!resolved) {
        lua_pushnil(L);
        lua_pushstring(L, why.c_str());
        return 2;
    }
    std::error_code ec;
    bool ok = fs::remove(*resolved, ec);
    if (ok) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, ec ? ec.message().c_str() : "cannot remove directory");
    return 2;
}

int l_lfs_currentdir(lua_State* L) {
    CompatHolder* h = get_compat(L);
    if (!h || h->env.config_dir.empty()) {
        lua_pushnil(L);
        lua_pushstring(L, "config directory unavailable");
        return 2;
    }
    std::error_code ec;
    fs::path p = fs::weakly_canonical(h->env.config_dir, ec);
    if (ec || p.empty()) p = h->env.config_dir;
    std::string s = p.string();
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}

int l_lfs_chdir(lua_State* L) {
    const char* raw_path = luaL_checkstring(L, 1);
    CompatHolder* h = get_compat(L);
    if (h && !h->env.config_dir.empty()) {
        std::string why;
        auto resolved = resolve_sandbox_path(L, raw_path, PathAccess::Read, true, why);
        std::error_code ec;
        fs::path root = fs::weakly_canonical(h->env.config_dir, ec);
        if (ec || root.empty()) root = h->env.config_dir;
        if (resolved && *resolved == root) {
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushnil(L);
    lua_pushstring(L, "chdir is disabled in Hailstone sandbox");
    return 2;
}

int l_lfs_touch(lua_State* L) {
    const char* raw_path = luaL_checkstring(L, 1);
    std::string why;
    auto resolved = resolve_sandbox_path(L, raw_path, PathAccess::Write, false, why);
    if (!resolved) {
        lua_pushnil(L);
        lua_pushstring(L, why.c_str());
        return 2;
    }
#if !defined(_WIN32)
    if (::utime(resolved->c_str(), nullptr) == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
#endif
    lua_pushnil(L);
    lua_pushstring(L, "touch failed");
    return 2;
}

int luaopen_lfs(lua_State* L) {
    static const luaL_Reg kFuncs[] = {
        {"attributes", l_lfs_attributes},
        {"symlinkattributes", l_lfs_attributes},
        {"dir", l_lfs_dir},
        {"mkdir", l_lfs_mkdir},
        {"rmdir", l_lfs_rmdir},
        {"currentdir", l_lfs_currentdir},
        {"chdir", l_lfs_chdir},
        {"touch", l_lfs_touch},
        {nullptr, nullptr},
    };
    luaL_newlib(L, kFuncs);
    return 1;
}

// ---------------------------------------------------------------- Sandboxed io library

struct LuaSandboxFile {
    std::FILE* fp = nullptr;
    bool closed = false;
    bool writable = false;
    std::size_t current_size = 0;
    std::uintmax_t quota_remaining = 0;

    void close_handle() {
        if (fp) {
            std::fclose(fp);
            fp = nullptr;
        }
        closed = true;
    }
};

LuaSandboxFile* check_sandbox_file(lua_State* L, int idx = 1) {
    auto* f = static_cast<LuaSandboxFile*>(luaL_checkudata(L, idx, kSandboxFileMeta));
    if (f->closed || !f->fp) luaL_error(L, "attempt to use a closed file");
    return f;
}

bool valid_lua_file_mode(const char* mode, bool& is_write) {
    if (!mode || !*mode) return false;
    char base = mode[0];
    if (base != 'r' && base != 'w' && base != 'a') return false;
    const char* p = mode + 1;
    bool plus = false;
    if (*p == '+') {
        plus = true;
        ++p;
    }
    if (*p == 'b') {
        ++p;
        if (!plus && *p == '+') {
            plus = true;
            ++p;
        }
    }
    if (*p != '\0') return false;
    is_write = (base == 'w' || base == 'a' || plus);
    return true;
}

int l_file_close(lua_State* L) {
    auto* f = static_cast<LuaSandboxFile*>(luaL_checkudata(L, 1, kSandboxFileMeta));
    if (f->closed || !f->fp) {
        lua_pushnil(L);
        lua_pushstring(L, "attempt to use a closed file");
        return 2;
    }
    int rc = std::fclose(f->fp);
    f->fp = nullptr;
    f->closed = true;
    if (rc == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, std::strerror(errno));
    return 2;
}

int l_file_gc(lua_State* L) {
    auto* f = static_cast<LuaSandboxFile*>(lua_touserdata(L, 1));
    if (f) f->close_handle();
    return 0;
}

int l_file_tostring(lua_State* L) {
    auto* f = static_cast<LuaSandboxFile*>(luaL_checkudata(L, 1, kSandboxFileMeta));
    if (f->closed || !f->fp) lua_pushliteral(L, "file (closed)");
    else lua_pushfstring(L, "file (%p)", f->fp);
    return 1;
}

int l_file_flush(lua_State* L) {
    auto* f = check_sandbox_file(L);
    if (std::fflush(f->fp) == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, std::strerror(errno));
    return 2;
}

bool read_line(lua_State* L, std::FILE* fp, bool keep_eol) {
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    int ch = 0;
    while ((ch = std::getc(fp)) != EOF && ch != '\n') {
        luaL_addchar(&b, static_cast<char>(ch));
    }
    if (ch == '\n') {
        if (keep_eol) luaL_addchar(&b, '\n');
        luaL_pushresult(&b);
        return true;
    }
    luaL_pushresult(&b);
    if (lua_rawlen(L, -1) > 0) return true;
    lua_pop(L, 1);
    return false;
}

void read_all(lua_State* L, std::FILE* fp) {
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    char buf[4096];
    for (;;) {
        std::size_t n = std::fread(buf, 1, sizeof(buf), fp);
        if (n > 0) luaL_addlstring(&b, buf, n);
        if (n < sizeof(buf)) break;
    }
    luaL_pushresult(&b);
}

bool read_bytes(lua_State* L, std::FILE* fp, std::size_t count) {
    if (count == 0) {
        int ch = std::getc(fp);
        if (ch == EOF) return false;
        std::ungetc(ch, fp);
        lua_pushliteral(L, "");
        return true;
    }
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    char buf[4096];
    std::size_t rem = count;
    while (rem > 0) {
        std::size_t want = std::min(rem, sizeof(buf));
        std::size_t n = std::fread(buf, 1, want, fp);
        if (n > 0) {
            luaL_addlstring(&b, buf, n);
            rem -= n;
        }
        if (n < want) break;
    }
    luaL_pushresult(&b);
    if (lua_rawlen(L, -1) > 0) return true;
    lua_pop(L, 1);
    return false;
}

bool read_number(lua_State* L, std::FILE* fp) {
    std::string tok;
    int ch = 0;
    while ((ch = std::getc(fp)) != EOF && std::isspace(static_cast<unsigned char>(ch))) {
    }
    if (ch == EOF) return false;
    while (ch != EOF && !std::isspace(static_cast<unsigned char>(ch)) && tok.size() < 200) {
        tok.push_back(static_cast<char>(ch));
        ch = std::getc(fp);
    }
    if (ch != EOF) std::ungetc(ch, fp);
    char* end = nullptr;
    double val = std::strtod(tok.c_str(), &end);
    if (end == tok.c_str() || *end != '\0') return false;
    lua_pushnumber(L, val);
    return true;
}

int read_sandbox_args(lua_State* L, std::FILE* fp, int first_arg) {
    int nargs = lua_gettop(L) - first_arg + 1;
    std::clearerr(fp);
    if (nargs <= 0) {
        if (read_line(L, fp, false)) return 1;
        lua_pushnil(L);
        return 1;
    }
    luaL_checkstack(L, nargs + LUA_MINSTACK, "too many arguments");
    int nres = 0;
    bool ok = true;
    for (int i = first_arg; i <= first_arg + nargs - 1 && ok; ++i) {
        if (lua_type(L, i) == LUA_TNUMBER) {
            lua_Integer cnt = luaL_checkinteger(L, i);
            ok = (cnt >= 0) && read_bytes(L, fp, static_cast<std::size_t>(cnt));
        } else {
            const char* fmt = luaL_checkstring(L, i);
            if (*fmt == '*') ++fmt;
            switch (*fmt) {
            case 'n': ok = read_number(L, fp); break;
            case 'l': ok = read_line(L, fp, false); break;
            case 'L': ok = read_line(L, fp, true); break;
            case 'a':
                read_all(L, fp);
                ok = true;
                break;
            default: return luaL_argerror(L, i, "invalid format");
            }
        }
        if (ok) ++nres;
    }
    if (std::ferror(fp)) {
        lua_pop(L, nres);
        lua_pushnil(L);
        lua_pushstring(L, std::strerror(errno));
        return 2;
    }
    if (!ok) {
        lua_pushnil(L);
        ++nres;
    }
    return nres;
}

int l_file_read(lua_State* L) {
    auto* f = check_sandbox_file(L);
    return read_sandbox_args(L, f->fp, 2);
}

int l_file_write(lua_State* L) {
    auto* f = check_sandbox_file(L);
    if (!f->writable) {
        lua_pushnil(L);
        lua_pushstring(L, "file is not open for writing");
        return 2;
    }
    CompatHolder* h = get_compat(L);
    if (h && h->env.write_blocked && h->env.write_blocked()) {
        lua_pushnil(L);
        lua_pushstring(L, "write blocked while dry-running or stopping");
        return 2;
    }
    int nargs = lua_gettop(L);
    for (int i = 2; i <= nargs; ++i) {
        std::size_t len = 0;
        const char* s = luaL_checklstring(L, i, &len);
        if (len > 0) {
            if (f->current_size + len > kSandboxMaxFileBytes || len > f->quota_remaining) {
                lua_pushnil(L);
                lua_pushstring(L, "Hailstone config quota exceeded");
                return 2;
            }
            if (std::fwrite(s, 1, len, f->fp) != len) {
                lua_pushnil(L);
                lua_pushstring(L, std::strerror(errno));
                return 2;
            }
            f->current_size += len;
            f->quota_remaining -= len;
        }
    }
    lua_pushvalue(L, 1);
    return 1;
}

int l_file_seek(lua_State* L) {
    static constexpr const char* kWhenceNames[] = {"set", "cur", "end", nullptr};
    static constexpr int kWhenceModes[] = {SEEK_SET, SEEK_CUR, SEEK_END};
    auto* f = check_sandbox_file(L);
    int op = luaL_checkoption(L, 2, "cur", kWhenceNames);
    lua_Integer offset = luaL_optinteger(L, 3, 0);
    if (std::fseek(f->fp, static_cast<long>(offset), kWhenceModes[op]) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, std::strerror(errno));
        return 2;
    }
    long pos = std::ftell(f->fp);
    if (pos < 0) {
        lua_pushnil(L);
        lua_pushstring(L, std::strerror(errno));
        return 2;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(pos));
    return 1;
}

int l_file_setvbuf(lua_State* L) {
    static constexpr const char* kModes[] = {"no", "full", "line", nullptr};
    static constexpr int kModeVals[] = {_IONBF, _IOFBF, _IOLBF};
    auto* f = check_sandbox_file(L);
    int mode = luaL_checkoption(L, 2, nullptr, kModes);
    std::size_t size = static_cast<std::size_t>(luaL_optinteger(L, 3, LUAL_BUFFERSIZE));
    int rc = std::setvbuf(f->fp, nullptr, kModeVals[mode], size);
    if (rc == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, std::strerror(errno));
    return 2;
}

int l_file_lines_iter(lua_State* L) {
    auto* f = static_cast<LuaSandboxFile*>(luaL_checkudata(L, lua_upvalueindex(1), kSandboxFileMeta));
    bool to_close = lua_toboolean(L, lua_upvalueindex(2)) != 0;
    int n_fmt = static_cast<int>(lua_tointeger(L, lua_upvalueindex(3)));
    if (f->closed || !f->fp) return 0;

    int base = lua_gettop(L);
    for (int i = 1; i <= n_fmt; ++i) {
        lua_pushvalue(L, lua_upvalueindex(3 + i));
    }
    int nres = read_sandbox_args(L, f->fp, base + 1);
    if (nres >= 1 && lua_isnil(L, base + 1)) {
        if (to_close) f->close_handle();
        return 0;
    }
    return nres;
}

void push_lines_closure(lua_State* L, bool to_close, int first_fmt) {
    int n_fmt = lua_gettop(L) - first_fmt + 1;
    if (n_fmt < 0) n_fmt = 0;
    lua_pushboolean(L, to_close ? 1 : 0);
    lua_pushinteger(L, n_fmt);
    for (int i = 0; i < n_fmt; ++i) {
        lua_pushvalue(L, first_fmt + i);
    }
    lua_pushcclosure(L, l_file_lines_iter, 3 + n_fmt);
}

int l_file_lines(lua_State* L) {
    check_sandbox_file(L);
    lua_pushvalue(L, 1);
    push_lines_closure(L, false, 2);
    return 1;
}

int open_sandbox_file(lua_State* L, const char* raw_path, const char* mode) {
    bool is_write = false;
    if (!valid_lua_file_mode(mode, is_write)) {
        return luaL_error(L, "invalid file mode '%s'", mode);
    }
    std::string why;
    auto resolved = resolve_sandbox_path(L, raw_path, is_write ? PathAccess::Write : PathAccess::Read, false, why);
    if (!resolved) {
        lua_pushnil(L);
        lua_pushstring(L, why.c_str());
        return 2;
    }

    std::error_code ec;
    if (fs::is_directory(*resolved, ec)) {
        lua_pushnil(L);
        lua_pushstring(L, "Is a directory");
        return 2;
    }

    std::size_t current_size = 0;
    std::uintmax_t quota_remaining = 0;
    if (is_write) {
        CompatHolder* h = get_compat(L);
        if (!h || h->env.config_dir.empty()) {
            lua_pushnil(L);
            lua_pushstring(L, "config directory unavailable");
            return 2;
        }
        fs::create_directories(h->env.config_dir, ec);
        const bool exists = fs::exists(*resolved, ec);
        const SandboxDirUsage used = sandbox_dir_usage(h->env.config_dir, *resolved);
        if (!exists && used.files + 1 > kSandboxMaxFiles) {
            lua_pushnil(L);
            lua_pushstring(L, "Hailstone config file count limit reached");
            return 2;
        }
        if (mode[0] != 'w' && exists) {
            auto fsz = fs::file_size(*resolved, ec);
            if (!ec) current_size = static_cast<std::size_t>(std::min<std::uintmax_t>(fsz, kSandboxMaxFileBytes));
        }
        if (used.bytes + current_size > kSandboxMaxBytes) {
            lua_pushnil(L);
            lua_pushstring(L, "Hailstone config size quota exceeded");
            return 2;
        }
        quota_remaining = kSandboxMaxBytes - (used.bytes + current_size);
    }

    std::FILE* fp = open_stdio_file(*resolved, mode);
    if (!fp) {
        lua_pushnil(L);
        lua_pushstring(L, std::strerror(errno));
        return 2;
    }

    auto* f = static_cast<LuaSandboxFile*>(lua_newuserdatauv(L, sizeof(LuaSandboxFile), 0));
    new (f) LuaSandboxFile{};
    f->fp = fp;
    f->writable = is_write;
    f->current_size = current_size;
    f->quota_remaining = quota_remaining;
    luaL_setmetatable(L, kSandboxFileMeta);
    return 1;
}

int l_io_open(lua_State* L) {
    const char* raw_path = luaL_checkstring(L, 1);
    const char* mode = luaL_optstring(L, 2, "r");
    return open_sandbox_file(L, raw_path, mode);
}

int l_io_close(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    return l_file_close(L);
}

int l_io_type(lua_State* L) {
    luaL_checkany(L, 1);
    auto* f = static_cast<LuaSandboxFile*>(luaL_testudata(L, 1, kSandboxFileMeta));
    if (!f) lua_pushnil(L);
    else if (f->closed || !f->fp) lua_pushliteral(L, "closed file");
    else lua_pushliteral(L, "file");
    return 1;
}

int l_io_lines(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        return luaL_error(L, "io.lines requires a filename in Hailstone sandbox");
    }
    const char* raw_path = luaL_checkstring(L, 1);
    int nres = open_sandbox_file(L, raw_path, "r");
    if (lua_isnil(L, -nres)) {
        return luaL_error(L, "%s", lua_tostring(L, -nres + 1));
    }
    int file_idx = lua_gettop(L);
    int n_fmt = file_idx - 2; // index 1 is raw_path, 2..file_idx-1 are optional read formats
    if (n_fmt < 0) n_fmt = 0;
    luaL_checkstack(L, 4 + n_fmt, "too many arguments");
    lua_pushvalue(L, file_idx);
    lua_pushboolean(L, 1);
    lua_pushinteger(L, n_fmt);
    for (int i = 2; i < file_idx; ++i) {
        lua_pushvalue(L, i);
    }
    lua_pushcclosure(L, l_file_lines_iter, 3 + n_fmt);
    return 1;
}

int l_io_flush(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}

int l_io_write(lua_State* L) {
    CompatHolder* h = get_compat(L);
    int nargs = lua_gettop(L);
    std::string out;
    for (int i = 1; i <= nargs; ++i) {
        std::size_t len = 0;
        const char* s = luaL_checklstring(L, i, &len);
        out.append(s, len);
    }
    if (h && h->env.say && !out.empty()) h->env.say(out);
    lua_pushboolean(L, 1);
    return 1;
}

int l_io_disabled(lua_State* L) {
    lua_pushnil(L);
    lua_pushstring(L, "operation disabled in Hailstone sandbox");
    return 2;
}

int luaopen_io_sandbox(lua_State* L) {
    if (luaL_newmetatable(L, kSandboxFileMeta)) {
        static const luaL_Reg kFileMethods[] = {
            {"close", l_file_close},
            {"flush", l_file_flush},
            {"lines", l_file_lines},
            {"read", l_file_read},
            {"seek", l_file_seek},
            {"setvbuf", l_file_setvbuf},
            {"write", l_file_write},
            {nullptr, nullptr},
        };
        luaL_newlib(L, kFileMethods);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_file_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, l_file_gc);
        lua_setfield(L, -2, "__close");
        lua_pushcfunction(L, l_file_tostring);
        lua_setfield(L, -2, "__tostring");
        lua_pushliteral(L, "FILE*");
        lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);

    static const luaL_Reg kIoFuncs[] = {
        {"open", l_io_open},
        {"close", l_io_close},
        {"type", l_io_type},
        {"lines", l_io_lines},
        {"flush", l_io_flush},
        {"write", l_io_write},
        {"read", l_io_disabled},
        {"input", l_io_disabled},
        {"output", l_io_disabled},
        {nullptr, nullptr},
    };
    luaL_newlib(L, kIoFuncs);
    return 1;
}

void register_safe_package_and_debug(lua_State* L) {
    // Minimal safe package table: exposes package.config and package.loaded (aliased to hailstone.loaded)
    // without native C library loading (package.loadlib / cpath).
    lua_newtable(L);
#if defined(_WIN32)
    lua_pushliteral(L, "\\\n;\n?\n!\n-\n");
#else
    lua_pushliteral(L, "/\n;\n?\n!\n-\n");
#endif
    lua_setfield(L, -2, "config");
    lua_pushliteral(L, "");
    lua_setfield(L, -2, "path");
    lua_pushliteral(L, "");
    lua_setfield(L, -2, "cpath");
    lua_getfield(L, LUA_REGISTRYINDEX, "hailstone.loaded");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    lua_setfield(L, -2, "loaded");
    lua_newtable(L);
    lua_setfield(L, -2, "preload");
    lua_newtable(L);
    lua_setfield(L, -2, "searchers");
    lua_setglobal(L, "package");

    // Minimal safe debug subset: only debug.traceback and debug.getinfo; no debug.getregistry,
    // debug.setmetatable, debug.sethook, or upvalue/local mutators.
    lua_pushcfunction(L, luaopen_debug);
    lua_pushliteral(L, LUA_DBLIBNAME);
    lua_call(L, 1, 1);
    lua_newtable(L);
    for (const char* keep : {"traceback", "getinfo"}) {
        lua_getfield(L, -2, keep);
        lua_setfield(L, -2, keep);
    }
    lua_setglobal(L, LUA_DBLIBNAME);
    lua_pop(L, 1);
}

// ---------------------------------------------------------------- actors (IPC bridged)

std::string read_str_field(lua_State* L, int tbl_idx, const char* key) {
    lua_getfield(L, tbl_idx, key);
    std::string out;
    if (lua_isstring(L, -1)) out = lua_tostring(L, -1);
    lua_pop(L, 1);
    return out;
}

int send_actor_impl(lua_State* L, int addr_idx, int payload_idx) {
    CompatHolder* h = get_compat(L);
    ipc::ActorPacket pkt;
    if (h) {
        if (h->env.character_name) pkt.sender_char = h->env.character_name();
        if (h->env.server_name) pkt.sender_server = h->env.server_name();
        pkt.script = h->env.script_name;
    }
    if (lua_isstring(L, addr_idx)) {
        pkt.mailbox = lua_tostring(L, addr_idx);
    } else if (lua_istable(L, addr_idx)) {
        pkt.mailbox = read_str_field(L, addr_idx, "mailbox");
        std::string s = read_str_field(L, addr_idx, "script");
        if (!s.empty()) pkt.script = s;
        pkt.target_server = read_str_field(L, addr_idx, "server");
        pkt.target_char = read_str_field(L, addr_idx, "character");
    }
    pkt.payload_json = lua_value_to_json(L, payload_idx, 0);

    bool sent = false;
    if (h && h->env.ipc_bus && h->env.ipc_bus->connected()) {
        sent = h->env.ipc_bus->send_actor(pkt);
    }
    // Always loop back locally when disconnected or targeting self/broadcast so single-client & multi-script work.
    if (h && h->env.loopback_push && (!sent || pkt.target_char.empty() || pkt.target_char == pkt.sender_char)) {
        h->env.loopback_push(pkt);
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_actors_send(lua_State* L) {
    if (lua_gettop(L) == 1) {
        lua_newtable(L);
        lua_insert(L, 1);
    }
    return send_actor_impl(L, 1, 2);
}

int l_dropbox_send(lua_State* L) {
    if (lua_gettop(L) == 2) {
        // dropbox:send(payload)
        lua_getfield(L, 1, "_mailbox");
        lua_newtable(L);
        lua_insert(L, -2);
        lua_setfield(L, -2, "mailbox");
        return send_actor_impl(L, lua_gettop(L), 2);
    }
    return send_actor_impl(L, 2, 3);
}

int l_actors_register(lua_State* L) {
    std::string mailbox;
    int fn_idx = 0;
    if (lua_isstring(L, 1)) {
        mailbox = lua_tostring(L, 1);
        if (lua_isfunction(L, 2)) fn_idx = 2;
    } else if (lua_isfunction(L, 1)) {
        fn_idx = 1;
    }
    if (fn_idx > 0) {
        lua_getfield(L, LUA_REGISTRYINDEX, kActorHandlersKey);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, LUA_REGISTRYINDEX, kActorHandlersKey);
        }
        lua_newtable(L);
        lua_pushlstring(L, mailbox.data(), mailbox.size());
        lua_setfield(L, -2, "mailbox");
        lua_pushvalue(L, fn_idx);
        lua_setfield(L, -2, "cb");
        lua_rawseti(L, -2, static_cast<lua_Integer>(lua_rawlen(L, -2) + 1));
        lua_pop(L, 1);
    }

    lua_newtable(L);
    lua_pushlstring(L, mailbox.data(), mailbox.size());
    lua_setfield(L, -2, "_mailbox");
    lua_pushcfunction(L, l_dropbox_send);
    lua_setfield(L, -2, "send");
    return 1;
}

int luaopen_actors(lua_State* L) {
    static const luaL_Reg kFuncs[] = {
        {"register", l_actors_register},
        {"send", l_actors_send},
        {nullptr, nullptr},
    };
    luaL_newlib(L, kFuncs);
    return 1;
}

// ---------------------------------------------------------------- Lua compat shims (PackageMan, ICONS, Set, ImPlot, ImAnim, Zep)

constexpr const char* kLuaCompatScript = R"lua(
local preloaded = ...

-- MacroQuest commonly seeds with a fractional elapsed-time value; Lua 5.4
-- requires an integer seed.
do
    local randomseed = math.randomseed
    math.randomseed = function(seed) return randomseed(math.floor(tonumber(seed) or 0)) end
end

-- mq.PackageMan
preloaded['mq.PackageMan'] = {
    Require = function(pkg, mod_name)
        local target = mod_name or pkg
        local ok, m = pcall(require, target)
        if ok then return m end
        if pkg == 'luafilesystem' then return require('lfs') end
        error('PackageMan.Require failed for ' .. tostring(target) .. ': ' .. tostring(m))
    end,
    Install = function() return true end,
}

-- mq.set / mq.Set
local Set = {}
Set.__index = Set
function Set.new(list)
    local s = { _items = {} }
    if type(list) == 'table' then
        for _, v in ipairs(list) do s._items[v] = true end
    end
    return setmetatable(s, Set)
end
function Set:add(v) self._items[v] = true end
function Set:remove(v) self._items[v] = nil end
function Set:contains(v) return self._items[v] == true end
function Set:clear() self._items = {} end
function Set:toList()
    local out = {}
    for k in pairs(self._items) do out[#out + 1] = k end
    return out
end
function Set:size()
    local n = 0
    for _ in pairs(self._items) do n = n + 1 end
    return n
end
preloaded['mq.set'] = Set
preloaded['mq.Set'] = Set

-- mq.ICONS (FontAwesome / Material Design UTF-8 fallback table)
local known_icons = {
    FA_PLAY = '>', FA_PAUSE = '||', FA_STOP = '[]', FA_REFRESH = '@', FA_SYNC = '@',
    FA_COG = '*', FA_COGS = '*', FA_CHECK = '+', FA_TIMES = 'x', FA_WARNING = '!',
    FA_INFO_CIRCLE = 'i', FA_BUG = '#', FA_SHIELD = '#', FA_HEART = '+', FA_MEDKIT = '+',
    FA_CROSSHAIRS = '+', FA_BOLT = '*', FA_USER = '@', FA_USERS = '@', FA_LIST = '=',
    FA_FILE = 'F', FA_FOLDER = 'D', FA_SAVE = 'S', FA_TRASH = 'X', FA_EDIT = 'E',
    FA_EYE = 'O', FA_SEARCH = '?', FA_ARROW_UP = '^', FA_ARROW_DOWN = 'v',
    MD_PLAY_ARROW = '>', MD_PAUSE = '||', MD_STOP = '[]', MD_SETTINGS = '*',
    MD_WARNING = '!', MD_INFO = 'i', MD_CHECK = '+', MD_CLOSE = 'x',
    MD_BUG_REPORT = '#', MD_REFRESH = '@', MD_DELETE = 'X', MD_SAVE = 'S',
    MD_HELP = '?', MD_CONTENT_COPY = 'C', MD_ARROW_DROP_DOWN = 'v',
}
local icons = setmetatable(known_icons, {
    __index = function(_, k)
        if type(k) == 'string' then return '*' end
        return ''
    end,
})
preloaded['mq.ICONS'] = icons

-- ImGui's global string hash helper used by RG Mercs for stable widget IDs.
-- FNV-1a is deterministic and sufficient for the Lua-side ID namespace.
_G.ImHashStr = _G.ImHashStr or function(value)
    local hash = 2166136261
    for i = 1, #tostring(value) do
        hash = bit32.bxor(hash, string.byte(tostring(value), i))
        hash = (hash * 16777619) % 4294967296
    end
    return hash
end
_G.IM_COL32 = _G.IM_COL32 or function(r, g, b, a)
    return bit32.bor(
        bit32.band(math.floor(a or 255), 0xff) * 0x1000000,
        bit32.band(math.floor(b or 0), 0xff) * 0x10000,
        bit32.band(math.floor(g or 0), 0xff) * 0x100,
        bit32.band(math.floor(r or 0), 0xff))
end

-- ImPlot lightweight safe shim (RG Mercs performance monitor)
_G.ImAxis = _G.ImAxis or { X1 = 0, X2 = 1, X3 = 2, Y1 = 3, Y2 = 4, Y3 = 5 }
_G.ImPlotLineFlags = _G.ImPlotLineFlags or { None = 0, Segments = 1024, Loop = 2048, SkipNaN = 4096, NoClip = 8192, Shaded = 16384 }
_G.ImPlotFlags = _G.ImPlotFlags or { None = 0 }
local ImPlot = {
    BeginPlot = function() return false end,
    EndPlot = function() end,
    SetupAxes = function() end,
    SetupAxis = function() end,
    SetupAxisLimits = function() end,
    PlotLine = function() end,
    PlotShaded = function() end,
    PlotBars = function() end,
    PushStyleColor = function() end,
    PopStyleColor = function() end,
}
preloaded['ImPlot'] = ImPlot

-- ImAnim lightweight deterministic tween shim (RG Mercs splash loader)
_G.IamEaseType = _G.IamEaseType or { Linear = 0, InQuad = 1, OutQuad = 2, InOutQuad = 3, InQuart = 7, OutExpo = 14, OutBack = 20, OutBounce = 26 }
_G.IamWaveType = _G.IamWaveType or { Sine = 0, Sawtooth = 1, Square = 2, Triangle = 3 }
_G.IamPolicy = _G.IamPolicy or { Crossfade = 0, Cut = 1 }
local ImAnim = {
    EasePreset = function(t) return t or 0 end,
    OscillateVec2 = function() return ImVec2(0, 0) end,
    TweenFloat = function(_, _, target) return target or 0 end,
    TweenVec2 = function(_, _, target) return target or ImVec2(0, 0) end,
    TweenColor = function(_, _, target) return target end,
    Oscillate = function(_, min_value, max_value) return (min_value + max_value) * 0.5 end,
}
preloaded['ImAnim'] = ImAnim

-- Zep lightweight console/editor shim (RG Mercs debug window)
local function new_zep_widget()
    return {
        _lines = {},
        _text = '',
        _flags = 0,
        AppendText = function(self, s)
            if s then self._lines[#self._lines + 1] = tostring(s) end
        end,
        Clear = function(self) self._lines = {} end,
        PushStyleColor = function() end,
        PopStyleColor = function() end,
        SetText = function(self, s) self._text = tostring(s or '') end,
        GetText = function(self) return self._text end,
        HasFlag = function(self, f) return (self._flags & (f or 0)) ~= 0 end,
        SetFlags = function(self, f) self._flags = self._flags | (f or 0) end,
        ClearFlags = function(self, f) self._flags = self._flags & ~(f or 0) end,
        GetBuffer = function(self) return self end,
        CreateBuffer = function(self) return new_zep_widget() end,
        Render = function(self)
            if ImGui and ImGui.TextUnformatted and #self._lines > 0 then
                local start = math.max(1, #self._lines - 20)
                for i = start, #self._lines do
                    ImGui.TextUnformatted(self._lines[i])
                end
            end
        end,
    }
end
local Zep = {
    ConsoleCol = { Text = 0, Background = 1 },
    BufferFlags = { Dirty = 1, ReadOnly = 2 },
    Console = { new = function() return new_zep_widget() end },
    Editor = { new = function() return new_zep_widget() end },
}
preloaded['Zep'] = Zep
)lua";

} // namespace

std::string lua_value_to_json(lua_State* L, int idx, int depth) {
    std::unordered_set<const void*> seen;
    return to_json_impl(L, idx, depth, seen);
}

bool push_json_to_lua(lua_State* L, std::string_view json) {
    JsonParser p{json, 0};
    int top = lua_gettop(L);
    if (!p.parse_value(L, 0)) {
        lua_settop(L, top);
        return false;
    }
    return true;
}

void register_compat_modules(lua_State* L, const CompatEnv& env) {
    auto* h = static_cast<CompatHolder*>(lua_newuserdatauv(L, sizeof(CompatHolder), 0));
    new (h) CompatHolder{env};
    lua_newtable(L);
    lua_pushcfunction(L, compat_gc);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, kCompatEnvKey);

    lua_getfield(L, LUA_REGISTRYINDEX, kPreloadedKey);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, kPreloadedKey);
    }

    // Native C modules
    luaopen_bit32(L);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "bit32");
    lua_setfield(L, -2, "bit32");

    luaopen_io_sandbox(L);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "io");
    lua_setfield(L, -2, "io");

    luaopen_lsqlite3(L);
    lua_setfield(L, -2, "lsqlite3");

    luaopen_lfs(L);
    lua_setfield(L, -2, "lfs");

    luaopen_actors(L);
    lua_setfield(L, -2, "actors");

    register_safe_package_and_debug(L);

    // Lua compat shims
    if (luaL_loadbufferx(L, kLuaCompatScript, std::strlen(kLuaCompatScript), "=hailstone_compat", "t") == LUA_OK) {
        lua_pushvalue(L, -2); // pass preloaded table
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }

    lua_pop(L, 1); // pop preloaded table
}

void deliver_compat_actor(lua_State* L, const ipc::ActorPacket& pkt,
                          const std::function<void(const std::string& err)>& on_error) {
    lua_getfield(L, LUA_REGISTRYINDEX, kActorHandlersKey);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    CompatHolder* h = get_compat(L);
    std::string self_char = (h && h->env.character_name) ? h->env.character_name() : std::string();
    std::string self_srv = (h && h->env.server_name) ? h->env.server_name() : std::string();
    auto ieq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    };
    if (!pkt.target_char.empty() && !self_char.empty() && !ieq(pkt.target_char, self_char)) {
        lua_pop(L, 1);
        return;
    }
    if (!pkt.target_server.empty() && !self_srv.empty() && !ieq(pkt.target_server, self_srv)) {
        lua_pop(L, 1);
        return;
    }

    const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        lua_getfield(L, -1, "mailbox");
        const char* mb = lua_tostring(L, -1);
        std::string mb_str = mb ? mb : "";
        lua_pop(L, 1);

        if (!mb_str.empty() && !pkt.mailbox.empty() && !ieq(mb_str, pkt.mailbox)) {
            lua_pop(L, 1);
            continue;
        }

        lua_getfield(L, -1, "cb");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 2);
            continue;
        }

        // Build message argument: { content = ..., sender = { character, server, script, mailbox }, send = fn }
        lua_newtable(L);
        if (!push_json_to_lua(L, pkt.payload_json)) lua_pushnil(L);
        lua_setfield(L, -2, "content");

        lua_newtable(L);
        lua_pushlstring(L, pkt.sender_char.data(), pkt.sender_char.size());
        lua_setfield(L, -2, "character");
        lua_pushlstring(L, pkt.sender_server.data(), pkt.sender_server.size());
        lua_setfield(L, -2, "server");
        lua_pushlstring(L, pkt.script.data(), pkt.script.size());
        lua_setfield(L, -2, "script");
        lua_pushlstring(L, pkt.mailbox.data(), pkt.mailbox.size());
        lua_setfield(L, -2, "mailbox");
        lua_setfield(L, -2, "sender");

        lua_pushcfunction(L, l_actors_send);
        lua_setfield(L, -2, "send");

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "actor callback error";
            lua_pop(L, 1);
            if (on_error) on_error(err);
        }
        lua_pop(L, 1); // pop entry
    }
    lua_pop(L, 1); // pop handlers table
}

} // namespace hailstone::mq
