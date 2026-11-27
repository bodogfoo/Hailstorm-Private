#pragma once

// MacroQuest-style group/box launcher profiles (profiles/<name>.ini and profiles/<name>.json).
// Never stores plaintext passwords or session secrets; credentials stay strictly in platform::CredentialStore.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::core {

struct MqImportedAccount;

struct GroupSlot {
    int slot_number = 1;           // 1..24 slot index within the group profile
    int launch_order = 1;          // ascending order in which slots are launched
    bool enabled = true;
    std::string session_id;        // e.g. "slot01"
    std::string character;
    std::string account;
    std::string server_id;         // Hailstone server profile id/name override for this slot
    std::string world;             // target world server
    std::string class_role;        // e.g. "WAR", "CLR", "Tank", "Healer"
    std::string settings_path;     // isolated character settings path/folder inside Hailstone data_dir
    bool auto_login = true;
    bool headless = true;
    bool auto_kick = true;
    int bg_tick_hz = 25;
    std::string gpu_mode = "cached";
    std::string group_leader;
    // Extended MQ Commander-style slot settings (all optional, password-free).
    std::string follow_target;     // character name to follow by default
    std::string combat_profile;    // e.g. "Tank", "MeleeDPS", "CasterDPS", "Healer", "Solo"
    std::string script_module;     // e.g. "rgmercs" or ""
    std::string script_settings;   // relative profile/config identifier for script_module
    std::string spell_loadout;     // named spell loadout preset for this slot
};

struct GroupProfile {
    std::string id;                // filename stem (e.g. "ez_6box")
    std::string name;              // human display name
    std::string description;
    std::string preset_kind;       // e.g. "Main Tank", "Caster Group", "Melee Group", "Raid", "Solo"
    std::string default_server_id;
    std::string default_world;
    std::filesystem::path source_path;
    bool auto_login = true;
    int bg_tick_hz = 25;
    std::string gpu_mode = "cached";
    bool auto_kick = true;
    std::vector<GroupSlot> slots;
};

struct PreLoginProfileState {
    std::string profile;
    std::string slot;
    std::string server;
    std::string world;
    std::string account;
    std::string character;
    std::string settings_path;
    std::string session_id;
    std::string gpu_mode;
    std::string follow_target;
    std::string combat_profile;
    std::string script_module;
    std::string spell_loadout;
    int bg_tick_hz = 0;
    bool auto_login = false;
    bool auto_kick = false;
    bool headless_bg = false;
};

// Supported built-in MQ Commander-style group profile presets.
std::vector<std::string> default_group_profile_preset_names();

// Build a deterministic passwordless preset profile ("Main Tank", "Caster Group", "Melee Group", "Raid", "Solo").
GroupProfile make_named_group_profile_preset(std::string_view preset_name,
                                             std::string_view server_id = {},
                                             std::string_view world = {});

// Sanitize a profile name or id into a filesystem-safe slug (lowercase [a-z0-9_-]).
std::string sanitize_group_profile_id(std::string_view raw);

// The session id a slot is launched under when its roster row names none:
// `slotNN` for slot numbers 1..24. `client_app_login.cpp` and the session broker
// (`tools/hailstone24_manager`) both use it, so the bus roster, the verified kill
// and the combat window key on one identity whichever path started the client.
std::string format_slot_id(int slot_number);

// Resolve a `--profile NAME_OR_PATH` or profile id against `profiles_dir`.
// Rejects path traversal outside `profiles_dir` for relative names.
std::optional<std::filesystem::path> find_group_profile_file(const std::filesystem::path& profiles_dir,
                                                             std::string_view name_or_path);

// List all valid profiles (*.ini, *.json) under `profiles_dir`, sorted by name (case-insensitively), then id.
std::vector<GroupProfile> list_group_profiles(const std::filesystem::path& profiles_dir);

// Load an INI (.ini) or JSON (.json) group profile file. Strips/ignores any secret or password fields,
// fills missing slot defaults (session_id, server_id, world, launch_order), and sorts slots deterministically
// by (launch_order, slot_number).
bool load_group_profile(const std::filesystem::path& path, GroupProfile& out, std::string* error = nullptr);

// Save a group profile in INI or JSON format without ever writing password or secret keys.
bool save_group_profile_ini(const GroupProfile& profile, const std::filesystem::path& path,
                            std::string* error = nullptr);
bool save_group_profile_json(const GroupProfile& profile, const std::filesystem::path& path,
                             std::string* error = nullptr);

// Find a slot by 1-based slot_number string ("1", "02"), session_id ("slot02"), character, or account.
const GroupSlot* resolve_group_slot(const GroupProfile& profile, std::string_view slot_or_name);

// Confine `requested` strictly inside `data_dir`. Rejects ".." traversal or external paths with std::nullopt.
std::optional<std::filesystem::path> resolve_safe_settings_path(const std::filesystem::path& data_dir,
                                                                const std::filesystem::path& requested);

// Isolated default relative path for a slot's character settings inside data_dir:
// servers/<server_id>/characters/<world>/<character>/hailstone.ini
std::string default_slot_settings_relpath(std::string_view server_id, std::string_view world,
                                          std::string_view character);

// Resolve effective settings file/dir path strictly confined within `data_dir`.
std::optional<std::filesystem::path> resolve_effective_settings_path(const std::filesystem::path& data_dir,
                                                                     const GroupProfile& profile,
                                                                     const GroupSlot& slot);

// Apply group profile + slot selection BEFORE auto-login begins, preserving any explicit CLI overrides already
// set in `state`. Returns true if a profile was applied (or none requested), false on explicit error.
bool resolve_pre_autologin_profile(const std::filesystem::path& profiles_dir,
                                   const std::filesystem::path& data_dir,
                                   std::string_view profile_arg,
                                   std::string_view slot_arg,
                                   std::string_view configured_group_profile,
                                   PreLoginProfileState& state,
                                   GroupProfile* out_profile = nullptr,
                                   GroupSlot* out_slot = nullptr,
                                   std::string* out_error = nullptr);

// Group MQ AutoLogin records by `profile_group` (falling back to `default_profile_name`) and write
// passwordless `profiles/<id>.ini` files inside `profiles_dir`. Returns written profile paths.
std::vector<std::filesystem::path> export_group_profiles_from_mq(const std::vector<MqImportedAccount>& records,
                                                                   std::string_view server_id,
                                                                   std::string_view fallback_world,
                                                                   std::string_view default_profile_name,
                                                                   const std::filesystem::path& profiles_dir,
                                                                   std::vector<std::string>* warnings = nullptr);

// Import MacroQuest profile/roster INI definitions (preserving roles, follow targets, combat/script settings,
// spell loadouts, startup order, and headless preferences) while strictly discarding any password/secret keys.
bool import_mq_profiles_from_ini(const std::filesystem::path& mq_ini_path,
                                 std::string_view server_id,
                                 std::string_view fallback_world,
                                 const std::filesystem::path& profiles_dir,
                                 std::vector<std::filesystem::path>& out_written,
                                 std::vector<std::string>* warnings = nullptr);

} // namespace hailstone::core
