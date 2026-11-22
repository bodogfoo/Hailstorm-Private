#include "cortex/memory.h"

#include <filesystem>
#include <system_error>

#include <sqlite3.h>

namespace hailstone::cortex {

MemoryStore::~MemoryStore() {
    close();
}

bool MemoryStore::open(const std::string& db_path) {
    std::lock_guard<std::mutex> lock(mu_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }
    exec_sql("PRAGMA journal_mode=WAL;");
    exec_sql("PRAGMA synchronous=NORMAL;");

    const char* schema =
        "CREATE TABLE IF NOT EXISTS character_profiles ("
        "  server_name TEXT NOT NULL,"
        "  character_name TEXT NOT NULL,"
        "  class_short TEXT NOT NULL DEFAULT '',"
        "  role TEXT NOT NULL DEFAULT '',"
        "  personality TEXT NOT NULL DEFAULT '',"
        "  current_objective TEXT NOT NULL DEFAULT '',"
        "  PRIMARY KEY (server_name, character_name)"
        ");"
        "CREATE TABLE IF NOT EXISTS character_memories ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  server_name TEXT NOT NULL,"
        "  character_name TEXT NOT NULL,"
        "  kind TEXT NOT NULL,"
        "  content TEXT NOT NULL,"
        "  timestamp_ms INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_char_mem ON character_memories(server_name, character_name, id DESC);"
        "CREATE TABLE IF NOT EXISTS decision_history ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  server_name TEXT NOT NULL,"
        "  character_name TEXT NOT NULL,"
        "  action TEXT NOT NULL,"
        "  reason_code TEXT NOT NULL,"
        "  explanation TEXT NOT NULL,"
        "  timestamp_ms INTEGER NOT NULL,"
        "  latency_ms INTEGER NOT NULL,"
        "  valid INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_char_dec ON decision_history(server_name, character_name, id DESC);"
        "CREATE TABLE IF NOT EXISTS group_memories ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  server_name TEXT NOT NULL,"
        "  group_leader TEXT NOT NULL,"
        "  from_character TEXT NOT NULL,"
        "  content TEXT NOT NULL,"
        "  timestamp_ms INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_grp_mem ON group_memories(server_name, group_leader, id DESC);";

    return exec_sql(schema);
}

void MemoryStore::close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool MemoryStore::exec_sql(const char* sql) {
    if (!db_) return false;
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

bool MemoryStore::upsert_profile(const CharacterIdentity& id, const std::string& current_objective) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) return false;
    const char* sql =
        "INSERT INTO character_profiles(server_name, character_name, class_short, role, personality, current_objective) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6) "
        "ON CONFLICT(server_name, character_name) DO UPDATE SET "
        "class_short=excluded.class_short, role=excluded.role, "
        "personality=excluded.personality, current_objective=excluded.current_objective;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, id.server_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, id.character_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, id.class_short.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, id.role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, id.personality.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, current_objective.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool MemoryStore::load_profile(const std::string& server_name,
                               const std::string& character_name,
                               StoredProfile& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) return false;
    const char* sql =
        "SELECT class_short, role, personality, current_objective "
        "FROM character_profiles WHERE server_name=?1 AND character_name=?2;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, server_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, character_name.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out.server_name = server_name;
        out.character_name = character_name;
        out.class_short = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        out.role = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        out.personality = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        out.current_objective = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool MemoryStore::add_memory(const std::string& server_name,
                             const std::string& character_name,
                             const std::string& kind,
                             const std::string& content,
                             std::uint64_t timestamp_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) return false;
    const char* sql =
        "INSERT INTO character_memories(server_name, character_name, kind, content, timestamp_ms) "
        "VALUES(?1, ?2, ?3, ?4, ?5);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, server_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, character_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(timestamp_ms));
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    prune_memories(server_name, character_name);
    return true;
}

void MemoryStore::prune_memories(const std::string& server_name, const std::string& character_name) {
    const char* sql =
        "DELETE FROM character_memories WHERE server_name=?1 AND character_name=?2 "
        "AND id NOT IN ("
        "  SELECT id FROM character_memories WHERE server_name=?1 AND character_name=?2 "
        "  ORDER BY id DESC LIMIT ?3"
        ");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, server_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, character_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(max_memories_per_char_));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<MemoryEntry> MemoryStore::recent_memories(const std::string& server_name,
                                                      const std::string& character_name,
                                                      std::size_t limit) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<MemoryEntry> out;
    if (!db_) return out;
    const char* sql =
        "SELECT id, kind, content, timestamp_ms FROM character_memories "
        "WHERE server_name=?1 AND character_name=?2 ORDER BY id DESC LIMIT ?3;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(stmt, 1, server_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, character_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(limit));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MemoryEntry e;
        e.id = sqlite3_column_int64(stmt, 0);
        e.server_name = server_name;
        e.character_name = character_name;
        e.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        e.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        e.timestamp_ms = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 3));
        out.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::size_t MemoryStore::memory_count(const std::string& server_name,
                                      const std::string& character_name) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) return 0;
    const char* sql =
        "SELECT COUNT(*) FROM character_memories WHERE server_name=?1 AND character_name=?2;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, server_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, character_name.c_str(), -1, SQLITE_TRANSIENT);
    std::size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = static_cast<std::size_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return count;
}

bool MemoryStore::add_decision(const std::string& server_name,
                               const std::string& character_name,
                               const Decision& d) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) return false;
    const char* sql =
        "INSERT INTO decision_history(server_name, character_name, action, reason_code, explanation, timestamp_ms, latency_ms, valid) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, server_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, character_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, d.action_raw.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, d.reason_code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, d.explanation.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(d.timestamp_ms));
    sqlite3_bind_int(stmt, 7, static_cast<int>(d.latency_ms));
    sqlite3_bind_int(stmt, 8, d.valid ? 1 : 0);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::vector<Decision> MemoryStore::recent_decisions(const std::string& server_name,
                                                  const std::string& character_name,
                                                  std::size_t limit) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Decision> out;
    if (!db_) return out;
    const char* sql =
        "SELECT action, reason_code, explanation, timestamp_ms, latency_ms, valid "
        "FROM decision_history WHERE server_name=?1 AND character_name=?2 ORDER BY id DESC LIMIT ?3;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(stmt, 1, server_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, character_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(limit));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Decision d;
        d.action_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        d.intention = parse_intention(d.action_raw);
        d.reason_code = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        d.explanation = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        d.timestamp_ms = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 3));
        d.latency_ms = static_cast<std::uint32_t>(sqlite3_column_int(stmt, 4));
        d.valid = sqlite3_column_int(stmt, 5) != 0;
        out.push_back(std::move(d));
    }
    sqlite3_finalize(stmt);
    return out;
}

bool MemoryStore::add_group_memory(const std::string& server_name,
                                   const std::string& group_leader,
                                   const std::string& from_character,
                                   const std::string& content,
                                   std::uint64_t timestamp_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) return false;
    const char* sql =
        "INSERT INTO group_memories(server_name, group_leader, from_character, content, timestamp_ms) "
        "VALUES(?1, ?2, ?3, ?4, ?5);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, server_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, group_leader.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, from_character.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(timestamp_ms));
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::vector<MemoryEntry> MemoryStore::recent_group_memories(const std::string& server_name,
                                                            const std::string& group_leader,
                                                            std::size_t limit) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<MemoryEntry> out;
    if (!db_) return out;
    const char* sql =
        "SELECT id, from_character, content, timestamp_ms FROM group_memories "
        "WHERE server_name=?1 AND group_leader=?2 ORDER BY id DESC LIMIT ?3;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(stmt, 1, server_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, group_leader.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(limit));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MemoryEntry e;
        e.id = sqlite3_column_int64(stmt, 0);
        e.server_name = server_name;
        e.character_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        e.kind = "group";
        e.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        e.timestamp_ms = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 3));
        out.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::shared_ptr<MemoryStore> open_memory_store(const std::filesystem::path& db_file) {
    // The folders above the file are this writer's job: paths.h creates only the roots it knows.
    if (!db_file.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(db_file.parent_path(), ec);
        if (ec) return nullptr;
    }
    auto store = std::make_shared<MemoryStore>();
    // A path that is not a database (a folder, a file without write access) refuses here: the
    // caller falls back to running without persistence rather than failing to start.
    if (!store->open(db_file.string())) return nullptr;
    return store;
}

} // namespace hailstone::cortex
