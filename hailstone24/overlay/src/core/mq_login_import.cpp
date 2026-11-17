#include "core/mq_login_import.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "core/ascii_case.h"
#include "core/ini_file.h"
#include "core/wipe.h"

namespace hailstone::core {

namespace {

std::string trim_copy(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

std::string lower_copy(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool is_mostly_printable(std::string_view s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

bool looks_like_sqlite_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    char hdr[16] = {};
    in.read(hdr, sizeof(hdr));
    return in.gcount() >= 15 && std::string_view(hdr, 15) == "SQLite format 3";
}

bool parse_caret_line(std::string_view line, MqImportedAccount& out) {
    // Supported delimited MQ formats:
    //   server^account^character^password
    //   server^account^password
    //   account^password
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t sep = line.find('^', start);
        if (sep == std::string_view::npos) {
            parts.push_back(trim_copy(line.substr(start)));
            break;
        }
        parts.push_back(trim_copy(line.substr(start, sep - start)));
        start = sep + 1;
    }
    if (parts.size() == 4 && !parts[1].empty() && !parts[3].empty()) {
        out.server_name = parts[0];
        out.account = parts[1];
        out.character = parts[2];
        out.password = parts[3];
        return true;
    }
    if (parts.size() == 3 && !parts[1].empty() && !parts[2].empty()) {
        out.server_name = parts[0];
        out.account = parts[1];
        out.password = parts[2];
        return true;
    }
    if (parts.size() == 2 && !parts[0].empty() && !parts[1].empty()) {
        out.account = parts[0];
        out.password = parts[1];
        return true;
    }
    return false;
}

bool parse_sqlite_login_db(const std::filesystem::path& db_path, std::string_view master_password,
                           std::vector<MqImportedAccount>& out_records, MqImportResult& meta) {
    meta.source_kind = "sqlite-login-db";
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(db_path.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        meta.error = std::string("failed to open MQ login.db: ") + (db ? sqlite3_errmsg(db) : "out of memory");
        if (db) sqlite3_close(db);
        return false;
    }

    auto read_col = [](sqlite3_stmt* stmt, int col) -> std::string {
        const unsigned char* txt = sqlite3_column_text(stmt, col);
        const int len = sqlite3_column_bytes(stmt, col);
        if (!txt || len <= 0) return {};
        return std::string(reinterpret_cast<const char*>(txt), static_cast<std::size_t>(len));
    };

    bool has_master_pass = false;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT value FROM settings WHERE key='master_pass' LIMIT 1", -1, &stmt, nullptr) ==
            SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                has_master_pass = !read_col(stmt, 0).empty();
            }
            sqlite3_finalize(stmt);
        }
    }
    meta.master_password_required = has_master_pass;
    if (has_master_pass && master_password.empty()) {
        meta.warnings.push_back(
            "MQ login.db has a master_pass hash set, but no master password was supplied; stored passwords may be XOR-encrypted.");
    }

    // Detect whether modern schema (accounts.id / server_type) or legacy schema (accounts.account PK) is used.
    bool modern_schema = false;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA table_info(accounts)", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (read_col(stmt, 1) == "id") modern_schema = true;
            }
            sqlite3_finalize(stmt);
        }
    }

    const char* query_modern = R"(
        SELECT a.account, a.password, COALESCE(a.server_type, ''),
               COALESCE(c.character, ''), COALESCE(c.server, ''),
               COALESCE((SELECT p.class FROM personas p WHERE p.character_id = c.id ORDER BY p.last_seen DESC LIMIT 1), ''),
               COALESCE((SELECT p.level FROM personas p WHERE p.character_id = c.id ORDER BY p.last_seen DESC LIMIT 1), 0),
               COALESCE(pg.name, ''), COALESCE(pr.hotkey, ''), COALESCE(pr.sort_order, 0),
               COALESCE(pr.will_load, 1)
        FROM accounts a
        LEFT JOIN characters c ON c.account_id = a.id
        LEFT JOIN profiles pr ON pr.character_id = c.id
        LEFT JOIN profile_groups pg ON pg.id = pr.group_id
        ORDER BY a.id ASC, pr.sort_order ASC
    )";
    const char* query_legacy = R"(
        SELECT a.account, COALESCE(a.password, ''), '',
               COALESCE(c.character, ''), COALESCE(c.server, ''),
               COALESCE((SELECT p.class FROM personas p WHERE p.character_id = c.id ORDER BY p.last_seen DESC LIMIT 1), ''),
               COALESCE((SELECT p.level FROM personas p WHERE p.character_id = c.id ORDER BY p.last_seen DESC LIMIT 1), 0),
               COALESCE(pg.name, ''), COALESCE(pr.hotkey, ''), 0, 1
        FROM accounts a
        LEFT JOIN characters c ON LOWER(c.account) = LOWER(a.account)
        LEFT JOIN profiles pr ON pr.character_id = c.id
        LEFT JOIN profile_groups pg ON pg.id = pr.group_id
        ORDER BY a.account ASC
    )";

    sqlite3_stmt* stmt = nullptr;
    const char* sql = modern_schema ? query_modern : query_legacy;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        // Fallback minimal accounts + characters query in case optional tables (personas/profiles) are missing.
        sqlite3_finalize(stmt);
        stmt = nullptr;
        const char* fallback_sql = modern_schema
            ? "SELECT a.account, COALESCE(a.password, ''), COALESCE(a.server_type, ''), COALESCE(c.character, ''), COALESCE(c.server, ''), '', 0, '', '', 0, 1 FROM accounts a LEFT JOIN characters c ON c.account_id = a.id"
            : "SELECT a.account, COALESCE(a.password, ''), '', COALESCE(c.character, ''), COALESCE(c.server, ''), '', 0, '', '', 0, 1 FROM accounts a LEFT JOIN characters c ON LOWER(c.account) = LOWER(a.account)";
        if (sqlite3_prepare_v2(db, fallback_sql, -1, &stmt, nullptr) != SQLITE_OK) {
            meta.error = std::string("failed to query MQ login.db accounts: ") + sqlite3_errmsg(db);
            sqlite3_close(db);
            return false;
        }
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MqImportedAccount rec;
        rec.account = trim_copy(read_col(stmt, 0));
        std::string raw_pw = read_col(stmt, 1);
        rec.server_type = trim_copy(read_col(stmt, 2));
        rec.character = trim_copy(read_col(stmt, 3));
        rec.server_name = trim_copy(read_col(stmt, 4));
        rec.character_class = trim_copy(read_col(stmt, 5));
        rec.character_level = sqlite3_column_int(stmt, 6);
        rec.profile_group = trim_copy(read_col(stmt, 7));
        rec.hotkey = trim_copy(read_col(stmt, 8));
        rec.sort_order = sqlite3_column_int(stmt, 9);
        rec.enabled = sqlite3_column_int(stmt, 10) != 0;
        rec.password_encrypted = has_master_pass;

        if (has_master_pass && !master_password.empty() && !raw_pw.empty()) {
            rec.password = mq_xor_decrypt(raw_pw, master_password);
            if (!is_mostly_printable(rec.password)) {
                meta.warnings.push_back("Account '" + rec.account +
                                        "' decrypted to non-printable bytes; verify --mq-master-pass.");
            }
        } else {
            rec.password = std::move(raw_pw);
        }
        core::wipe(raw_pw);

        if (!rec.account.empty()) {
            out_records.push_back(std::move(rec));
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return true;
}

bool parse_autologin_ini(const std::filesystem::path& ini_path, std::vector<MqImportedAccount>& out_records,
                         MqImportResult& meta) {
    meta.source_kind = "autologin-ini";
    IniFile primary;
    if (!primary.load(ini_path)) {
        meta.error = "failed to read MQ ini file: " + ini_path.string();
        return false;
    }

    IniFile ini_storage;
    const IniFile* active = &primary;
    if (auto alt = primary.get("Settings", "IniLocation"); alt && !trim_copy(*alt).empty()) {
        std::filesystem::path alt_path(trim_copy(*alt));
        if (ini_storage.load(alt_path)) active = &ini_storage;
        else meta.warnings.push_back("Settings.IniLocation could not be read: " + alt_path.string());
    }

    std::size_t sort_idx = 0;

    // 1) Check [Profiles] ProfileN=<SectionName> for _Blob or caret-delimited entries
    for (const std::string& key : active->keys("Profiles")) {
        const std::string val = trim_copy(active->get_string("Profiles", key));
        if (val.empty()) continue;

        MqImportedAccount caret;
        if (parse_caret_line(val, caret)) {
            caret.profile_group = "Profiles";
            caret.sort_order = static_cast<int>(++sort_idx);
            out_records.push_back(std::move(caret));
            continue;
        }

        // Section referenced by ProfileN=<GroupSection>
        for (const std::string& gkey : active->keys(val)) {
            if (gkey.find("_Blob") != std::string::npos) {
                ++meta.dpapi_blobs_skipped;
                continue;
            }
            const std::string gval = trim_copy(active->get_string(val, gkey));
            MqImportedAccount rec;
            if (parse_caret_line(gval, rec)) {
                rec.profile_group = val;
                rec.sort_order = static_cast<int>(++sort_idx);
                out_records.push_back(std::move(rec));
            }
        }
    }

    // 2) SessionN or Account sections containing Password=
    for (const std::string& sec : active->sections()) {
        if (iequals(sec, "Settings") || iequals(sec, "Profiles")) continue;
        std::string pw = active->get_string(sec, "Password");
        if (pw.empty()) continue;

        std::string account;
        if (sec.size() >= 7 && iequals(std::string_view(sec).substr(0, 7), "Session")) {
            account = trim_copy(active->get_string(sec, "StationName"));
            if (account.empty()) account = trim_copy(active->get_string(sec, "Account"));
        } else {
            account = trim_copy(active->get_string(sec, "StationName"));
            if (account.empty()) account = trim_copy(sec);
        }
        if (account.empty()) {
            core::wipe(pw);
            continue;
        }

        MqImportedAccount rec;
        rec.profile_group = sec;
        rec.account = std::move(account);
        rec.password = std::move(pw);
        rec.server_name = trim_copy(active->get_string(sec, "Server"));
        rec.character = trim_copy(active->get_string(sec, "Character"));
        rec.character_class = trim_copy(active->get_string(sec, "Class"));
        rec.character_level = active->get_int(sec, "Level", 0);
        rec.sort_order = static_cast<int>(++sort_idx);
        out_records.push_back(std::move(rec));
    }

    if (meta.dpapi_blobs_skipped > 0) {
        meta.warnings.push_back(
            "Skipped " + std::to_string(meta.dpapi_blobs_skipped) +
            " legacy Windows DPAPI (_Blob) entries that require Windows CryptUnprotectData; export accounts or use MQ2AutoLogin login.db instead.");
    }
    return true;
}

bool parse_delimited_file(const std::filesystem::path& path, std::vector<MqImportedAccount>& out_records,
                          MqImportResult& meta) {
    meta.source_kind = "delimited-list";
    std::ifstream in(path);
    if (!in) {
        meta.error = "failed to open login file: " + path.string();
        return false;
    }
    std::string line;
    int sort_idx = 0;
    while (std::getline(in, line)) {
        std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') continue;
        MqImportedAccount rec;
        if (parse_caret_line(trimmed, rec)) {
            rec.sort_order = ++sort_idx;
            out_records.push_back(std::move(rec));
        }
    }
    return !out_records.empty();
}

} // namespace

std::string mq_xor_decrypt(std::string_view cipher, std::string_view master_password) {
    std::string out(cipher);
    if (master_password.empty()) return out;
    const std::size_t mod_val = master_password.size();
    std::size_t i = 0;
    for (char& c : out) {
        c = static_cast<char>(c ^ master_password[++i % mod_val]);
    }
    return out;
}

bool parse_mq_logins(const std::filesystem::path& source_path, std::string_view master_password,
                     std::vector<MqImportedAccount>& out_records, MqImportResult& meta) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source_path, ec)) {
        meta.error = "MQ login source file not found: " + source_path.string();
        return false;
    }
    const std::string ext = lower_copy(source_path.extension().string());
    if (ext == ".db" || ext == ".sqlite" || ext == ".sqlite3" || looks_like_sqlite_file(source_path)) {
        return parse_sqlite_login_db(source_path, master_password, out_records, meta);
    }
    if (ext == ".ini" || ext == ".cfg") {
        return parse_autologin_ini(source_path, out_records, meta);
    }
    if (parse_delimited_file(source_path, out_records, meta)) return true;
    return parse_autologin_ini(source_path, out_records, meta);
}

MqImportResult import_mq_logins(const MqImportOptions& opts, platform::CredentialStore& credentials) {
    MqImportResult res;
    std::vector<MqImportedAccount> records;
    if (!parse_mq_logins(opts.source_path, opts.master_password, records, res)) {
        for (auto& r : records) core::wipe(r.password);
        return res;
    }
    res.accounts_found = records.size();

    std::error_code ec;
    std::filesystem::create_directories(opts.servers_dir, ec);
    auto existing_profiles = list_profiles(opts.servers_dir);

    ServerProfile target;
    const ServerProfile* found = nullptr;
    if (!opts.target_server_id.empty()) {
        found = find_profile(existing_profiles, opts.target_server_id);
        if (!found) {
            for (const auto& p : existing_profiles) {
                if (iequals(p.name, opts.target_server_id)) {
                    found = &p;
                    break;
                }
            }
        }
    } else if (!existing_profiles.empty()) {
        found = &existing_profiles.front();
    }

    if (found) {
        target = *found;
    } else {
        const std::string display = opts.target_server_id.empty() ? opts.fallback_server_name : opts.target_server_id;
        target.id = make_profile_id(display, opts.servers_dir);
        target.name = display;
        target.host = opts.fallback_host;
        target.port = opts.fallback_port;
        target.remember = true;
    }

    const std::string cred_server = platform::credential_server(target.host, target.port);
    if (!credentials.available()) {
        res.warnings.push_back(
            "System keyring (libsecret) unavailable; accounts & characters imported without saving keyring secrets.");
    }

    std::set<std::string> unique_accounts;
    std::vector<MqImportedAccount> roster_slots;

    for (auto& rec : records) {
        if (rec.account.empty()) continue;

        SavedAccount* acct = find_account(target, rec.account);
        if (!acct) {
            target.accounts.push_back(SavedAccount{
                .username = rec.account,
                .character = rec.character,
                .world = rec.server_name,
                .characters = {},
                .box = opts.mark_box,
                .save_password = false,
            });
            acct = &target.accounts.back();
        } else {
            if (acct->character.empty() && !rec.character.empty()) acct->character = rec.character;
            if (acct->world.empty() && !rec.server_name.empty()) acct->world = rec.server_name;
            if (opts.mark_box) acct->box = true;
        }

        if (!rec.character.empty()) {
            const std::string world_key =
                rec.server_name.empty() ? std::string("default") : saved_character_key_part(rec.server_name);
            const std::string char_key = saved_character_key_part(rec.character);
            auto it = std::find_if(acct->characters.begin(), acct->characters.end(), [&](const SavedCharacter& c) {
                return iequals(c.name, char_key) && iequals(c.world, world_key);
            });
            if (it == acct->characters.end()) {
                acct->characters.push_back(SavedCharacter{
                    .name = char_key,
                    .world = world_key,
                    .level = rec.character_level,
                    .classes = rec.character_class,
                });
                ++res.characters_linked;
            }
        }

        if (!rec.password.empty() && opts.mark_save_password && credentials.available()) {
            if (credentials.save(cred_server, rec.account, rec.password)) {
                acct->save_password = true;
                ++res.passwords_saved;
            } else {
                res.warnings.push_back("Keyring refused password for account '" + rec.account + "': " +
                                       credentials.error());
            }
        }
        core::wipe(rec.password);

        if (unique_accounts.insert(lower_copy(rec.account)).second) {
            res.imported_accounts.push_back(rec.account);
            if (target.username.empty()) target.username = rec.account;
            roster_slots.push_back(rec);
        }
    }

    if (!save_profile(target, opts.servers_dir)) {
        res.error = "failed to write server profile to " + profile_ini(target.id, opts.servers_dir).string();
        return res;
    }

    if (!opts.roster_out.empty() && !roster_slots.empty()) {
        IniFile roster;
        const std::size_t slot_count = std::min<std::size_t>(roster_slots.size(), 24);
        std::string shared_world;
        bool worlds_differ = false;
        for (std::size_t idx = 0; idx < slot_count; ++idx) {
            const std::string& w = roster_slots[idx].server_name;
            if (w.empty()) continue;
            if (shared_world.empty()) shared_world = w;
            else if (!iequals(shared_world, w)) worlds_differ = true;
        }

        roster.set("global", "default_server", target.id);
        if (!shared_world.empty() && !worlds_differ) {
            roster.set("global", "default_world", shared_world);
        }
        roster.set("global", "host", target.host);
        roster.set_int("global", "port", target.port);
        roster.set_int("global", "count", static_cast<int>(slot_count));
        roster.set_int("global", "bg_tick_hz", 25);
        roster.set("global", "gpu_mode", "cached");
        roster.set_int("global", "auto_kick", 1);

        for (std::size_t idx = 0; idx < slot_count; ++idx) {
            const auto& r = roster_slots[idx];
            const std::size_t one_based = idx + 1;
            const std::string two = (one_based < 10 ? "0" : "") + std::to_string(one_based);
            const std::string sec = "slot" + two;
            roster.set_int(sec, "enabled", 1);
            roster.set(sec, "session_id", sec);
            roster.set(sec, "server_id", target.id);
            if (!r.server_name.empty()) roster.set(sec, "world", r.server_name);
            roster.set(sec, "account", r.account);
            if (!r.character.empty()) roster.set(sec, "character", r.character);
            if (!r.character_class.empty()) roster.set(sec, "class", r.character_class);
            roster.set_int(sec, "headless", idx == 0 ? 0 : 1);
        }
        if (!roster.save(opts.roster_out)) {
            res.warnings.push_back("Failed to write roster file: " + opts.roster_out.string());
        }
    }

    res.accounts_imported = unique_accounts.size();
    res.server_profile_id = target.id;
    res.server_host = target.host;
    res.server_port = target.port;
    res.ok = true;
    return res;
}

} // namespace hailstone::core
