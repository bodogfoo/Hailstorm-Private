#pragma once

// Engine selection for the native Hailstone combat stack.
//
// The client has two things that can drive a character's combat: the native
// C++ engine in src/combat (this area) and the legacy RGMercs Lua add-on. They
// must never both issue actions for one character in one tick, so every action
// goes through ActionIssuerGate, and what the native engine is allowed to do is
// decided here by per-group and per-character feature flags.
//
// The flags default to false: an install that changes nothing keeps the legacy
// RGMercs behaviour it had before. docs/native-combat.md "Engine selection"
// describes the switch and docs/unknowns.md records what is still guessed.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/config.h"
#include "core/ini_file.h"

namespace hailstone::combat {

// Which engine may issue gameplay actions for a character right now.
enum class CombatEngineKind : std::uint8_t {
    Disabled = 0,      // neither engine may act
    NativeHailstone,   // native C++ engine
    LegacyRGMercs,     // the RGMercs Lua add-on
};

inline constexpr std::string_view combat_engine_kind_name(CombatEngineKind k) noexcept {
    switch (k) {
    case CombatEngineKind::Disabled: return "Disabled";
    case CombatEngineKind::NativeHailstone: return "NativeHailstone";
    case CombatEngineKind::LegacyRGMercs: return "LegacyRGMercs";
    }
    return "Disabled";
}

// One switch per native subsystem so a player can adopt them one at a time.
// native_combat is the master switch: the other three do nothing without it.
struct NativeCombatFeatureFlags {
    bool native_combat = false;
    bool native_class_profiles = false;
    bool native_movement = false;
    bool native_spellcasting = false;

    bool any_enabled() const noexcept {
        return native_combat || native_class_profiles || native_movement || native_spellcasting;
    }
    bool all_enabled() const noexcept {
        return native_combat && native_class_profiles && native_movement && native_spellcasting;
    }
    bool operator==(const NativeCombatFeatureFlags& o) const noexcept {
        return native_combat == o.native_combat && native_class_profiles == o.native_class_profiles &&
               native_movement == o.native_movement && native_spellcasting == o.native_spellcasting;
    }
};

// Layered inputs. A layer that is set replaces the whole flag set, so a group or
// character can switch one subsystem back on/off without repeating the rest.
struct EngineSelectionRequest {
    NativeCombatFeatureFlags global_flags{};
    std::optional<NativeCombatFeatureFlags> group_flags;
    std::optional<NativeCombatFeatureFlags> character_flags;

    // What each engine asked for on this tick.
    bool native_requested = true;
    bool legacy_requested = false;
};

struct EngineSelection {
    CombatEngineKind kind = CombatEngineKind::Disabled;
    NativeCombatFeatureFlags effective_flags{};
    // True when both engines asked to act and one had to be suppressed.
    bool conflict = false;
    // Short player-facing label for the UI ("Native Hailstone", "Legacy RGMercs", "Disabled").
    std::string indicator;
    // One line saying which subsystems are native, for the UI/diagnostics.
    std::string detail;
    // Why the other engine was kept out, when it asked to act.
    std::string suppressed_reason;
};

NativeCombatFeatureFlags effective_native_flags(const EngineSelectionRequest& request);

EngineSelection resolve_engine_selection(const EngineSelectionRequest& request);

bool native_may_issue_actions(const EngineSelection& selection) noexcept;
bool legacy_may_issue_actions(const EngineSelection& selection) noexcept;

// The client's one answer for a Lua script's command line (AutomationEnv::
// script_actions_allowed): allowed whenever no flag configured a selection, so
// an install that sets nothing behaves as it did before the flags existed; else
// the gate decides and records a violation when the legacy engine may not act.
class ActionIssuerGate;
bool legacy_script_action_allowed(const std::optional<EngineSelection>& selection, ActionIssuerGate& gate);

// The legacy add-on's own scripts ("rgmercs", "rgmercs/heartbeat", ...): the ones
// the ownership gate stands down when the native engine owns the character.
bool legacy_script_name(std::string_view name) noexcept;

// What ScriptHost::list() says about one of them right now.
enum class LegacyScriptState : std::uint8_t {
    Absent = 0, // not loaded
    Running,    // loaded and executing: it is still competing with the native engine
    Paused,     // loaded and held: the stand-down already happened
    Other,      // starting, stopping, or ended with an error: not ours to pause mid-flight
};
LegacyScriptState legacy_script_state_from_status(std::string_view status) noexcept;

enum class LegacyStandDown : std::uint8_t {
    None = 0,     // the legacy engine may act here: leave its script alone
    LeaveRunning, // nothing to do this tick
    PauseNow,     // the native engine owns the character and the script is running
};

// Asked every tick, not once. RGMercs restarts itself (a reload, an error repair,
// the autoload at a new zone), and a one-shot pause leaves it running and
// competing with the native engine -- which is what the dashboard then reports.
LegacyStandDown plan_legacy_stand_down(const std::optional<EngineSelection>& selection,
                                       LegacyScriptState state) noexcept;

// Whether the legacy add-on's script may be started here at all. The script host
// asks before an autoload/required start (skipped, so it never loads) and before
// an explicit /lua run or /script run (refused with the reason). This is what
// keeps the zone-in autoload from reviving RGMercs behind the ownership gate.
bool legacy_script_may_start(const std::optional<EngineSelection>& selection, std::string_view name) noexcept;

// Parses/writes a layer's flags. Returns nullopt when the section holds no flag
// key at all, so layers stay distinguishable from "explicitly all false".
std::optional<NativeCombatFeatureFlags> read_engine_flags(const IniFile& ini, std::string_view section);
void write_engine_flags(IniFile& ini, std::string_view section, const NativeCombatFeatureFlags& flags);

// Section names used by the profile files (see docs/native-combat.md).
std::string engine_section_group(std::string_view group_id);
std::string engine_section_character(std::string_view character_name);

// The same flags in the client's own settings file, which uses dotted keys
// rather than sections: combat.native.<key>, combat.native.group.<id>.<key> and
// combat.native.character.<name>.<key>. A player who sets none of them keeps the
// behaviour the client had (the native controller driven by its own commands);
// setting one hands the choice to this gate.
inline constexpr std::string_view kEngineSettingPrefix = "combat.native.";
std::string engine_prefix_group(std::string_view group_id);
std::string engine_prefix_character(std::string_view character_name);

std::optional<NativeCombatFeatureFlags> read_engine_flags(const Config& config, std::string_view prefix);

// nullopt when no layer set any flag, so "nothing configured" stays distinct
// from "explicitly disabled".
std::optional<EngineSelection> resolve_engine_from_settings(const Config& config,
                                                            std::string_view group_id,
                                                            std::string_view character_name,
                                                            bool legacy_requested);

// One writer per tick. Every gameplay action the native engine issues must pass
// allow_native() first; the legacy bridge asks allow_legacy(). The gate records
// a violation instead of silently letting two engines fight over a character.
class ActionIssuerGate {
public:
    void begin_tick(const EngineSelection& selection, std::uint64_t now_ms);

    bool allow_native();
    bool allow_legacy();

    void reset();

    std::uint64_t tick_ms() const noexcept { return tick_ms_; }
    CombatEngineKind selected_kind() const noexcept { return selected_; }
    // Non-empty when the losing engine tried to act anyway.
    const std::string& last_violation() const noexcept { return last_violation_; }
    int violation_count() const noexcept { return violation_count_; }

private:
    CombatEngineKind selected_ = CombatEngineKind::Disabled;
    std::uint64_t tick_ms_ = 0;
    bool tick_open_ = false;
    CombatEngineKind issued_ = CombatEngineKind::Disabled;
    std::string last_violation_;
    int violation_count_ = 0;
};

} // namespace hailstone::combat
