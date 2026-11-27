#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "combat/engine_selection.h"

using namespace hailstone::combat;

namespace {

NativeCombatFeatureFlags all_flags() {
    NativeCombatFeatureFlags f;
    f.native_combat = true;
    f.native_class_profiles = true;
    f.native_movement = true;
    f.native_spellcasting = true;
    return f;
}

} // namespace

TEST_CASE("engine selection: flags default to off so legacy keeps working") {
    NativeCombatFeatureFlags f;
    CHECK_FALSE(f.native_combat);
    CHECK_FALSE(f.native_class_profiles);
    CHECK_FALSE(f.native_movement);
    CHECK_FALSE(f.native_spellcasting);
    CHECK_FALSE(f.any_enabled());
    CHECK_FALSE(f.all_enabled());
}

TEST_CASE("engine selection: nothing enabled and nothing requesting is Disabled") {
    EngineSelectionRequest req;
    req.native_requested = false;
    req.legacy_requested = false;

    const EngineSelection sel = resolve_engine_selection(req);
    CHECK(sel.kind == CombatEngineKind::Disabled);
    CHECK_FALSE(native_may_issue_actions(sel));
    CHECK_FALSE(legacy_may_issue_actions(sel));
    CHECK(sel.indicator == "Disabled");
}

TEST_CASE("engine selection: legacy runs when the native master switch is off") {
    EngineSelectionRequest req;
    req.legacy_requested = true;
    CHECK_FALSE(req.global_flags.native_combat);

    const EngineSelection sel = resolve_engine_selection(req);
    CHECK(sel.kind == CombatEngineKind::LegacyRGMercs);
    CHECK(legacy_may_issue_actions(sel));
    CHECK_FALSE(native_may_issue_actions(sel));
    CHECK_FALSE(sel.conflict);
    CHECK(sel.indicator == "Legacy RGMercs");
}

TEST_CASE("engine selection: native master switch suppresses legacy in the same tick") {
    EngineSelectionRequest req;
    req.global_flags = all_flags();
    req.legacy_requested = true;

    const EngineSelection sel = resolve_engine_selection(req);
    CHECK(sel.kind == CombatEngineKind::NativeHailstone);
    CHECK(native_may_issue_actions(sel));
    CHECK_FALSE(legacy_may_issue_actions(sel));
    CHECK(sel.conflict);
    CHECK_FALSE(sel.suppressed_reason.empty());
    CHECK(sel.indicator == "Native Hailstone");
}

TEST_CASE("engine selection: subordinate flags alone cannot start native combat") {
    EngineSelectionRequest req;
    req.global_flags.native_combat = false;
    req.global_flags.native_movement = true;
    req.global_flags.native_spellcasting = true;
    req.legacy_requested = true;

    const NativeCombatFeatureFlags effective = effective_native_flags(req);
    CHECK_FALSE(effective.native_combat);
    CHECK_FALSE(effective.native_movement);
    CHECK_FALSE(effective.native_spellcasting);

    const EngineSelection sel = resolve_engine_selection(req);
    CHECK(sel.kind == CombatEngineKind::LegacyRGMercs);
    REQUIRE_FALSE(sel.suppressed_reason.empty());
}

TEST_CASE("engine selection: character layer overrides group layer overrides global") {
    EngineSelectionRequest req;
    req.global_flags = all_flags();

    NativeCombatFeatureFlags group;
    group.native_combat = true;

    NativeCombatFeatureFlags character;
    character.native_combat = false;

    req.group_flags = group;
    CHECK(effective_native_flags(req).native_class_profiles == false);
    CHECK(effective_native_flags(req).native_combat == true);

    req.character_flags = character;
    CHECK(effective_native_flags(req).native_combat == false);
}

TEST_CASE("engine selection: INI round trip keeps unset layers unset") {
    hailstone::IniFile ini;
    CHECK_FALSE(read_engine_flags(ini, "Engine").has_value());

    NativeCombatFeatureFlags flags;
    flags.native_combat = true;
    flags.native_spellcasting = true;
    write_engine_flags(ini, "Engine", flags);

    const auto read_back = read_engine_flags(ini, "Engine");
    REQUIRE(read_back.has_value());
    CHECK(read_back->native_combat);
    CHECK_FALSE(read_back->native_class_profiles);
    CHECK_FALSE(read_back->native_movement);
    CHECK(read_back->native_spellcasting);

    CHECK(engine_section_group("g1") == "Engine.Group.g1");
    CHECK(engine_section_character("Aegis") == "Engine.Character.Aegis");
}

TEST_CASE("action issuer gate: only the selected engine may act") {
    EngineSelectionRequest req;
    req.global_flags = all_flags();
    const EngineSelection sel = resolve_engine_selection(req);

    ActionIssuerGate gate;
    gate.begin_tick(sel, 1000);
    CHECK(gate.allow_native());
    CHECK_FALSE(gate.allow_legacy());
    CHECK(gate.violation_count() == 1);
    CHECK_FALSE(gate.last_violation().empty());
}

TEST_CASE("action issuer gate: exactly one engine per tick") {
    EngineSelectionRequest legacy_req;
    legacy_req.legacy_requested = true;
    const EngineSelection legacy_sel = resolve_engine_selection(legacy_req);

    ActionIssuerGate gate;
    gate.begin_tick(legacy_sel, 0);
    CHECK(gate.allow_legacy());
    CHECK_FALSE(gate.allow_native());

    // Next tick belongs to native: the gate reopens.
    EngineSelectionRequest native_req;
    native_req.global_flags = all_flags();
    gate.begin_tick(resolve_engine_selection(native_req), 100);
    CHECK(gate.allow_native());
    CHECK_FALSE(gate.allow_legacy());
}

TEST_CASE("action issuer gate: a disabled selection lets nobody act") {
    EngineSelectionRequest req;
    req.native_requested = false;
    req.legacy_requested = false;
    const EngineSelection sel = resolve_engine_selection(req);

    ActionIssuerGate gate;
    gate.begin_tick(sel, 0);
    CHECK_FALSE(gate.allow_native());
    CHECK_FALSE(gate.allow_legacy());
}

TEST_CASE("engine selection: nothing configured in the settings file keeps the old behaviour") {
    hailstone::Config config;
    CHECK_FALSE(resolve_engine_from_settings(config, "boxgroup", "Boxwar", false).has_value());

    // A key for another area of the client does not count as configuring this one.
    config.set("combat.enabled", "1");
    CHECK_FALSE(resolve_engine_from_settings(config, "boxgroup", "Boxwar", false).has_value());
}

TEST_CASE("engine selection: the settings file layers character over group over global") {
    // Config::parse() replaces the whole set, so every stage spells out the layers
    // it means to have; adding one key must not drop the ones already read.
    static constexpr std::string_view kGlobal =
        "combat.native.native_combat=1\ncombat.native.native_spellcasting=1\n";
    static constexpr std::string_view kGroup = "combat.native.group.boxgroup.native_combat=0\n";
    static constexpr std::string_view kCharacter = "combat.native.character.Boxwar.native_combat=on\n";

    hailstone::Config config;
    config.parse(kGlobal);
    auto selection = resolve_engine_from_settings(config, "boxgroup", "Boxwar", false);
    REQUIRE(selection.has_value());
    CHECK(selection->kind == CombatEngineKind::NativeHailstone);
    CHECK(selection->effective_flags.native_combat);
    CHECK(selection->effective_flags.native_spellcasting);

    // The group switches the native set off even though the global layer is on.
    config.parse(std::string(kGlobal) + std::string(kGroup));
    selection = resolve_engine_from_settings(config, "boxgroup", "Boxwar", false);
    REQUIRE(selection.has_value());
    CHECK(selection->kind == CombatEngineKind::Disabled);
    CHECK_FALSE(selection->effective_flags.native_spellcasting); // the master switch gates the rest

    // The character switches it back on for itself only; the group's off still
    // applies to everyone else in it.
    config.parse(std::string(kGlobal) + std::string(kGroup) + std::string(kCharacter));
    const auto char_selection = resolve_engine_from_settings(config, "boxgroup", "Boxwar", false);
    REQUIRE(char_selection.has_value());
    CHECK(char_selection->kind == CombatEngineKind::NativeHailstone);
    const auto other = resolve_engine_from_settings(config, "boxgroup", "Boxclr", false);
    REQUIRE(other.has_value());
    CHECK(other->kind == CombatEngineKind::Disabled);
}

TEST_CASE("engine selection: legacy and native asking together resolve to one owner and say why") {
    hailstone::Config config;
    config.parse("combat.native.native_combat=1\n");
    const auto selection = resolve_engine_from_settings(config, "boxgroup", "Boxwar", true);
    REQUIRE(selection.has_value());
    CHECK(selection->kind == CombatEngineKind::NativeHailstone);
    CHECK(selection->conflict);
    CHECK(selection->suppressed_reason.find("Legacy") != std::string::npos);
    CHECK_FALSE(legacy_may_issue_actions(*selection));
    CHECK(native_may_issue_actions(*selection));
}

TEST_CASE("engine selection: a subsystem flag without the master switch does nothing") {
    hailstone::Config config;
    config.parse("combat.native.native_movement=1\n");
    const auto selection = resolve_engine_from_settings(config, "", "", false);
    REQUIRE(selection.has_value());
    CHECK(selection->kind == CombatEngineKind::Disabled);
    CHECK_FALSE(selection->effective_flags.any_enabled());
}

TEST_CASE("engine selection: the stand-down is re-planned every tick, and legacy may not start there") {
    // The live failure this covers: a one-shot pause left RGMercs free to restart
    // itself (reload, error repair, the zone-in autoload) and the dashboard then
    // showed rgmercs:RUNNING beside a native row. The plan is asked every tick,
    // and a start is refused before the script loads at all.
    NativeCombatFeatureFlags flags;
    flags.native_combat = true;

    EngineSelection native;
    native.kind = CombatEngineKind::NativeHailstone;
    native.effective_flags = flags;
    const std::optional<EngineSelection> native_sel = native;

    // Running: pause it now. Paused: it is already stood down. Anything else
    // (starting, stopping, an error) is not ours to pause mid-flight.
    CHECK(plan_legacy_stand_down(native_sel, LegacyScriptState::Running) == LegacyStandDown::PauseNow);
    CHECK(plan_legacy_stand_down(native_sel, LegacyScriptState::Paused) == LegacyStandDown::LeaveRunning);
    CHECK(plan_legacy_stand_down(native_sel, LegacyScriptState::Other) == LegacyStandDown::LeaveRunning);
    CHECK(plan_legacy_stand_down(native_sel, LegacyScriptState::Absent) == LegacyStandDown::LeaveRunning);

    // The status the dashboard shows maps to exactly those states, and anything
    // unexpected is Other rather than misread as running.
    CHECK(legacy_script_state_from_status("RUNNING") == LegacyScriptState::Running);
    CHECK(legacy_script_state_from_status("PAUSED") == LegacyScriptState::Paused);
    CHECK(legacy_script_state_from_status("FAILED") == LegacyScriptState::Other);
    CHECK(legacy_script_state_from_status("") == LegacyScriptState::Absent);

    // Every RGMercs script is the legacy add-on, including its child scripts, so
    // heartbeat cannot run a command line the gate would drop anyway.
    CHECK(legacy_script_name("rgmercs"));
    CHECK(legacy_script_name("/rgmercs/heartbeat"));
    CHECK_FALSE(legacy_script_name("my_buff_script"));

    // A start is refused while native owns the character; other scripts and an
    // unconfigured install are untouched.
    CHECK_FALSE(legacy_script_may_start(native_sel, "rgmercs"));
    CHECK_FALSE(legacy_script_may_start(native_sel, "rgmercs/heartbeat"));
    CHECK(legacy_script_may_start(native_sel, "my_buff_script"));
    CHECK(legacy_script_may_start(std::nullopt, "rgmercs"));

    // With RGMercs selected instead, it keeps its stand-down-free behaviour.
    EngineSelection legacy;
    legacy.kind = CombatEngineKind::LegacyRGMercs;
    legacy.effective_flags = flags;
    const std::optional<EngineSelection> legacy_sel = legacy;
    CHECK(plan_legacy_stand_down(legacy_sel, LegacyScriptState::Running) == LegacyStandDown::None);
    CHECK(legacy_script_may_start(legacy_sel, "rgmercs"));
}
