#include <doctest/doctest.h>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "core/ini_file.h"
#include "core/mq_login_import.h"
#include "core/server_profile.h"
#include "platform/credentials.h"
#include "scratch_dir.h"

namespace fs = std::filesystem;
using namespace hailstone;

namespace {

// Inverse/identical symmetric XOR encryption matching MacroQuest Next src/login/Login.cpp.
std::string xor_transform(std::string_view input, std::string_view key) {
    if (key.empty()) return std::string(input);
    std::string out(input);
    std::size_t idx = 0;
    for (char& c : out) c = static_cast<char>(c ^ key[++idx % key.size()]);
    return out;
}

void exec_sql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "sqlite error";
        sqlite3_free(err);
        FAIL(msg);
    }
}

void build_test_login_db(const fs::path& db_path, std::string_view master_pw) {
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(db_path.string().c_str(), &db) == SQLITE_OK);

    exec_sql(db, R"(
        CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE accounts (id INTEGER PRIMARY KEY AUTOINCREMENT, account TEXT NOT NULL, password TEXT, server_type TEXT);
        CREATE TABLE characters (id INTEGER PRIMARY KEY AUTOINCREMENT, account_id INTEGER, server TEXT, character TEXT);
        CREATE TABLE personas (character_id INTEGER, class TEXT, level INTEGER, last_seen INTEGER);
        CREATE TABLE profile_groups (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT);
        CREATE TABLE profiles (id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, character_id INTEGER, hotkey TEXT, sort_order INTEGER, will_load INTEGER);
    )");

    auto insert_account = [&](const char* acct, std::string_view pw, const char* srv_type) {
        const std::string enc = master_pw.empty() ? std::string(pw) : xor_transform(pw, master_pw);
        sqlite3_stmt* st = nullptr;
        REQUIRE(sqlite3_prepare_v2(db, "INSERT INTO accounts(account, password, server_type) VALUES(?, ?, ?)",
                                   -1, &st, nullptr) == SQLITE_OK);
        sqlite3_bind_text(st, 1, acct, -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 2, enc.data(), static_cast<int>(enc.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, srv_type, -1, SQLITE_TRANSIENT);
        REQUIRE(sqlite3_step(st) == SQLITE_DONE);
        sqlite3_finalize(st);
    };

    if (!master_pw.empty()) {
        exec_sql(db, "INSERT INTO settings(key, value) VALUES('master_pass', 'sha256_stub_hash');");
    }

    insert_account("ezbox01", "TankPass!24", "eqemu");
    insert_account("ezbox02", "ClericPass#99", "eqemu");
    exec_sql(db, "INSERT INTO characters(account_id, server, character) VALUES(1, 'EZ Server', 'BigTank');");
    exec_sql(db, "INSERT INTO characters(account_id, server, character) VALUES(2, 'EZ Server', 'HealBot');");
    exec_sql(db, "INSERT INTO personas(character_id, class, level, last_seen) VALUES(1, 'Warrior', 75, 100);");
    exec_sql(db, "INSERT INTO personas(character_id, class, level, last_seen) VALUES(2, 'Cleric', 75, 100);");
    exec_sql(db, "INSERT INTO profile_groups(name) VALUES('MainRaid');");
    exec_sql(db, "INSERT INTO profiles(group_id, character_id, hotkey, sort_order, will_load) VALUES(1, 1, 'F1', 1, 1);");
    exec_sql(db, "INSERT INTO profiles(group_id, character_id, hotkey, sort_order, will_load) VALUES(1, 2, 'F2', 2, 1);");

    sqlite3_close(db);
}

} // namespace

TEST_CASE("MQ AutoLogin importer: decrypts login.db XOR passwords into isolated keyring without writing plaintext passwords") {
    test::ScratchDir dir("hs24_mq_import");
    const fs::path db_path = dir.path() / "login.db";
    const fs::path servers = dir.path() / "servers";
    const fs::path roster = dir.path() / "hailstone24_roster.ini";

    build_test_login_db(db_path, "MyMasterSecret");

    platform::MemoryCredentials creds;
    core::MqImportOptions opts;
    opts.source_path = db_path;
    opts.master_password = "MyMasterSecret";
    opts.servers_dir = servers;
    opts.roster_out = roster;

    const auto res = core::import_mq_logins(opts, creds);
    CHECK(res.ok);
    CHECK(res.source_kind == "sqlite-login-db");
    CHECK(res.master_password_required);
    CHECK(res.accounts_imported == 2);
    CHECK(res.passwords_saved == 2);
    CHECK(res.characters_linked == 2);

    const std::string cred_srv = platform::credential_server(res.server_host, res.server_port);
    CHECK(creds.load(cred_srv, "ezbox01") == "TankPass!24");
    CHECK(creds.load(cred_srv, "ezbox02") == "ClericPass#99");

    const auto profile = load_profile(res.server_profile_id, servers);
    REQUIRE(profile.has_value());
    REQUIRE(profile->accounts.size() == 2);
    CHECK(profile->accounts[0].username == "ezbox01");
    CHECK(profile->accounts[0].character == "BigTank");
    CHECK(profile->accounts[1].username == "ezbox02");
    CHECK(profile->accounts[1].character == "HealBot");

    // Ensure generated roster INI matches hailstone24_broker schema and never leaks plaintext passwords.
    std::ifstream in(roster);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string roster_text = ss.str();
    CHECK(roster_text.find("BigTank") != std::string::npos);
    CHECK(roster_text.find("HealBot") != std::string::npos);
    CHECK(roster_text.find("TankPass!24") == std::string::npos);
    CHECK(roster_text.find("ClericPass#99") == std::string::npos);

    IniFile parsed_roster;
    REQUIRE(parsed_roster.load(roster));
    CHECK(parsed_roster.get_string("global", "default_server") == res.server_profile_id);
    CHECK(parsed_roster.get_string("global", "default_world") == "EZ Server");
    CHECK(parsed_roster.get_int("global", "auto_kick", 0) == 1);
    CHECK(parsed_roster.get_int("slot01", "enabled", 0) == 1);
    CHECK(parsed_roster.get_string("slot01", "session_id") == "slot01");
    CHECK(parsed_roster.get_string("slot01", "server_id") == res.server_profile_id);
    CHECK(parsed_roster.get_string("slot01", "world") == "EZ Server");
    CHECK(parsed_roster.get_string("slot01", "account") == "ezbox01");
    CHECK(parsed_roster.get_string("slot01", "character") == "BigTank");
    CHECK(parsed_roster.get_int("slot01", "headless", -1) == 0);
    CHECK(parsed_roster.get_int("slot02", "headless", -1) == 1);
}

TEST_CASE("MQ AutoLogin importer: supports MQ2AutoLogin.ini and caret-delimited lists") {
    test::ScratchDir dir("hs24_mq_ini_import");
    const fs::path ini_path = dir.path() / "MQ2AutoLogin.ini";
    const fs::path txt_path = dir.path() / "logins.txt";
    const fs::path servers = dir.path() / "servers";

    {
        std::ofstream out(ini_path);
        out << "[Settings]\nDefaultServer=EZ Server\n";
        out << "[Profiles]\nGroup1=bardbox\n";
        out << "[bardbox]\nServer=EZ Server\nAccount=ezbox03\nPassword=SongPassword!\nCharacter=MelodyBot\n";
    }
    {
        std::ofstream out(txt_path);
        out << "EZ Server^ezbox04^PullMonk^KickSecret!\n";
    }

    platform::MemoryCredentials creds;
    core::MqImportOptions o1;
    o1.source_path = ini_path;
    o1.servers_dir = servers;
    const auto r1 = core::import_mq_logins(o1, creds);
    CHECK(r1.ok);
    CHECK(r1.source_kind == "autologin-ini");
    CHECK(r1.accounts_imported == 1);
    CHECK(creds.load(platform::credential_server(r1.server_host, r1.server_port), "ezbox03") == "SongPassword!");

    core::MqImportOptions o2;
    o2.source_path = txt_path;
    o2.servers_dir = servers;
    const auto r2 = core::import_mq_logins(o2, creds);
    CHECK(r2.ok);
    CHECK(r2.source_kind == "delimited-list");
    CHECK(r2.accounts_imported == 1);
    CHECK(creds.load(platform::credential_server(r2.server_host, r2.server_port), "ezbox04") == "KickSecret!");
}
