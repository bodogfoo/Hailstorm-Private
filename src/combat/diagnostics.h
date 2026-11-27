#pragma once

// Structured, deduplicated diagnostics for the native stack.
//
// The legacy add-on's habit of logging the same failure every tick is what makes
// a six-box session unreadable, so a repeated identical failure collapses into
// one record with a repeat count instead of a new line. Each record carries who
// it was about (character, class, server, group), which module decided, the
// action and target, the reason, and the game state at the time, so a support
// report can be read without the player's own log file.
//
// docs/native-combat.md "Diagnostics" documents the line format.

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::combat {

enum class DiagnosticModule : std::uint8_t {
    Engine = 0,
    Combat,
    Movement,
    Spellbook,
    Casting,
    Healing,
    Pets,
    Buffs,
    Recovery,
    Migration,
    Command,
    Ui,
};

inline constexpr std::string_view diagnostic_module_name(DiagnosticModule m) noexcept {
    switch (m) {
    case DiagnosticModule::Engine: return "Engine";
    case DiagnosticModule::Combat: return "Combat";
    case DiagnosticModule::Movement: return "Movement";
    case DiagnosticModule::Spellbook: return "Spellbook";
    case DiagnosticModule::Casting: return "Casting";
    case DiagnosticModule::Healing: return "Healing";
    case DiagnosticModule::Pets: return "Pets";
    case DiagnosticModule::Buffs: return "Buffs";
    case DiagnosticModule::Recovery: return "Recovery";
    case DiagnosticModule::Migration: return "Migration";
    case DiagnosticModule::Command: return "Command";
    case DiagnosticModule::Ui: return "Ui";
    }
    return "Engine";
}

enum class DiagnosticSeverity : std::uint8_t {
    Info = 0,
    Warning,
    Error,
};

inline constexpr std::string_view diagnostic_severity_name(DiagnosticSeverity s) noexcept {
    switch (s) {
    case DiagnosticSeverity::Info: return "info";
    case DiagnosticSeverity::Warning: return "warn";
    case DiagnosticSeverity::Error: return "error";
    }
    return "info";
}

struct DiagnosticRecord {
    std::uint64_t first_ms = 0;
    std::uint64_t last_ms = 0;
    std::string character;
    std::string server;
    std::string class_short;
    std::string group_id;
    DiagnosticModule module = DiagnosticModule::Engine;
    DiagnosticSeverity severity = DiagnosticSeverity::Info;
    std::string action;
    std::string target;
    std::string reason;
    std::string state;
    bool ok = true;
    // Total times this exact failure/success was reported, including the ones
    // folded into this record.
    std::uint32_t occurrences = 1;
    std::uint32_t suppressed_repeats = 0;
};

struct DiagnosticsOptions {
    // A repeat inside this window of an identical record only bumps the count.
    std::uint32_t dedupe_window_ms = 5000;
    // Oldest records are dropped past this many.
    std::size_t max_records = 200;
};

std::string format_diagnostic(const DiagnosticRecord& record);

class DiagnosticsSink {
public:
    void set_options(DiagnosticsOptions options) { options_ = options; }
    const DiagnosticsOptions& options() const noexcept { return options_; }

    void set_identity(std::string character,
                      std::string server,
                      std::string class_short,
                      std::string group_id);

    // Records one event, or folds it into the recent identical record. Returns
    // the record it landed on (never a dangling pointer: the deque outlives the
    // call and is only trimmed on insert).
    const DiagnosticRecord& emit(DiagnosticModule module,
                                 DiagnosticSeverity severity,
                                 std::string_view action,
                                 std::string_view target,
                                 bool ok,
                                 std::string_view reason,
                                 std::string_view state,
                                 std::uint64_t now_ms);

    // A legacy RGMercs/MQ command the native stack does not implement. Warns once
    // per distinct command per session; later mentions only bump the record's
    // repeat count. Returns true when this call reported it for the first time.
    bool emit_unsupported_command(std::string_view command, std::uint64_t now_ms);

    bool command_warned(std::string_view command) const;

    std::vector<DiagnosticRecord> records() const;
    std::vector<DiagnosticRecord> records(DiagnosticModule module) const;
    std::size_t size() const noexcept { return records_.size(); }
    std::uint64_t suppressed_total() const noexcept { return suppressed_total_; }

    // Most recent formatted line, or empty when nothing has been recorded.
    std::string last_line() const;

    void clear();

private:
    DiagnosticRecord* find_dedup_target(const DiagnosticRecord& candidate, std::uint64_t now_ms);

    DiagnosticsOptions options_{};
    std::string character_;
    std::string server_;
    std::string class_short_;
    std::string group_id_;
    std::deque<DiagnosticRecord> records_;
    std::uint64_t suppressed_total_ = 0;
    // Every distinct legacy command already reported. Kept even after its record
    // is trimmed, so a command warns once per session and never again.
    std::vector<std::string> warned_commands_;
};

} // namespace hailstone::combat
