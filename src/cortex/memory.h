#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cortex/types.h"

struct sqlite3;

namespace hailstone::cortex {

struct MemoryEntry {
    std::int64_t id = 0;
    std::string server_name;
    std::string character_name;
    std::string kind; // "observation", "outcome", "strategy_ok", "strategy_fail", "request", "discovery", "objective"
    std::string content;
    std::uint64_t timestamp_ms = 0;
};

struct StoredProfile {
    std::string server_name;
    std::string character_name;
    std::string class_short;
    std::string role;
    std::string personality;
    std::string current_objective;
};

class MemoryStore {
public:
    MemoryStore() = default;
    ~MemoryStore();

    MemoryStore(const MemoryStore&) = delete;
    MemoryStore& operator=(const MemoryStore&) = delete;

    bool open(const std::string& db_path);
    void close();
    bool is_open() const { return db_ != nullptr; }

    void set_retention_limit(std::size_t max_memories_per_char) {
        max_memories_per_char_ = max_memories_per_char;
    }

    bool upsert_profile(const CharacterIdentity& id, const std::string& current_objective);
    bool load_profile(const std::string& server_name,
                      const std::string& character_name,
                      StoredProfile& out);

    bool add_memory(const std::string& server_name,
                    const std::string& character_name,
                    const std::string& kind,
                    const std::string& content,
                    std::uint64_t timestamp_ms);

    std::vector<MemoryEntry> recent_memories(const std::string& server_name,
                                             const std::string& character_name,
                                             std::size_t limit = 8);

    std::size_t memory_count(const std::string& server_name,
                             const std::string& character_name);

    bool add_decision(const std::string& server_name,
                      const std::string& character_name,
                      const Decision& d);

    std::vector<Decision> recent_decisions(const std::string& server_name,
                                           const std::string& character_name,
                                           std::size_t limit = 8);

    bool add_group_memory(const std::string& server_name,
                          const std::string& group_leader,
                          const std::string& from_character,
                          const std::string& content,
                          std::uint64_t timestamp_ms);

    std::vector<MemoryEntry> recent_group_memories(const std::string& server_name,
                                                   const std::string& group_leader,
                                                   std::size_t limit = 6);

private:
    bool exec_sql(const char* sql);
    void prune_memories(const std::string& server_name, const std::string& character_name);

    mutable std::mutex mu_;
    sqlite3* db_ = nullptr;
    std::size_t max_memories_per_char_ = 200;
    std::size_t max_decisions_per_char_ = 100;
};

// Opens a store on `db_file`, creating the folders above it when they are missing (paths.h leaves
// folder creation to each writer). Returns nullptr when the database cannot be opened, so a caller
// can fall back to running without persistence instead of failing to start.
std::shared_ptr<MemoryStore> open_memory_store(const std::filesystem::path& db_file);

} // namespace hailstone::cortex
