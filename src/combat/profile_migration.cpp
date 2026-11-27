#include "combat/profile_migration.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

#include "combat/class_profiles.h"
#include "combat/combat_profile.h"
#include "core/group_profile.h"
#include "core/ini_file.h"

namespace hailstone::combat {

namespace {

namespace fs = std::filesystem;

std::string trim_copy(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
    return std::string(text.substr(begin, end - begin));
}

std::string upper_copy(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return out;
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

std::string backup_stamp(std::uint64_t now_ms) {
    if (now_ms == 0) return "unstamped";
    return std::to_string(now_ms);
}

// Keeps a destination inside the data dir even for a hostile character name.
fs::path safe_destination(const fs::path& data_dir, std::string_view group_id, std::string_view character) {
    std::string file_name;
    file_name.reserve(character.size() + 12);
    for (char c : character) {
        const auto uc = static_cast<unsigned char>(c);
        file_name.push_back((std::isalnum(uc) != 0 || c == '_' || c == '-') ? c : '_');
    }
    if (file_name.empty()) file_name = "character";
    file_name += ".combat.ini";

    const fs::path candidate = data_dir / "profiles" / "migrated" / std::string(group_id) / file_name;
    if (is_path_within_root(data_dir, candidate)) return candidate;
    // Fall back to the flat folder rather than escape the data dir.
    return data_dir / "profiles" / "migrated" / file_name;
}

} // namespace

std::string class_short_from_legacy_role(std::string_view role) {
    const std::string upper = upper_copy(trim_copy(role));
    if (upper.empty()) return {};

    if (upper == "TANK" || upper == "WARRIOR" || upper == "PLATE") return "WAR";
    if (upper == "ROGUE" || upper == "DPS" || upper == "MELEE" || upper == "MELEEDPS" || upper == "MELEE DPS")
        return "ROG";
    if (upper == "HEALER" || upper == "PRIEST") return "CLR";
    if (upper == "MAGICIAN") return "MAG";
    if (upper == "NECROMANCER") return "NEC";
    if (upper == "ENCHANTER" || upper == "CC") return "ENC";

    if (upper.size() == 3 && find_class_profile(upper) != nullptr) return upper;
    return {};
}

std::string MigrationReport::summary() const {
    std::string line = "Migration: " + std::to_string(profiles_read) + " legacy profile(s), " +
                       std::to_string(slots_seen) + " slot(s): " + std::to_string(profiles_created) +
                       " to create, " + std::to_string(profiles_skipped) + " skipped, " +
                       std::to_string(unsupported_classes) + " unsupported class";
    if (!backup_dir.empty()) line += ", backups in " + backup_dir.string();
    return line;
}

std::string MigrationReport::to_text() const {
    std::string out;
    out += "# Hailstone profile migration report\n";
    out += "schema_version=" + std::to_string(kNativeProfileSchemaVersion) + "\n";
    out += "data_dir=" + data_dir.string() + "\n";
    out += "profiles_dir=" + profiles_dir.string() + "\n";
    out += "backup_dir=" + backup_dir.string() + "\n";
    out += "profiles_read=" + std::to_string(profiles_read) + "\n";
    out += "profiles_failed=" + std::to_string(profiles_failed) + "\n";
    out += "slots_seen=" + std::to_string(slots_seen) + "\n";
    out += "profiles_created=" + std::to_string(profiles_created) + "\n";
    out += "profiles_skipped=" + std::to_string(profiles_skipped) + "\n";
    out += "unsupported_classes=" + std::to_string(unsupported_classes) + "\n";
    out += "backups_written=" + std::to_string(backups_written) + "\n";
    out += "\n[actions]\n";
    for (const auto& action : actions) {
        out += std::string(migration_action_name(action.kind));
        out += "\tgroup=" + action.group_id;
        out += "\tslot=" + action.slot;
        out += "\tcharacter=" + action.character;
        out += "\tclass=" + action.class_short;
        out += "\tfrom=" + action.source.filename().string();
        out += "\tto=" + action.destination.string();
        if (!action.detail.empty()) out += "\tdetail=" + action.detail;
        out += "\n";
    }
    if (!warnings.empty()) {
        out += "\n[warnings]\n";
        for (const auto& warning : warnings) out += warning + "\n";
    }
    return out;
}

bool MigrationReport::write_text(const fs::path& path) const {
    std::error_code ec;
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    const std::string text = to_text();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return file.good();
}

int read_profile_schema_version(const fs::path& path) {
    IniFile ini;
    if (!ini.load(path)) return 0;
    const std::string text = trim_copy(ini.get_string("Migration", "schema_version", ""));
    if (text.empty()) return 0;
    try {
        return std::stoi(text);
    } catch (...) {
        return 0;
    }
}

MigrationReport plan_profile_migration(const MigrationOptions& options) {
    MigrationReport report;
    report.data_dir = options.data_dir;
    report.profiles_dir = options.profiles_dir;
    report.now_ms = options.now_ms;
    report.backup_dir = options.data_dir / "backups" / ("profile-migration-" + backup_stamp(options.now_ms));

    std::error_code ec;
    if (!fs::is_directory(options.profiles_dir, ec)) {
        report.warnings.push_back("No legacy profiles directory at " + options.profiles_dir.string());
        return report;
    }

    std::vector<fs::path> sources;
    for (fs::directory_iterator it(options.profiles_dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& path = it->path();
        if (!it->is_regular_file(ec)) continue;
        if (path.extension() != ".ini") continue;
        if (ends_with(path.filename().string(), ".combat.ini")) continue; // already native
        sources.push_back(path);
    }
    std::sort(sources.begin(), sources.end());

    for (const auto& source : sources) {
        core::GroupProfile group;
        std::string error;
        if (!core::load_group_profile(source, group, &error)) {
            ++report.profiles_failed;
            MigrationAction action;
            action.kind = MigrationActionKind::UnreadableProfile;
            action.source = source;
            action.detail = error;
            report.actions.push_back(std::move(action));
            report.warnings.push_back("Could not read " + source.filename().string() + ": " + error);
            continue;
        }
        ++report.profiles_read;

        const std::string group_id = group.id.empty() ? source.stem().string() : group.id;
        if (options.backup_legacy) {
            MigrationAction backup;
            backup.kind = MigrationActionKind::BackUpLegacy;
            backup.source = source;
            backup.destination = report.backup_dir / source.filename();
            backup.group_id = group_id;
            report.actions.push_back(std::move(backup));
            ++report.backups_written;
        }

        for (const auto& slot : group.slots) {
            if (!slot.enabled || slot.character.empty()) continue;
            ++report.slots_seen;

            std::string class_short = class_short_from_legacy_role(slot.class_role);
            if (class_short.empty()) class_short = class_short_from_legacy_role(slot.combat_profile);

            MigrationAction action;
            action.group_id = group_id;
            action.slot = slot.session_id.empty() ? std::to_string(slot.slot_number) : slot.session_id;
            action.character = slot.character;
            action.class_role = slot.class_role;
            action.class_short = class_short;
            action.source = source;

            if (class_short.empty()) {
                action.kind = MigrationActionKind::UnsupportedClass;
                action.detail = "no native profile for role \"" + slot.class_role + "\"";
                report.actions.push_back(std::move(action));
                ++report.unsupported_classes;
                report.warnings.push_back("Slot " + action.slot + " (" + slot.character +
                                          ") has no native class profile; keeping it on the legacy module");
                continue;
            }

            action.destination = safe_destination(options.data_dir, group_id, slot.character);
            const int existing = fs::exists(action.destination, ec) ? read_profile_schema_version(action.destination) : 0;
            if (existing > 0) {
                const bool outdated = existing < kNativeProfileSchemaVersion;
                if (!outdated || !options.upgrade_outdated) {
                    if (!options.overwrite_current) {
                        action.kind = MigrationActionKind::SkipExisting;
                        action.detail = "already migrated (schema v" + std::to_string(existing) + ")";
                        report.actions.push_back(std::move(action));
                        ++report.profiles_skipped;
                        continue;
                    }
                }
                action.kind = MigrationActionKind::CreateProfile;
                action.detail = "upgrade from schema v" + std::to_string(existing);
            } else {
                action.kind = MigrationActionKind::CreateProfile;
            }
            if (slot.script_module == "rgmercs" || slot.script_module == "RGMercs") {
                action.detail += action.detail.empty() ? "" : "; ";
                action.detail += "legacy script module replaced";
            }
            report.actions.push_back(std::move(action));
            ++report.profiles_created;
        }
    }

    return report;
}

MigrationResult apply_profile_migration(const MigrationReport& report) {
    MigrationResult result;
    std::error_code ec;

    for (const auto& action : report.actions) {
        switch (action.kind) {
        case MigrationActionKind::BackUpLegacy: {
            fs::create_directories(action.destination.parent_path(), ec);
            if (ec) {
                result.errors.push_back("Cannot create " + action.destination.parent_path().string() + ": " + ec.message());
                break;
            }
            ec.clear();
            fs::copy_file(action.source, action.destination, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                result.errors.push_back("Cannot back up " + action.source.filename().string() + ": " + ec.message());
            } else {
                ++result.backups_written;
            }
            break;
        }
        case MigrationActionKind::CreateProfile: {
            if (!is_path_within_root(report.data_dir, action.destination)) {
                result.errors.push_back("Refusing to write outside the data dir: " + action.destination.string());
                break;
            }
            CombatProfile profile = default_combat_profile_for_class(action.class_short);
            profile.profile_name = action.character + " (migrated)";
            profile.mode = "group";
            profile.profile_path = action.destination.string();
            if (!action.class_role.empty() && profile.role.empty()) profile.role = action.class_role;

            fs::create_directories(action.destination.parent_path(), ec);
            ec.clear();
            std::string error;
            if (!save_combat_profile(profile, action.destination, error)) {
                result.errors.push_back("Cannot write " + action.destination.string() + ": " + error);
                break;
            }

            IniFile ini;
            if (ini.load(action.destination)) {
                ini.set("Migration", "schema_version", std::to_string(kNativeProfileSchemaVersion));
                ini.set("Migration", "migrated_from", action.source.filename().string());
                ini.set("Migration", "migrated_at_ms", std::to_string(report.now_ms));
                ini.set("Migration", "legacy_class_role", action.class_role);
                ini.set("Migration", "legacy_slot", action.slot);
                if (!ini.save(action.destination)) {
                    result.errors.push_back("Wrote " + action.destination.string() + " but could not stamp schema version");
                    break;
                }
            }
            ++result.files_written;
            break;
        }
        case MigrationActionKind::SkipExisting:
        case MigrationActionKind::UnsupportedClass:
        case MigrationActionKind::UnreadableProfile:
            break;
        }
    }

    result.ok = result.errors.empty();
    return result;
}

MigrationResult rollback_profile_migration(const MigrationReport& report) {
    MigrationResult result;
    std::error_code ec;

    for (const auto& action : report.actions) {
        if (action.kind != MigrationActionKind::CreateProfile) continue;
        ec.clear();
        if (fs::remove(action.destination, ec) && !ec) {
            ++result.files_written;
        } else if (ec) {
            result.errors.push_back("Cannot remove " + action.destination.string() + ": " + ec.message());
        }
    }

    // The legacy profiles were never modified, so the backups are the only other
    // thing an apply created.
    for (const auto& action : report.actions) {
        if (action.kind != MigrationActionKind::BackUpLegacy) continue;
        ec.clear();
        if (fs::remove(action.destination, ec) && !ec) ++result.backups_written;
    }
    if (!report.backup_dir.empty()) {
        ec.clear();
        if (fs::is_empty(report.backup_dir, ec) && !ec) fs::remove(report.backup_dir, ec);
    }

    result.ok = result.errors.empty();
    return result;
}

} // namespace hailstone::combat
