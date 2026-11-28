#pragma once

// Persistent, versioned JSON database for per-character epic quest progress,
// global/group/character policies, audit history, backup/restore, and import/export.
// Never stores passwords, tokens, or secrets.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "epic/epic_types.h"

namespace hailstone::epic {

class EpicProgressStore {
public:
    void clear();

    bool load(const std::filesystem::path& path, std::string* error = nullptr);
    bool save(const std::filesystem::path& path, std::string* error = nullptr) const;

    bool import_json(std::string_view json_text, std::string* error = nullptr, bool merge = true);
    std::string export_json() const;

    bool backup_to(const std::filesystem::path& backup_path, std::string* error = nullptr) const;
    bool restore_from(const std::filesystem::path& backup_path, std::string* error = nullptr);

    CharacterEpicProfile* find_profile(const CharacterEpicKey& key);
    const CharacterEpicProfile* find_profile(const CharacterEpicKey& key) const;
    std::vector<CharacterEpicProfile*> find_profiles_for_character(std::string_view server,
                                                                   std::string_view character);
    void upsert_profile(CharacterEpicProfile profile);
    bool remove_profile(const CharacterEpicKey& key);

    const std::vector<CharacterEpicProfile>& profiles() const { return profiles_; }
    std::vector<CharacterEpicProfile>& mutable_profiles() { return profiles_; }

    const EpicLootPolicy& policy() const { return policy_; }
    EpicLootPolicy& mutable_policy() { return policy_; }

    // Record an audit entry and optionally rollback the most recent rollbackable manual change.
    static void append_audit(CharacterEpicProfile& profile, AuditEntry entry, std::size_t max_entries = 100);
    static bool rollback_last_manual_change(CharacterEpicProfile& profile, std::uint64_t now_ms, std::string* reason = nullptr);

    int schema_version() const { return schema_version_; }

private:
    int schema_version_ = kEpicSchemaVersion;
    EpicLootPolicy policy_;
    std::vector<CharacterEpicProfile> profiles_;
};

} // namespace hailstone::epic
