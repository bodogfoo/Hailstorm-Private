#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "core/group_profile.h"
#include "core/ini_file.h"
#include "core/mq_login_import.h"
#include "platform/credentials.h"
#include "scratch_dir.h"

namespace fs = std::filesystem;
using namespace hailstone;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("GroupProfile: loads INI and JSON profiles sorted by launch_order and slot_number without plaintext secrets") {
    test::ScratchDir dir("hs24_group_profile_io");
    const fs::path profiles_dir = dir.path() / "profiles";
    fs::create_directories(profiles_dir);

    const fs::path ini_path = profiles_dir / "ez_raid.ini";
    {
        std::ofstream out(ini_path);
        out << "[profile]\n"
            << "name=EZ Six Box\n"
            << "description=Primary grind & pull team\n"
            << "default_server=ez_login\n"
            << "default_world=EZ Server\n"
            << "auto_login=1\n"
            << "password=ShouldBeIgnoredNeverRead\n"
            << "\n[slot02]\n"
            << "slot_number=2\n"
            << "launch_order=1\n"
            << "character=Stabbinyah\n"
            << "account=pumpkin\n"
            << "class_role=ROG\n"
            << "settings_path=servers/ez_login/characters/EZ Server/Stabbinyah/hailstone.ini\n"
            << "auto_login=1\n"
            << "headless=0\n"
            << "secret=DoNotKeep\n"
            << "\n[slot01]\n"
            << "slot_number=1\n"
            << "launch_order=3\n"
            << "character=Healbot\n"
            << "account=acorn\n"
            << "server_id=custom_server\n"
            << "world=EZ Test\n"
            << "class_role=CLR\n"
            << "auto_login=0\n"
            << "headless=1\n";
    }

    const fs::path json_path = profiles_dir / "ez_duo.json";
    {
        std::ofstream out(json_path);
        out << R"({
  "name": "EZ Duo",
  "description": "Two-box JSON profile",
  "default_server": "ez_login",
  "default_world": "EZ Server",
  "auto_login": true,
  "slots": [
    {
      "slot_number": 4,
      "launch_order": 2,
      "character": "MelodyBot",
      "account": "bardacct",
      "class_role": "BRD",
      "auto_login": true,
      "headless": true,
      "password": "NeverStoredInProfile"
    },
    {
      "slot_number": 1,
      "launch_order": 1,
      "character": "Stabbinyah",
      "account": "pumpkin",
      "class_role": "ROG",
      "settings_path": "characters/duo/stabbinyah.ini",
      "auto_login": true,
      "headless": false
    }
  ]
})";
    }

    const auto listed = core::list_group_profiles(profiles_dir);
    REQUIRE(listed.size() == 2);
    CHECK(listed[0].id == "ez_duo");
    CHECK(listed[1].id == "ez_raid");

    core::GroupProfile raid;
    std::string err;
    REQUIRE(core::load_group_profile(ini_path, raid, &err));
    CHECK(raid.name == "EZ Six Box");
    CHECK(raid.default_server_id == "ez_login");
    CHECK(raid.default_world == "EZ Server");
    REQUIRE(raid.slots.size() == 2);

    // Ordered deterministically by (launch_order, slot_number): slot 2 (order 1) first, slot 1 (order 3) second.
    CHECK(raid.slots[0].slot_number == 2);
    CHECK(raid.slots[0].launch_order == 1);
    CHECK(raid.slots[0].character == "Stabbinyah");
    CHECK(raid.slots[0].account == "pumpkin");
    CHECK(raid.slots[0].server_id == "ez_login");
    CHECK(raid.slots[0].world == "EZ Server");
    CHECK(raid.slots[0].class_role == "ROG");
    CHECK(raid.slots[0].auto_login);
    CHECK_FALSE(raid.slots[0].headless);

    CHECK(raid.slots[1].slot_number == 1);
    CHECK(raid.slots[1].launch_order == 3);
    CHECK(raid.slots[1].character == "Healbot");
    CHECK(raid.slots[1].server_id == "custom_server");
    CHECK(raid.slots[1].world == "EZ Test");
    CHECK(raid.slots[1].class_role == "CLR");
    CHECK_FALSE(raid.slots[1].auto_login);
    CHECK(raid.slots[1].headless);

    // JSON round-trip and secret stripping on save.
    core::GroupProfile duo;
    REQUIRE(core::load_group_profile(json_path, duo, &err));
    REQUIRE(duo.slots.size() == 2);
    CHECK(duo.slots[0].slot_number == 1);
    CHECK(duo.slots[0].launch_order == 1);
    CHECK(duo.slots[0].character == "Stabbinyah");
    CHECK(duo.slots[1].slot_number == 4);
    CHECK(duo.slots[1].launch_order == 2);
    CHECK(duo.slots[1].character == "MelodyBot");

    const fs::path saved_ini = dir.path() / "saved.ini";
    const fs::path saved_json = dir.path() / "saved.json";
    REQUIRE(core::save_group_profile_ini(duo, saved_ini, &err));
    REQUIRE(core::save_group_profile_json(raid, saved_json, &err));
    CHECK(slurp(saved_ini).find("NeverStoredInProfile") == std::string::npos);
    CHECK(slurp(saved_json).find("ShouldBeIgnoredNeverRead") == std::string::npos);
    CHECK(slurp(saved_json).find("password") == std::string::npos);
}

TEST_CASE("GroupProfile: selection, slot lookup, server/account/settings isolation, and path confinement") {
    test::ScratchDir dir("hs24_group_isolation");
    const fs::path data_dir = dir.path() / "data";
    const fs::path profiles_dir = data_dir / "profiles";
    fs::create_directories(profiles_dir);

    core::GroupProfile gp;
    gp.id = "team_alpha";
    gp.name = "Team Alpha";
    gp.default_server_id = "ez_srv";
    gp.default_world = "EZ Server";
    gp.auto_login = true;

    core::GroupSlot s1;
    s1.slot_number = 1;
    s1.launch_order = 2;
    s1.session_id = "slot01";
    s1.character = "Healbot";
    s1.account = "acct_clr";
    s1.server_id = "alt_srv";
    s1.world = "Alt World";
    s1.class_role = "CLR";
    s1.auto_login = false;

    core::GroupSlot s2;
    s2.slot_number = 2;
    s2.launch_order = 1;
    s2.session_id = "rogue_lead";
    s2.character = "Stabbinyah";
    s2.account = "pumpkin";
    s2.class_role = "ROG";
    s2.settings_path = "custom/stabbinyah_cfg";
    s2.auto_login = true;
    s2.headless = false;

    gp.slots = {s1, s2};
    REQUIRE(core::save_group_profile_ini(gp, profiles_dir / "team_alpha.ini"));

    // Path traversal out of profiles_dir is rejected.
    CHECK_FALSE(core::find_group_profile_file(profiles_dir, "../escape").has_value());
    REQUIRE(core::find_group_profile_file(profiles_dir, "team_alpha").has_value());
    REQUIRE(core::find_group_profile_file(profiles_dir, "Team Alpha").has_value());

    // Pre-autologin resolves BEFORE auto-login and respects launch order when --slot is omitted.
    core::PreLoginProfileState st;
    core::GroupProfile resolved_gp;
    core::GroupSlot resolved_slot;
    std::string err;
    REQUIRE(core::resolve_pre_autologin_profile(profiles_dir, data_dir, "team_alpha", "", "", st, &resolved_gp,
                                                &resolved_slot, &err));
    CHECK(resolved_slot.character == "Stabbinyah");
    CHECK(st.server == "ez_srv");
    CHECK(st.world == "EZ Server");
    CHECK(st.account == "pumpkin");
    CHECK(st.character == "Stabbinyah");
    CHECK(st.session_id == "rogue_lead");
    CHECK(st.auto_login);
    CHECK_FALSE(st.headless_bg);
    CHECK(st.settings_path == (data_dir / "custom/stabbinyah_cfg").lexically_normal().string());

    // Selecting slot 1 overrides to Healbot's isolated server/account/world/settings and respects auto_login=false.
    core::PreLoginProfileState st1;
    REQUIRE(core::resolve_pre_autologin_profile(profiles_dir, data_dir, "team_alpha", "1", "", st1, &resolved_gp,
                                                &resolved_slot, &err));
    CHECK(st1.server == "alt_srv");
    CHECK(st1.world == "Alt World");
    CHECK(st1.account == "acct_clr");
    CHECK(st1.character == "Healbot");
    CHECK_FALSE(st1.auto_login);
    CHECK(st1.settings_path ==
          (data_dir / "servers/alt_srv/characters/Alt_World/Healbot/hailstone.ini").lexically_normal().string());

    // Explicit CLI overrides are preserved even when profile is active.
    core::PreLoginProfileState cli_override;
    cli_override.account = "cli_account";
    cli_override.character = "CliChar";
    REQUIRE(core::resolve_pre_autologin_profile(profiles_dir, data_dir, "team_alpha", "1", "", cli_override));
    CHECK(cli_override.account == "cli_account");
    CHECK(cli_override.character == "CliChar");

    // Settings paths escaping data_dir are rejected.
    CHECK_FALSE(core::resolve_safe_settings_path(data_dir, "../outside.ini").has_value());
    CHECK_FALSE(core::resolve_safe_settings_path(data_dir, "/etc/passwd").has_value());
    CHECK(core::resolve_safe_settings_path(data_dir, "servers/ez/characters/Stabbinyah/hailstone.ini").has_value());

    // Resolve slot by character, session_id, or account.
    CHECK(core::resolve_group_slot(resolved_gp, "Healbot")->slot_number == 1);
    CHECK(core::resolve_group_slot(resolved_gp, "rogue_lead")->slot_number == 2);
    CHECK(core::resolve_group_slot(resolved_gp, "acct_clr")->slot_number == 1);
}

TEST_CASE("GroupProfile: passwordless export from MQ AutoLogin import keeps credentials solely in keyring") {
    test::ScratchDir dir("hs24_mq_profile_export");
    const fs::path ini_path = dir.path() / "MQ2AutoLogin.ini";
    const fs::path servers = dir.path() / "servers";
    const fs::path profiles = dir.path() / "profiles";

    {
        std::ofstream out(ini_path);
        out << "[Settings]\nDefaultServer=EZ Server\n\n"
            << "[Profiles]\n"
            << "Group1=MainGrind\n\n"
            << "[MainGrind_1]\nServer=EZ Server\nAccount=pumpkin\nPassword=SecretOne!\nCharacter=Stabbinyah\n\n"
            << "[MainGrind_2]\nServer=EZ Server\nAccount=acorn\nPassword=SecretTwo!\nCharacter=Healbot\n";
    }

    platform::MemoryCredentials creds;
    core::MqImportOptions opts;
    opts.source_path = ini_path;
    opts.servers_dir = servers;
    opts.profiles_dir = profiles;
    opts.write_group_profiles = true;

    const auto res = core::import_mq_logins(opts, creds);
    CHECK(res.ok);
    CHECK(res.accounts_imported == 2);
    CHECK(res.passwords_saved == 2);
    REQUIRE_FALSE(res.group_profiles_written.empty());

    const std::string cred_srv = platform::credential_server(res.server_host, res.server_port);
    CHECK(creds.load(cred_srv, "pumpkin") == "SecretOne!");
    CHECK(creds.load(cred_srv, "acorn") == "SecretTwo!");

    const auto exported_profiles = core::list_group_profiles(profiles);
    REQUIRE(exported_profiles.size() == 1);
    CHECK(exported_profiles[0].slots.size() == 2);
    CHECK(exported_profiles[0].slots[0].character == "Stabbinyah");
    CHECK(exported_profiles[0].slots[1].character == "Healbot");

    const std::string raw = slurp(exported_profiles[0].source_path);
    CHECK(raw.find("SecretOne!") == std::string::npos);
    CHECK(raw.find("SecretTwo!") == std::string::npos);
    CHECK(raw.find("Stabbinyah") != std::string::npos);
}

TEST_CASE("GroupProfile: built-in MQ Commander presets, extended slot fields INI/JSON roundtrip, and MQ profile INI import") {
    test::ScratchDir dir("hs24_group_presets");
    const fs::path profiles_dir = dir.path() / "profiles";
    fs::create_directories(profiles_dir);

    // 1. Named presets exist and generate valid, password-free profiles.
    const auto names = core::default_group_profile_preset_names();
    REQUIRE(names.size() == 5);
    for (const auto& preset_name : names) {
        const auto p = core::make_named_group_profile_preset(preset_name, "ez_login", "EZ Server");
        CHECK(p.preset_kind == preset_name);
        CHECK_FALSE(p.slots.empty());
        const fs::path ini_out = profiles_dir / (p.id + ".ini");
        const fs::path json_out = profiles_dir / (p.id + ".json");
        std::string err;
        REQUIRE(core::save_group_profile_ini(p, ini_out, &err));
        REQUIRE(core::save_group_profile_json(p, json_out, &err));

        core::GroupProfile from_ini, from_json;
        REQUIRE(core::load_group_profile(ini_out, from_ini, &err));
        REQUIRE(core::load_group_profile(json_out, from_json, &err));
        CHECK(from_ini.preset_kind == preset_name);
        CHECK(from_json.preset_kind == preset_name);
        CHECK(from_ini.slots.size() == p.slots.size());
        CHECK(from_json.slots.size() == p.slots.size());
        CHECK(slurp(ini_out).find("password") == std::string::npos);
        CHECK(slurp(json_out).find("password") == std::string::npos);
    }

    // 2. Extended slot fields (follow_target, combat_profile, script_module, script_settings, spell_loadout) round-trip.
    core::GroupProfile custom;
    custom.id = "ez_custom";
    custom.name = "EZ Custom";
    custom.preset_kind = "Melee Group";
    custom.default_server_id = "ez_login";
    custom.default_world = "EZ Server";
    core::GroupSlot rog;
    rog.slot_number = 1;
    rog.launch_order = 1;
    rog.character = "Stabbinyah";
    rog.account = "pumpkin";
    rog.class_role = "ROG";
    rog.follow_target = "Yavaha";
    rog.combat_profile = "MeleeDPS";
    rog.script_module = "rgmercs";
    rog.script_settings = "rog_ez_default";
    rog.spell_loadout = "MeleeUtility";
    rog.headless = false;
    custom.slots.push_back(rog);

    const fs::path custom_ini = profiles_dir / "ez_custom.ini";
    const fs::path custom_json = profiles_dir / "ez_custom.json";
    REQUIRE(core::save_group_profile_ini(custom, custom_ini));
    REQUIRE(core::save_group_profile_json(custom, custom_json));

    core::GroupProfile loaded_ini, loaded_json;
    REQUIRE(core::load_group_profile(custom_ini, loaded_ini));
    REQUIRE(core::load_group_profile(custom_json, loaded_json));
    for (const auto* lp : {&loaded_ini, &loaded_json}) {
        REQUIRE(lp->slots.size() == 1);
        CHECK(lp->slots[0].follow_target == "Yavaha");
        CHECK(lp->slots[0].combat_profile == "MeleeDPS");
        CHECK(lp->slots[0].script_module == "rgmercs");
        CHECK(lp->slots[0].script_settings == "rog_ez_default");
        CHECK(lp->slots[0].spell_loadout == "MeleeUtility");
    }

    // 3. Import MacroQuest profile INI preserving extended slot metadata while stripping passwords.
    const fs::path mq_ini = dir.path() / "MQProfiles.ini";
    {
        std::ofstream out(mq_ini);
        out << "[Profiles]\n"
            << "Group1=CasterBox\n\n"
            << "[CasterBox_1]\n"
            << "Server=EZ Server\n"
            << "Account=tankacct\n"
            << "Password=SuperSecretPassword1\n"
            << "Character=Yavaha\n"
            << "Class=WAR\n"
            << "CombatProfile=Tank\n"
            << "Script=rgmercs\n"
            << "Headless=0\n"
            << "Order=1\n\n"
            << "[CasterBox_2]\n"
            << "Server=EZ Server\n"
            << "Account=casteracct\n"
            << "Password=SuperSecretPassword2\n"
            << "Character=Noro's Son\n"
            << "Class=MAG\n"
            << "Follow=Yavaha\n"
            << "CombatProfile=CasterDPS\n"
            << "Script=rgmercs\n"
            << "ScriptSettings=mag_burn\n"
            << "SpellLoadout=NukeAndPet\n"
            << "Headless=1\n"
            << "Order=2\n";
    }

    const fs::path imported_dir = dir.path() / "imported_profiles";
    std::vector<fs::path> written;
    std::vector<std::string> warnings;
    REQUIRE(core::import_mq_profiles_from_ini(mq_ini, "ez_login", "EZ Server", imported_dir, written, &warnings));
    REQUIRE(written.size() == 1);

    const std::string imported_raw = slurp(written[0]);
    CHECK(imported_raw.find("SuperSecretPassword1") == std::string::npos);
    CHECK(imported_raw.find("SuperSecretPassword2") == std::string::npos);
    CHECK(imported_raw.find("password") == std::string::npos);

    core::GroupProfile imported;
    REQUIRE(core::load_group_profile(written[0], imported));
    REQUIRE(imported.slots.size() == 2);
    CHECK(imported.slots[0].character == "Yavaha");
    CHECK(imported.slots[0].combat_profile == "Tank");
    CHECK_FALSE(imported.slots[0].headless);
    CHECK(imported.slots[1].character == "Noro's Son");
    CHECK(imported.slots[1].follow_target == "Yavaha");
    CHECK(imported.slots[1].combat_profile == "CasterDPS");
    CHECK(imported.slots[1].script_module == "rgmercs");
    CHECK(imported.slots[1].script_settings == "mag_burn");
    CHECK(imported.slots[1].spell_loadout == "NukeAndPet");
    CHECK(imported.slots[1].headless);
}

