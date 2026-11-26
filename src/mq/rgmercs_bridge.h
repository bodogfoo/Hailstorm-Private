#pragma once

// Hailstone-native bridge for RG Mercs: decouples gameplay/automation state from presentation.
// Keeps bounded logs & error banner state, module toggles, typed RG Mercs settings persisted per character inside
// Hailstone's data directory, read-only SQLite / Lua-table settings import, and a script-facing adapter queue.

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hailstone::mq {

class RGMercsBridge {
public:
    struct SettingEntry {
        std::string key;
        std::string category; // Combat, Targeting, Healing, Buffs, Debuffs, Movement, Loot, Class, General
        std::string type;     // bool, int, float, string, choice
        std::string value;
        std::string default_value;
        std::string description;
        double min_val = 0.0;
        double max_val = 100.0;
        std::vector<std::string> choices;
        bool imported = false;
    };

    struct ModuleState {
        std::string name;
        bool enabled = true;
        std::string status = "Idle"; // Idle, Running, Paused, Disabled, Error
        std::string detail;
    };

    struct LogEntry {
        std::string timestamp;
        std::string level; // info, warn, error
        std::string message;
    };

    struct LifecycleTransition {
        std::string timestamp;
        std::string from_state;
        std::string to_state;
        std::string reason;
    };

    struct ImportResult {
        bool ok = false;
        int imported_count = 0;
        std::string source;
        std::string error;
        std::vector<std::string> warnings;
    };

    struct Snapshot {
        std::string character;
        std::string server;
        std::string class_name;
        std::string settings_path;
        std::string imported_from;
        int imported_count = 0;
        std::string script_status; // "stopped", "starting", "running", "paused", "stopping", "reloading", "error"
        std::string runtime_state; // adapter status override / detail
        std::string last_error;
        int error_count = 0;
        bool suppress_script_ui = true;
        bool dirty = false;
        std::vector<std::string> available_dbs;
        std::vector<ModuleState> modules;
        std::vector<SettingEntry> settings;
        std::vector<LogEntry> logs;
        std::vector<LifecycleTransition> transitions;
    };

    static constexpr std::size_t kMaxLogs = 1000;
    static constexpr std::size_t kMaxTransitions = 64;

    RGMercsBridge();

    // Binds persistence to a character inside Hailstone's data_dir. Never writes outside `data_dir`.
    void attach_character(const std::filesystem::path& data_dir, const std::filesystem::path& character_dir,
                          std::string character_name, std::string server_name, std::string class_name);

    void clear_character();

    // Loads/saves settings from `settings_path_` (strictly inside `data_dir`).
    bool load_settings();
    bool save_settings();

    // Scans <data_dir>/scripts/config for RG Mercs SQLite DBs and Lua data tables.
    std::vector<std::filesystem::path> discover_import_sources() const;

    // Imports settings read-only from `source` (SQLite .db/.sqlite/.sqlite3 or key=value .lua data file),
    // verifying `source` is strictly inside `data_dir`. Never modifies the source file.
    ImportResult import_from_path(const std::filesystem::path& source);
    ImportResult import_best_source();

    // Settings access (used by both native UI and Lua adapter).
    bool set_setting(std::string_view key, std::string_view value, std::string_view category_hint = {},
                     bool queue_for_script = true);
    std::optional<SettingEntry> get_setting(std::string_view key) const;
    std::vector<SettingEntry> settings() const;
    void reset_to_defaults();

    // Module state & toggles.
    void set_module(std::string_view name, bool enabled, std::string_view status = {}, std::string_view detail = {},
                    bool queue_for_script = true);
    std::vector<ModuleState> modules() const;

    // Script status, error banner & bounded log buffer.
    void set_script_status(std::string_view status, std::string_view reason_or_error = {});
    void set_runtime_state(std::string_view state);
    void set_script_error(std::string_view error_msg);
    void clear_error();
    std::vector<LifecycleTransition> transitions() const;

    void append_log(std::string_view level, std::string_view message);
    void clear_logs();
    std::vector<LogEntry> logs() const;

    // Presentation boundary: whether Hailstone suppresses RG Mercs' own MQ/ImGui script windows when native UI owns it.
    bool suppress_script_ui() const;
    void set_suppress_script_ui(bool suppress);

    // Drains UI-initiated setting/module updates queued for a running RG Mercs adapter loop.
    // Module toggles are returned with key "module:<Name>".
    std::vector<std::pair<std::string, std::string>> take_pending_changes();

    Snapshot snapshot() const;

private:
    void init_defaults_locked();
    void refresh_sources_locked();
    SettingEntry* find_setting_locked(std::string_view key);
    const SettingEntry* find_setting_locked(std::string_view key) const;
    ModuleState* find_module_locked(std::string_view name);
    bool save_settings_locked();
    bool load_settings_locked();
    ImportResult import_from_path_locked(const std::filesystem::path& source);
    void append_log_locked(std::string_view level, std::string_view message);
    void record_transition_locked(std::string_view to_state, std::string_view reason);

    static bool is_under_root(const std::filesystem::path& root, const std::filesystem::path& candidate,
                              bool allow_equal = false);
    static std::string infer_category(std::string_view section_or_table, std::string_view key);
    static std::string infer_type(std::string_view value);
    static std::string normalize_value(std::string_view type, std::string_view raw,
                                       std::string_view fallback = {},
                                       double min_val = 0.0, double max_val = 0.0,
                                       bool clamp_range = false, bool* out_converted_ok = nullptr);
    static std::string normalize_status_name(std::string_view raw);
    static std::string current_timestamp();

    mutable std::mutex mutex_;
    std::filesystem::path data_dir_;
    std::filesystem::path character_dir_;
    std::filesystem::path settings_path_;
    std::string character_name_;
    std::string server_name_;
    std::string class_name_;
    std::string imported_from_;
    int imported_count_ = 0;
    std::string script_status_ = "stopped";
    std::string runtime_state_;
    std::string last_error_;
    int error_count_ = 0;
    bool suppress_script_ui_ = true;
    bool dirty_ = false;
    std::vector<std::string> available_dbs_;
    std::vector<ModuleState> modules_;
    std::vector<SettingEntry> settings_;
    std::deque<LogEntry> logs_;
    std::deque<LifecycleTransition> transitions_;
    std::vector<std::pair<std::string, std::string>> pending_changes_;
};

} // namespace hailstone::mq
