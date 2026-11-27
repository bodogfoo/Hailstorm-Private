#include "combat/engine_selection.h"

#include <string>
#include <utility>

#include "core/ascii_case.h"
#include "core/config.h"
#include "core/ini_file.h"

namespace hailstone::combat {

namespace {

std::string trim_copy(std::string_view sv) {
    std::size_t b = 0;
    while (b < sv.size() && (sv[b] == ' ' || sv[b] == '\t' || sv[b] == '\r' || sv[b] == '\n')) ++b;
    std::size_t e = sv.size();
    while (e > b && (sv[e - 1] == ' ' || sv[e - 1] == '\t' || sv[e - 1] == '\r' || sv[e - 1] == '\n')) --e;
    return std::string(sv.substr(b, e - b));
}

bool parse_flag_value(std::string_view raw, bool fallback) {
    const std::string t = trim_copy(raw);
    if (t.empty()) return fallback;
    if (iequals(t, "1") || iequals(t, "true") || iequals(t, "yes") || iequals(t, "on")) return true;
    if (iequals(t, "0") || iequals(t, "false") || iequals(t, "no") || iequals(t, "off")) return false;
    return fallback;
}

std::string flags_detail(const NativeCombatFeatureFlags& f) {
    std::string out;
    auto add = [&out](std::string_view label) {
        if (!out.empty()) out += ", ";
        out += label;
    };
    if (f.native_combat) add("combat");
    if (f.native_class_profiles) add("class profiles");
    if (f.native_movement) add("movement");
    if (f.native_spellcasting) add("spellcasting");
    if (out.empty()) out = "no native subsystem";
    return out;
}

// A settings layer holds only the keys a player wrote, and a group or character
// layer overrides just those: `combat.native.group.<id>.native_class_profiles=on`
// must not switch the global master switch back off merely by not mentioning it.
struct FlagPatch {
    std::optional<bool> combat;
    std::optional<bool> class_profiles;
    std::optional<bool> movement;
    std::optional<bool> spellcasting;

    bool any() const {
        return combat.has_value() || class_profiles.has_value() || movement.has_value() ||
               spellcasting.has_value();
    }
};

FlagPatch read_flag_patch(const Config& config, std::string_view prefix) {
    const std::string base(prefix);
    FlagPatch patch;
    if (const auto value = config.get(base + "native_combat")) patch.combat = parse_flag_value(*value, false);
    if (const auto value = config.get(base + "native_class_profiles")) {
        patch.class_profiles = parse_flag_value(*value, false);
    }
    if (const auto value = config.get(base + "native_movement")) patch.movement = parse_flag_value(*value, false);
    if (const auto value = config.get(base + "native_spellcasting")) {
        patch.spellcasting = parse_flag_value(*value, false);
    }
    return patch;
}

void apply_flag_patch(NativeCombatFeatureFlags& flags, const FlagPatch& patch) {
    if (patch.combat) flags.native_combat = *patch.combat;
    if (patch.class_profiles) flags.native_class_profiles = *patch.class_profiles;
    if (patch.movement) flags.native_movement = *patch.movement;
    if (patch.spellcasting) flags.native_spellcasting = *patch.spellcasting;
}

} // namespace

NativeCombatFeatureFlags effective_native_flags(const EngineSelectionRequest& request) {
    NativeCombatFeatureFlags flags = request.global_flags;
    if (request.group_flags) flags = *request.group_flags;
    if (request.character_flags) flags = *request.character_flags;
    // The master switch gates the others: a subordinate flag on its own is
    // ignored rather than half-starting the native engine.
    if (!flags.native_combat) {
        flags.native_class_profiles = false;
        flags.native_movement = false;
        flags.native_spellcasting = false;
    }
    return flags;
}

// The layers as asked for, before the master switch gates them: used to explain
// why native was ignored when only a subordinate flag was set.
bool any_subordinate_requested(const EngineSelectionRequest& request) {
    NativeCombatFeatureFlags flags = request.global_flags;
    if (request.group_flags) flags = *request.group_flags;
    if (request.character_flags) flags = *request.character_flags;
    return flags.native_class_profiles || flags.native_movement || flags.native_spellcasting;
}

EngineSelection resolve_engine_selection(const EngineSelectionRequest& request) {
    EngineSelection out;
    out.effective_flags = effective_native_flags(request);

    const bool native_wants = request.native_requested && out.effective_flags.native_combat;
    const bool legacy_wants = request.legacy_requested;

    if (native_wants && legacy_wants) {
        // Native wins because the player explicitly switched it on; the legacy
        // add-on is told to stand down instead of both driving the character.
        out.kind = CombatEngineKind::NativeHailstone;
        out.conflict = true;
        out.suppressed_reason = "Legacy RGMercs asked to act while native combat is enabled; legacy suppressed";
    } else if (native_wants) {
        out.kind = CombatEngineKind::NativeHailstone;
    } else if (legacy_wants) {
        out.kind = CombatEngineKind::LegacyRGMercs;
        // Asked for as a layer, even though the gate above has already cleared it:
        // the player should be told why native did not start, not left guessing.
        if (any_subordinate_requested(request)) {
            out.suppressed_reason = "Native subsystem flags set without native_combat; native ignored";
        }
    } else {
        out.kind = CombatEngineKind::Disabled;
        if (!out.effective_flags.native_combat) {
            out.suppressed_reason = "No engine enabled";
        } else {
            out.suppressed_reason = "Native combat enabled but no character/engine request";
        }
    }

    switch (out.kind) {
    case CombatEngineKind::NativeHailstone:
        out.indicator = "Native Hailstone";
        out.detail = flags_detail(out.effective_flags);
        break;
    case CombatEngineKind::LegacyRGMercs:
        out.indicator = "Legacy RGMercs";
        out.detail = "RGMercs Lua add-on";
        break;
    case CombatEngineKind::Disabled:
        out.indicator = "Disabled";
        out.detail = "combat engine off";
        break;
    }
    return out;
}

bool native_may_issue_actions(const EngineSelection& selection) noexcept {
    return selection.kind == CombatEngineKind::NativeHailstone;
}

bool legacy_may_issue_actions(const EngineSelection& selection) noexcept {
    return selection.kind == CombatEngineKind::LegacyRGMercs;
}

std::optional<NativeCombatFeatureFlags> read_engine_flags(const IniFile& ini, std::string_view section) {
    static constexpr std::string_view kMaster = "native_combat";
    auto master = ini.get(section, kMaster);
    auto profiles = ini.get(section, "native_class_profiles");
    auto movement = ini.get(section, "native_movement");
    auto casting = ini.get(section, "native_spellcasting");
    if (!master && !profiles && !movement && !casting) return std::nullopt;

    NativeCombatFeatureFlags flags;
    flags.native_combat = parse_flag_value(master ? *master : std::string_view{}, false);
    flags.native_class_profiles = parse_flag_value(profiles ? *profiles : std::string_view{}, false);
    flags.native_movement = parse_flag_value(movement ? *movement : std::string_view{}, false);
    flags.native_spellcasting = parse_flag_value(casting ? *casting : std::string_view{}, false);
    return flags;
}

void write_engine_flags(IniFile& ini, std::string_view section, const NativeCombatFeatureFlags& flags) {
    ini.set(section, "native_combat", flags.native_combat ? "true" : "false");
    ini.set(section, "native_class_profiles", flags.native_class_profiles ? "true" : "false");
    ini.set(section, "native_movement", flags.native_movement ? "true" : "false");
    ini.set(section, "native_spellcasting", flags.native_spellcasting ? "true" : "false");
}

std::string engine_section_group(std::string_view group_id) {
    return "Engine.Group." + std::string(group_id);
}

std::string engine_section_character(std::string_view character_name) {
    return "Engine.Character." + std::string(character_name);
}

std::string engine_prefix_group(std::string_view group_id) {
    return std::string(kEngineSettingPrefix) + "group." + std::string(group_id) + ".";
}

std::string engine_prefix_character(std::string_view character_name) {
    return std::string(kEngineSettingPrefix) + "character." + std::string(character_name) + ".";
}

std::optional<NativeCombatFeatureFlags> read_engine_flags(const Config& config, std::string_view prefix) {
    const FlagPatch patch = read_flag_patch(config, prefix);
    if (!patch.any()) return std::nullopt;
    NativeCombatFeatureFlags flags;
    apply_flag_patch(flags, patch);
    return flags;
}

std::optional<EngineSelection> resolve_engine_from_settings(const Config& config,
                                                            std::string_view group_id,
                                                            std::string_view character_name,
                                                            bool legacy_requested) {
    const FlagPatch global = read_flag_patch(config, kEngineSettingPrefix);
    const FlagPatch group =
        group_id.empty() ? FlagPatch{} : read_flag_patch(config, engine_prefix_group(group_id));
    const FlagPatch character =
        character_name.empty() ? FlagPatch{} : read_flag_patch(config, engine_prefix_character(character_name));
    if (!global.any() && !group.any() && !character.any()) {
        return std::nullopt;
    }
    // Each layer overrides only the keys it sets, character over group over global.
    NativeCombatFeatureFlags flags;
    apply_flag_patch(flags, global);
    apply_flag_patch(flags, group);
    apply_flag_patch(flags, character);

    EngineSelectionRequest request;
    request.global_flags = flags;
    request.native_requested = true;
    request.legacy_requested = legacy_requested;
    return resolve_engine_selection(request);
}

void ActionIssuerGate::begin_tick(const EngineSelection& selection, std::uint64_t now_ms) {
    selected_ = selection.kind;
    tick_ms_ = now_ms;
    tick_open_ = true;
    issued_ = CombatEngineKind::Disabled;
}

// No flag configured the selection: the client behaves as it did before the
// flags existed. Otherwise the gate is the only authority: it denies (and
// records) a legacy action while the native engine owns the character.
bool legacy_script_action_allowed(const std::optional<EngineSelection>& selection, ActionIssuerGate& gate) {
    if (!selection) return true;
    return gate.allow_legacy();
}

bool legacy_script_name(std::string_view name) noexcept {
    while (!name.empty() && (name.front() == ' ' || name.front() == '/')) name.remove_prefix(1);
    return name == "rgmercs" || name.starts_with("rgmercs/");
}

LegacyScriptState legacy_script_state_from_status(std::string_view status) noexcept {
    if (status.empty()) return LegacyScriptState::Absent;
    if (status == "RUNNING") return LegacyScriptState::Running;
    if (status == "PAUSED") return LegacyScriptState::Paused;
    return LegacyScriptState::Other;
}

LegacyStandDown plan_legacy_stand_down(const std::optional<EngineSelection>& selection,
                                      LegacyScriptState state) noexcept {
    if (!selection || !native_may_issue_actions(*selection)) return LegacyStandDown::None;
    return state == LegacyScriptState::Running ? LegacyStandDown::PauseNow : LegacyStandDown::LeaveRunning;
}

bool legacy_script_may_start(const std::optional<EngineSelection>& selection, std::string_view name) noexcept {
    if (!legacy_script_name(name)) return true;
    if (!selection || !native_may_issue_actions(*selection)) return true;
    return false;
}

void ActionIssuerGate::reset() {
    selected_ = CombatEngineKind::Disabled;
    tick_ms_ = 0;
    tick_open_ = false;
    issued_ = CombatEngineKind::Disabled;
    last_violation_.clear();
    violation_count_ = 0;
}

bool ActionIssuerGate::allow_native() {
    if (selected_ != CombatEngineKind::NativeHailstone) {
        if (!tick_open_) {
            ++violation_count_;
            last_violation_ = "Native action issued before begin_tick";
        }
        return false;
    }
    if (issued_ == CombatEngineKind::LegacyRGMercs) {
        ++violation_count_;
        last_violation_ = "Native action requested in a tick the legacy engine already used";
        return false;
    }
    issued_ = CombatEngineKind::NativeHailstone;
    return true;
}

bool ActionIssuerGate::allow_legacy() {
    if (selected_ != CombatEngineKind::LegacyRGMercs) {
        if (!tick_open_) {
            ++violation_count_;
            last_violation_ = "Legacy action issued before begin_tick";
        } else if (selected_ == CombatEngineKind::NativeHailstone) {
            ++violation_count_;
            last_violation_ = "Legacy action requested while native combat is selected";
        }
        return false;
    }
    if (issued_ == CombatEngineKind::NativeHailstone) {
        ++violation_count_;
        last_violation_ = "Legacy action requested in a tick the native engine already used";
        return false;
    }
    issued_ = CombatEngineKind::LegacyRGMercs;
    return true;
}

} // namespace hailstone::combat
