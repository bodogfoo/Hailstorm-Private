#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "combat/cast_plan.h"
#include "combat/combat_types.h"

using namespace hailstone::combat;

namespace {

CombatSnapshot make_magician_snapshot() {
    CombatSnapshot s;
    s.now_ms = 10000;
    s.in_zone = true;
    s.level = 70;
    s.mana = 2000;
    s.max_mana = 3000;
    s.character_name = "Sparky";
    s.class_short = "MAG";

    KnownSpellInfo cof;
    cof.book_slot = 12;
    cof.spell_id = 501;
    cof.spell_name = "Child of Fire";
    cof.min_level = 60;
    cof.mana_cost = 250;
    cof.range = 200.0f;
    cof.detrimental = true;
    s.spellbook.push_back(cof);

    SpellGemInfo gem;
    gem.gem_index = 0;
    gem.spell_id = 501;
    gem.spell_name = "Child of Fire";
    gem.usable = true;
    gem.ready = true;
    gem.detrimental = true;
    gem.mana_cost = 250;
    gem.range = 200.0f;
    s.gems.push_back(gem);

    s.me.position = Vec3{0.0f, 0.0f, 0.0f};
    s.has_target = true;
    s.target.id = 42;
    s.target.name = "a fire beetle";
    s.target.is_npc = true;
    s.target.position = Vec3{50.0f, 0.0f, 0.0f};
    return s;
}

} // namespace

TEST_CASE("cast plan: a memorized, affordable, in-range nuke is castable") {
    const CombatSnapshot s = make_magician_snapshot();
    const CastReadiness r = evaluate_cast_readiness(s, "Child of Fire", 1, 0, 0);
    CHECK(r.castable);
    CHECK(r.blocker == CastBlocker::None);
    CHECK(r.gem_index == 0);
    CHECK(r.spell_id == 501);
    CHECK(r.book_slot == 12);
    CHECK(r.reason.empty());
}

TEST_CASE("cast plan: Child of Fire missing from the spellbook is named") {
    CombatSnapshot s = make_magician_snapshot();
    s.spellbook.clear();
    s.gems.clear();

    const CastReadiness r = evaluate_cast_readiness(s, "Child of Fire", 1, 0, 0);
    CHECK_FALSE(r.castable);
    CHECK(r.blocker == CastBlocker::NotInSpellbook);
    CHECK(r.reason.find("Child of Fire") != std::string::npos);
}

TEST_CASE("cast plan: known but not memorized is NotMemorized, not NotInSpellbook") {
    CombatSnapshot s = make_magician_snapshot();
    s.gems.clear();

    const CastReadiness r = evaluate_cast_readiness(s, "Child of Fire", 1, 0, 0);
    CHECK_FALSE(r.castable);
    CHECK(r.blocker == CastBlocker::NotMemorized);
    CHECK(r.reason.find("Child of Fire") != std::string::npos);
}

TEST_CASE("cast plan: memorized but greyed out is GemLocked") {
    CombatSnapshot s = make_magician_snapshot();
    s.gems[0].usable = false;

    const CastReadiness r = evaluate_cast_readiness(s, "Child of Fire", 1, 0, 0);
    CHECK_FALSE(r.castable);
    CHECK(r.blocker == CastBlocker::GemLocked);
}

TEST_CASE("cast plan: mana and cooldown are checked with the numbers in the reason") {
    CombatSnapshot s = make_magician_snapshot();
    s.mana = 100;

    const CastReadiness mana = evaluate_cast_readiness(s, "Child of Fire", 1, 0, 0);
    CHECK_FALSE(mana.castable);
    CHECK(mana.blocker == CastBlocker::NotEnoughMana);
    CHECK(mana.reason.find("250") != std::string::npos);

    s.mana = 2000;
    const CastReadiness cooldown = evaluate_cast_readiness(s, "Child of Fire", 1, 9000, 6000);
    CHECK_FALSE(cooldown.castable);
    CHECK(cooldown.blocker == CastBlocker::CooldownActive);

    // Enough time passed: ready again.
    const CastReadiness ready = evaluate_cast_readiness(s, "Child of Fire", 1, 3000, 6000);
    CHECK(ready.castable);
}

TEST_CASE("cast plan: a detrimental spell needs a target in range") {
    CombatSnapshot s = make_magician_snapshot();
    s.has_target = false;
    CHECK(evaluate_cast_readiness(s, "Child of Fire", 1, 0, 0).blocker == CastBlocker::NoTarget);

    s.has_target = true;
    s.target.position = Vec3{400.0f, 0.0f, 0.0f};
    const CastReadiness far = evaluate_cast_readiness(s, "Child of Fire", 1, 0, 0);
    CHECK(far.blocker == CastBlocker::TargetOutOfRange);
    CHECK(far.distance == doctest::Approx(400.0f));
}

TEST_CASE("cast plan: busy states short-circuit before spell lookup") {
    CombatSnapshot s = make_magician_snapshot();
    s.casting = true;
    CHECK(evaluate_cast_readiness(s, "Child of Fire", 1, 0, 0).blocker == CastBlocker::AlreadyCasting);

    s.casting = false;
    s.memorizing = true;
    CHECK(evaluate_cast_readiness(s, "Child of Fire", 1, 0, 0).blocker == CastBlocker::Memorizing);

    s.memorizing = false;
    s.stunned = true;
    CHECK(evaluate_cast_readiness(s, "Child of Fire", 1, 0, 0).blocker == CastBlocker::Stunned);

    s.stunned = false;
    s.in_zone = false;
    CHECK(evaluate_cast_readiness(s, "Child of Fire", 1, 0, 0).blocker == CastBlocker::NotInZone);
}

TEST_CASE("cast plan: level and missing spell data are distinguished") {
    CombatSnapshot s = make_magician_snapshot();
    s.level = 10;
    CHECK(evaluate_cast_readiness(s, "Child of Fire", 1, 0, 0).blocker == CastBlocker::LevelTooLow);

    s.level = 70;
    s.spellbook[0].spell_data_missing = true;
    CHECK(evaluate_cast_readiness(s, "Child of Fire", 1, 0, 0).blocker == CastBlocker::SpellDataMissing);
}

TEST_CASE("cast plan: a fallback alternative casts when the primary is absent") {
    CombatSnapshot s = make_magician_snapshot();
    s.spellbook[0].spell_name = "Burst of Flame";
    s.gems[0].spell_name = "Burst of Flame";

    const CastReadiness r = evaluate_cast_readiness(s, "Child of Fire|Burst of Flame", 1, 0, 0);
    CHECK(r.castable);
    CHECK(r.spell_id == 501);
}

TEST_CASE("cast plan: required spell status reports book and gem separately") {
    CombatSnapshot s = make_magician_snapshot();
    const auto status = check_required_spells(s, {"Child of Fire"});
    REQUIRE(status.size() == 1);
    CHECK(status[0].in_spellbook);
    CHECK(status[0].memorized);
    CHECK(status[0].gem_index == 0);
    CHECK(status[0].resolved_name == "Child of Fire");
    CHECK(all_required_spells_known(status));

    s.spellbook.clear();
    const auto missing = check_required_spells(s, {"Child of Fire"});
    REQUIRE(missing.size() == 1);
    CHECK_FALSE(missing[0].in_spellbook);
    CHECK_FALSE(all_required_spells_known(missing));
}

TEST_CASE("cast plan: an empty required list is satisfied") {
    const CombatSnapshot s = make_magician_snapshot();
    CHECK(all_required_spells_known(check_required_spells(s, {})));
}
