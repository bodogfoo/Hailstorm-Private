#pragma once

// Moving a group off the legacy RGMercs Lua add-on without losing anything.
//
// The legacy side keeps its data in two places: a group profile INI with one
// [slotNN] section per box (class role, script module, spell loadout name) and,
// for the Lua module itself, a config bundle under the add-on's folder. This
// layer reads the group profiles read-only, writes versioned native combat
// profiles under Hailstone's own data dir, keeps a backup of every legacy file
// it touched, writes a report, and can undo the whole thing.
//
// Nothing under the game folder is read or written, and the legacy files are
// never modified: a migrated native profile sits beside them, so the old add-on
// keeps working until the player switches over.
//
// docs/native-combat.md "Migrating from RGMercs", docs/layout.md "profiles/".

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::combat {

// Bumped whenever the shape of a migrated profile changes. A profile written by
// an older version is re-migrated, not overwritten blindly.
inline constexpr int kNativeProfileSchemaVersion = 2;

enum class MigrationActionKind : std::uint8_t {
    CreateProfile = 0,
    SkipExisting,
    BackUpLegacy,
    UnsupportedClass,
    UnreadableProfile,
};

inline constexpr std::string_view migration_action_name(MigrationActionKind k) noexcept {
    switch (k) {
    case MigrationActionKind::CreateProfile: return "CreateProfile";
    case MigrationActionKind::SkipExisting: return "SkipExisting";
    case MigrationActionKind::BackUpLegacy: return "BackUpLegacy";
    case MigrationActionKind::UnsupportedClass: return "UnsupportedClass";
    case MigrationActionKind::UnreadableProfile: return "UnreadableProfile";
    }
    return "CreateProfile";
}

struct MigrationOptions {
    // Hailstone data dir. Every path this layer writes resolves inside it.
    std::filesystem::path data_dir;
    // Directory holding the legacy group profiles (*.ini). Read only.
    std::filesystem::path profiles_dir;
    // Replace a migrated profile whose schema_version is older than the current one.
    bool upgrade_outdated = true;
    // Refuse to touch a migrated profile that is already at the current version.
    bool overwrite_current = false;
    // Copy each legacy profile into the backup folder before writing anything.
    bool backup_legacy = true;
    // Stamped into the report and the backup folder name; 0 means "unstamped".
    std::uint64_t now_ms = 0;
};

struct MigrationAction {
    MigrationActionKind kind = MigrationActionKind::CreateProfile;
    std::filesystem::path source;
    std::filesystem::path destination;
    std::string group_id;
    std::string slot;
    std::string character;
    std::string class_role;
    std::string class_short;
    std::string detail;
};

struct MigrationReport {
    std::filesystem::path data_dir;
    std::filesystem::path profiles_dir;
    std::filesystem::path backup_dir;
    std::uint64_t now_ms = 0;
    int profiles_read = 0;
    int profiles_failed = 0;
    int slots_seen = 0;
    int profiles_created = 0;
    int profiles_skipped = 0;
    int unsupported_classes = 0;
    int backups_written = 0;
    std::vector<MigrationAction> actions;
    std::vector<std::string> warnings;

    bool ok() const { return profiles_failed == 0; }
    std::string summary() const;
    std::string to_text() const;
    bool write_text(const std::filesystem::path& path) const;
};

struct MigrationResult {
    bool ok = false;
    int files_written = 0;
    int backups_written = 0;
    std::vector<std::string> errors;
};

// Reads every legacy group profile and works out what would be written. No
// writes happen here, so the command can show the plan before applying it.
MigrationReport plan_profile_migration(const MigrationOptions& options);

// Writes the migrated profiles and the legacy backups the plan calls for.
MigrationResult apply_profile_migration(const MigrationReport& report);

// Deletes the profiles an apply created and puts the backups back. Legacy files
// are untouched because apply never modified them.
MigrationResult rollback_profile_migration(const MigrationReport& report);

// [Migration] schema_version of a native profile file, or 0 when it has none.
int read_profile_schema_version(const std::filesystem::path& path);

// Normalizes a legacy class role ("Tank", "war", "Healer", "Caster") to a
// Hailstone class short code, or an empty string when it maps to nothing.
std::string class_short_from_legacy_role(std::string_view role);

} // namespace hailstone::combat
