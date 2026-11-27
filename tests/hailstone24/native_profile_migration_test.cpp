#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "combat/class_profiles.h"
#include "combat/combat_profile.h"
#include "combat/profile_migration.h"
#include "core/group_profile.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::combat;
using hailstone::test::ScratchDir;
namespace fs = std::filesystem;

namespace {

void write_file_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_file_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_legacy_group(const fs::path& dir, const std::string& stem, const std::string& slots) {
    const std::string text =
        "[profile]\n"
        "name=" + stem + " test group\n"
        "description=fixture\n"
        "default_server=fixture\n"
        "default_world=Fixture\n"
        "auto_login=1\n"
        "auto_kick=1\n"
        "bg_tick_hz=25\n"
        "gpu_mode=cached\n"
        "\n" + slots;
    write_file_text(dir / (stem + ".ini"), text);
}

std::string slot_block(int number, const std::string& character, const std::string& role) {
    const std::string nn = number < 10 ? "0" + std::to_string(number) : std::to_string(number);
    return "[slot" + nn + "]\n" + "slot_number=" + std::to_string(number) + "\n" + "launch_order=" +
           std::to_string(number) + "\n" + "enabled=1\n" + "session_id=slot" + nn + "\n" + "character=" + character +
           "\n" + "account=fixture" + nn + "\n" + "server_id=fixture\n" + "world=Fixture\n" + "class_role=" + role +
           "\n" + "settings_path=servers/fixture/characters/Fixture/" + character + "/hailstone.ini\n" +
           "auto_login=1\n" + "headless=0\n" + "script_module=rgmercs\n" + "\n";
}

MigrationOptions options_for(const fs::path& root) {
    MigrationOptions options;
    options.data_dir = root / "data";
    options.profiles_dir = root / "data" / "profiles";
    options.now_ms = 1700000000000ULL;
    fs::create_directories(options.data_dir);
    fs::create_directories(options.profiles_dir);
    return options;
}

} // namespace

TEST_CASE("migration: legacy roles map onto the native classes") {
    CHECK(class_short_from_legacy_role("Tank") == "WAR");
    CHECK(class_short_from_legacy_role("tank") == "WAR");
    CHECK(class_short_from_legacy_role(" Healer ") == "CLR");
    CHECK(class_short_from_legacy_role("MeleeDPS") == "ROG");
    CHECK(class_short_from_legacy_role("Magician") == "MAG");
    CHECK(class_short_from_legacy_role("NEC") == "NEC");
    CHECK(class_short_from_legacy_role("cc") == "ENC");
    CHECK(class_short_from_legacy_role("Bard") == "");
    CHECK(class_short_from_legacy_role("Caster") == ""); // ambiguous, not guessed
    CHECK(class_short_from_legacy_role("") == "");
}

TEST_CASE("migration: a launched slot's role selects the native class table") {
    // Group1-4 identity: the client launched as --profile X --slot Y must end up
    // with the class table its slot names, so a box behaves as the profile said
    // even before the world has told the client the character's class.
    ScratchDir root("migrate_slot_identity");
    const fs::path profiles = root.path() / "profiles";
    fs::create_directories(profiles);
    write_legacy_group(profiles, "boxgroup",
                       slot_block(1, "Boxclr", "Healer") + slot_block(2, "Boxtank", "Tank"));

    core::GroupProfile profile;
    REQUIRE(core::load_group_profile(profiles / "boxgroup.ini", profile));

    const core::GroupSlot* healer = core::resolve_group_slot(profile, "slot01");
    REQUIRE(healer != nullptr);
    CHECK(healer->character == "Boxclr");
    const std::string healer_class = class_short_from_legacy_role(healer->class_role);
    CHECK(healer_class == "CLR");

    CombatProfile table;
    std::string err;
    REQUIRE(build_native_combat_profile(healer_class, table, err));
    CHECK(table.class_short == "CLR");
    CHECK_FALSE(table.rules.empty());
    CHECK_FALSE(table.loadout.empty());

    const core::GroupSlot* tank = core::resolve_group_slot(profile, "Boxtank");
    REQUIRE(tank != nullptr);
    CHECK(class_short_from_legacy_role(tank->class_role) == "WAR");
}

TEST_CASE("migration: a missing legacy folder is a warning, not a failure") {
    const hailstone::test::ScratchDir scratch("migrate_missing");
    MigrationOptions options;
    options.data_dir = scratch.path() / "data";
    options.profiles_dir = scratch.path() / "nothing_here";

    const MigrationReport report = plan_profile_migration(options);
    CHECK(report.profiles_read == 0);
    CHECK(report.profiles_created == 0);
    CHECK(report.ok());
    REQUIRE_FALSE(report.warnings.empty());
}

TEST_CASE("migration: plan reads every slot and flags the ones with no native class") {
    const hailstone::test::ScratchDir scratch("migrate_plan");
    MigrationOptions options = options_for(scratch.path());
    write_legacy_group(options.profiles_dir, "boxgroup",
                       slot_block(1, "Boxwar", "Tank") + slot_block(2, "Boxclr", "Healer") +
                           slot_block(3, "Boxbrd", "Bard"));

    const MigrationReport report = plan_profile_migration(options);
    CHECK(report.profiles_read == 1);
    CHECK(report.slots_seen == 3);
    CHECK(report.profiles_created == 2);
    CHECK(report.unsupported_classes == 1);
    CHECK(report.backups_written == 1);
    CHECK(report.ok());

    int creates = 0;
    int unsupported = 0;
    for (const auto& action : report.actions) {
        if (action.kind == MigrationActionKind::CreateProfile) {
            ++creates;
            CHECK(action.group_id == "boxgroup");
            CHECK(action.source.filename() == "boxgroup.ini");
            CHECK(action.destination.parent_path() ==
                  options.data_dir / "profiles" / "migrated" / "boxgroup");
        }
        if (action.kind == MigrationActionKind::UnsupportedClass) {
            ++unsupported;
            CHECK(action.character == "Boxbrd");
        }
    }
    CHECK(creates == 2);
    CHECK(unsupported == 1);
    CHECK_FALSE(report.warnings.empty());

    // Nothing is written by a plan.
    CHECK_FALSE(fs::exists(options.data_dir / "profiles" / "migrated"));
    CHECK_FALSE(fs::exists(report.backup_dir));
}

TEST_CASE("migration: apply writes versioned profiles, keeps a backup and can be undone") {
    const hailstone::test::ScratchDir scratch("migrate_apply");
    MigrationOptions options = options_for(scratch.path());
    write_legacy_group(options.profiles_dir, "boxgroup",
                       slot_block(1, "Boxwar", "Tank") + slot_block(2, "Boxclr", "Healer"));

    const MigrationReport report = plan_profile_migration(options);
    const MigrationResult applied = apply_profile_migration(report);
    REQUIRE(applied.ok);
    CHECK(applied.files_written == 2);
    CHECK(applied.backups_written == 1);

    const fs::path war = options.data_dir / "profiles" / "migrated" / "boxgroup" / "Boxwar.combat.ini";
    const fs::path clr = options.data_dir / "profiles" / "migrated" / "boxgroup" / "Boxclr.combat.ini";
    REQUIRE(fs::exists(war));
    REQUIRE(fs::exists(clr));

    CHECK(read_profile_schema_version(war) == kNativeProfileSchemaVersion);
    const std::string text = read_file_text(clr);
    CHECK(text.find("schema_version") != std::string::npos);
    CHECK(text.find("legacy_class_role=Healer") != std::string::npos);
    CHECK(text.find("boxgroup.ini") != std::string::npos);

    // A loadable native profile, not just a file with the right name.
    CombatProfile loaded;
    std::string error;
    REQUIRE(load_combat_profile(clr, loaded, error));
    CHECK(loaded.class_short == "CLR");

    const fs::path backup = report.backup_dir / "boxgroup.ini";
    REQUIRE(fs::exists(backup));
    CHECK(read_file_text(backup) == read_file_text(options.profiles_dir / "boxgroup.ini"));

    // The legacy file itself is untouched.
    REQUIRE(fs::exists(options.profiles_dir / "boxgroup.ini"));

    // Report text is written where the caller asks.
    const fs::path report_path = options.data_dir / "migration-report.txt";
    REQUIRE(report.write_text(report_path));
    const std::string report_text = read_file_text(report_path);
    CHECK(report_text.find("profiles_created=2") != std::string::npos);
    CHECK(report_text.find("CreateProfile") != std::string::npos);

    const MigrationResult rolled_back = rollback_profile_migration(report);
    CHECK(rolled_back.ok);
    CHECK_FALSE(fs::exists(war));
    CHECK_FALSE(fs::exists(clr));
    CHECK_FALSE(fs::exists(backup));
    CHECK(fs::exists(options.profiles_dir / "boxgroup.ini"));
}

TEST_CASE("migration: a second run skips what is already current") {
    const hailstone::test::ScratchDir scratch("migrate_rerun");
    MigrationOptions options = options_for(scratch.path());
    write_legacy_group(options.profiles_dir, "boxgroup",
                       slot_block(1, "Boxwar", "Tank") + slot_block(2, "Boxclr", "Healer"));

    const MigrationReport first = plan_profile_migration(options);
    REQUIRE(apply_profile_migration(first).ok);

    const MigrationReport second = plan_profile_migration(options);
    CHECK(second.profiles_created == 0);
    CHECK(second.profiles_skipped == 2);

    MigrationOptions force = options;
    force.overwrite_current = true;
    const MigrationReport third = plan_profile_migration(force);
    CHECK(third.profiles_created == 2);
}

TEST_CASE("migration: an older schema version is upgraded, not silently kept") {
    const hailstone::test::ScratchDir scratch("migrate_upgrade");
    MigrationOptions options = options_for(scratch.path());
    write_legacy_group(options.profiles_dir, "boxgroup", slot_block(1, "Boxwar", "Tank"));

    const fs::path dest = options.data_dir / "profiles" / "migrated" / "boxgroup" / "Boxwar.combat.ini";
    write_file_text(dest, "[General]\nclass=WAR\n\n[Migration]\nschema_version=1\n");
    REQUIRE(read_profile_schema_version(dest) == 1);

    const MigrationReport report = plan_profile_migration(options);
    REQUIRE(report.actions.size() >= 1);
    bool upgraded = false;
    for (const auto& action : report.actions) {
        if (action.kind == MigrationActionKind::CreateProfile) {
            upgraded = true;
            CHECK(action.detail.find("schema v1") != std::string::npos);
        }
    }
    CHECK(upgraded);
    REQUIRE(apply_profile_migration(report).ok);
    CHECK(read_profile_schema_version(dest) == kNativeProfileSchemaVersion);

    // With upgrades switched off, the old file is left alone.
    write_file_text(dest, "[General]\nclass=WAR\n\n[Migration]\nschema_version=1\n");
    MigrationOptions frozen = options;
    frozen.upgrade_outdated = false;
    const MigrationReport kept = plan_profile_migration(frozen);
    CHECK(kept.profiles_created == 0);
    CHECK(read_profile_schema_version(dest) == 1);
}

TEST_CASE("migration: a hostile character name cannot write outside the data dir") {
    const hailstone::test::ScratchDir scratch("migrate_escape");
    MigrationOptions options = options_for(scratch.path());
    write_legacy_group(options.profiles_dir, "boxgroup", slot_block(1, "../../evil", "Tank"));

    const MigrationReport report = plan_profile_migration(options);
    for (const auto& action : report.actions) {
        if (action.kind != MigrationActionKind::CreateProfile) continue;
        const fs::path relative = fs::relative(action.destination, options.data_dir);
        CHECK(relative.string().find("..") == std::string::npos);
    }
    REQUIRE(apply_profile_migration(report).ok);
    // The name is sanitized rather than escaped: one file in the group's folder,
    // its name carrying no path separator at all.
    const fs::path group_dir = options.data_dir / "profiles" / "migrated" / "boxgroup";
    std::vector<fs::path> written;
    for (const auto& entry : fs::directory_iterator(group_dir)) written.push_back(entry.path());
    REQUIRE(written.size() == 1);
    const std::string name = written.front().filename().string();
    CHECK(name.find('/') == std::string::npos);
    CHECK(name.find('\\') == std::string::npos);
    CHECK(name.rfind(".combat.ini") == name.size() - std::string(".combat.ini").size());
    CHECK(name.front() == '_');
}

TEST_CASE("migration: an unreadable legacy profile is reported and the rest still migrates") {
    const hailstone::test::ScratchDir scratch("migrate_bad");
    MigrationOptions options = options_for(scratch.path());
    write_legacy_group(options.profiles_dir, "good", slot_block(1, "Boxwar", "Tank"));
    write_file_text(options.profiles_dir / "broken.ini", "not an ini at all\n[unterminated\n");

    const MigrationReport report = plan_profile_migration(options);
    CHECK(report.profiles_read >= 1);
    CHECK(report.profiles_created >= 1);
    // Either the loader rejects it (counted) or it parses to zero slots; both are fine.
    CHECK(report.profiles_read + report.profiles_failed >= 2);
}
