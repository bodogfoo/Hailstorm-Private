#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/server_profile.h"
#include "platform/credentials.h"
#include "platform/paths.h"

namespace hailstone::core {

struct MqImportedAccount {
    std::string account;
    std::string password; // wiped after saving to keyring
    std::string server_name;
    std::string server_type;
    std::string character;
    std::string character_class;
    int character_level = 0;
    std::string profile_group;
    std::string hotkey;
    int sort_order = 0;
    bool enabled = true;
    bool password_encrypted = false;
    bool dpapi_skipped = false;
};

struct MqImportOptions {
    std::filesystem::path source_path;      // login.db, MQ2AutoLogin.ini, or exported text file
    std::string master_password;            // optional MQ master password for login.db XOR decryption
    std::string target_server_id;           // existing profile id, or created profile id/name
    std::string fallback_server_name = "EZ Server";
    std::string fallback_host = "eqemulator.net";
    int fallback_port = 5999;
    std::filesystem::path servers_dir = platform::servers_dir();
    std::filesystem::path roster_out;       // optional output hailstone24_roster.ini (no passwords written)
    bool mark_box = true;
    bool mark_save_password = true;
};

struct MqImportResult {
    bool ok = false;
    std::string source_kind; // "sqlite-login-db", "autologin-ini", "delimited-list"
    bool master_password_required = false;
    std::size_t accounts_found = 0;
    std::size_t accounts_imported = 0;
    std::size_t passwords_saved = 0;
    std::size_t characters_linked = 0;
    std::size_t dpapi_blobs_skipped = 0;
    std::string server_profile_id;
    std::string server_host;
    int server_port = 0;
    std::vector<std::string> warnings;
    std::vector<std::string> imported_accounts;
    std::string error;
};

// Parse MQ logins without modifying Hailstone profiles or credentials (useful for previewing/tests).
// Caller should wipe passwords in returned records when done.
bool parse_mq_logins(const std::filesystem::path& source_path, std::string_view master_password,
                     std::vector<MqImportedAccount>& out_records, MqImportResult& meta);

// MacroQuest Next exact XOR routine from upstream src/login/Login.cpp:
//   int i = 0; for (char& c : out) c = static_cast<char>(c ^ key[++i % key.size()]);
std::string mq_xor_decrypt(std::string_view cipher, std::string_view master_password);

// Import all MQ usernames, passwords, servers, and characters into Hailstone's isolated ServerProfile
// + CredentialStore (system keyring / test MemoryCredentials). Never touches existing EQ or MQ files.
MqImportResult import_mq_logins(const MqImportOptions& opts,
                                platform::CredentialStore& credentials = platform::system_credentials());

} // namespace hailstone::core
