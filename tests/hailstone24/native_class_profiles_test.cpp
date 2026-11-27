#include <doctest/doctest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "combat/class_profiles.h"
#include "combat/combat_types.h"

using namespace hailstone::combat;

namespace {

bool has_rule_named(const CombatProfile& p, std::string_view name) {
    for (const auto& r : p.rules) {
        if (r.name == name) return true;
    }
    return false;
}

bool profile_mentions_spell(const CombatProfile& p, std::string_view spec_fragment) {
    for (const auto& e : p.loadout) {
        if (e.spell_name.find(spec_fragment) != std::string::npos) return true;
    }
    for (const auto& r : p.rules) {
        if (r.spell_name.find(spec_fragment) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST_CASE("class profiles: the six required classes exist") {
    const std::vector<std::string_view> shorts = native_class_shorts();
    CHECK(shorts.size() == 6);
    for (std::string_view cls : {"WAR", "ROG", "CLR", "MAG", "NEC", "ENC"}) {
        const ClassProfileDef* def = find_class_profile(cls);
        REQUIRE_MESSAGE(def != nullptr, "missing native profile for ", cls);
        CHECK(def->class_short == cls);
    }
    CHECK(find_class_profile("BRD") == nullptr);
    CHECK(find_class_profile("") == nullptr);
}

TEST_CASE("class profiles: lookup is case insensitive and ignores padding") {
    CHECK(find_class_profile("mag") != nullptr);
    CHECK(find_class_profile(" Mag ") != nullptr);
}

TEST_CASE("class profiles: warriors and rogues stick at melee range 3") {
    for (std::string_view cls : {"WAR", "ROG"}) {
        CombatProfile p;
        std::string error;
        REQUIRE(build_native_combat_profile(cls, p, error));
        CHECK(error.empty());
        CHECK(p.class_short == cls);
        CHECK(p.allow_melee_attack);
        CHECK(p.movement_mode == MovementMode::MeleeRange);
        CHECK(p.melee_range == doctest::Approx(3.0f));
    }

    CombatProfile war;
    std::string err;
    REQUIRE(build_native_combat_profile("WAR", war, err));
    CHECK(has_rule_named(war, "Taunt"));
    CHECK(has_rule_named(war, "Bash"));
    CHECK(war.role == "tank");

    CombatProfile rog;
    REQUIRE(build_native_combat_profile("ROG", rog, err));
    CHECK(has_rule_named(rog, "Backstab"));
    CHECK(rog.melee_behind); // rogues want the back arc
}

TEST_CASE("class profiles: cleric heals, cures and rezzes") {
    CombatProfile clr;
    std::string error;
    REQUIRE(build_native_combat_profile("CLR", clr, error));

    CHECK(clr.role == "healer");
    CHECK(clr.class_short == "CLR");
    CHECK_FALSE(clr.allow_melee_attack);

    const ClassProfileDef* def = find_class_profile("CLR");
    REQUIRE(def != nullptr);
    CHECK(def->wants_rez);
    CHECK(def->heal_at_hp_pct > 0);

    bool heal_on_lowest = false;
    bool rez_out_of_combat = false;
    for (const auto& r : clr.rules) {
        if (r.target == TargetKind::GroupLowestHp && r.target_max_hp_pct > 0) heal_on_lowest = true;
        if (r.name == "Resurrect") {
            rez_out_of_combat = r.combat_condition == CombatCondition::OutOfCombatOnly;
        }
    }
    CHECK(heal_on_lowest);
    CHECK(rez_out_of_combat);
    CHECK(profile_mentions_spell(clr, "Healing"));
}

TEST_CASE("class profiles: magician knows Child of Fire and keeps a pet") {
    CombatProfile mag;
    std::string error;
    REQUIRE(build_native_combat_profile("MAG", mag, error));

    CHECK(mag.allow_pet_attack);
    CHECK_FALSE(mag.allow_melee_attack);
    CHECK(mag.movement_mode == MovementMode::RangedRange);

    bool pet_attack_rule = false;
    for (const auto& r : mag.rules) {
        if (r.action_kind == SpellActionKind::PetAttack) pet_attack_rule = true;
    }
    CHECK(pet_attack_rule);

    const ClassProfileDef* def = find_class_profile("MAG");
    REQUIRE(def != nullptr);
    const auto required = std::find(def->required_spells.begin(), def->required_spells.end(), "Child of Fire");
    CHECK_MESSAGE(required != def->required_spells.end(), "Child of Fire must be a required magician spell");

    CHECK_MESSAGE(profile_mentions_spell(mag, "Child of Fire"), "Child of Fire must appear in rules or loadout");
}

TEST_CASE("class profiles: necromancer pets, dots, taps and liches") {
    CombatProfile nec;
    std::string error;
    REQUIRE(build_native_combat_profile("NEC", nec, error));

    CHECK(nec.allow_pet_attack);
    CHECK(has_rule_named(nec, "Dot"));
    CHECK(has_rule_named(nec, "Lifetap"));
    CHECK(has_rule_named(nec, "Lich"));
    CHECK(profile_mentions_spell(nec, "Lich"));
}

TEST_CASE("class profiles: enchanter hastes, grants clarity, leaves mez off") {
    CombatProfile enc;
    std::string error;
    REQUIRE(build_native_combat_profile("ENC", enc, error));

    CHECK(enc.role == "crowd_control");
    bool haste_on_tank = false;
    for (const auto& r : enc.rules) {
        if (r.name == "Haste") {
            haste_on_tank = (r.target == TargetKind::GroupTank) && r.require_buff_missing;
        }
    }
    CHECK(haste_on_tank);

    const CombatSpellRule* mez = nullptr;
    for (const auto& r : enc.rules) {
        if (r.name == "Mez") mez = &r;
    }
    REQUIRE(mez != nullptr);
    CHECK_FALSE(mez->enabled); // no add selection in the snapshot yet
}

TEST_CASE("class profiles: every built profile is internally consistent") {
    for (std::string_view cls : native_class_shorts()) {
        CombatProfile p;
        std::string error;
        REQUIRE(build_native_combat_profile(cls, p, error));

        CHECK(p.enabled);
        CHECK_FALSE(p.dry_run);
        CHECK_FALSE(p.auto_loadout);
        CHECK(p.rememorize_missing);
        CHECK_FALSE(p.rules.empty());
        // A pure melee class (WAR, ROG) has no gems to keep up; a class that casts
        // must name what to keep memorized, or every cast is refused for it.
        const bool casts_spells = std::any_of(p.rules.begin(), p.rules.end(), [](const CombatSpellRule& rule) {
            return rule.action_kind == SpellActionKind::CastSpell;
        });
        // doctest cannot decompose `a && b`; assert the implication directly.
        if (casts_spells) CHECK_FALSE(p.loadout.empty());
        CHECK(p.engage_at_hp_pct > 0);
        CHECK(p.retreat_hp_pct >= 0);
        CHECK(p.retreat_hp_pct < p.engage_at_hp_pct);
        CHECK(p.melee_range > 0.0f);
        CHECK(p.ranged_min_range < p.ranged_max_range);

        std::set<int> gems;
        for (const auto& e : p.loadout) {
            CHECK(e.gem_index >= 0);
            CHECK(e.gem_index < 12);
            CHECK_FALSE(e.spell_name.empty());
            CHECK_MESSAGE(gems.insert(e.gem_index).second, "duplicate gem index in ", cls);
        }
        for (const auto& r : p.rules) {
            CHECK_FALSE(r.name.empty());
            CHECK_FALSE(r.spell_name.empty());
            CHECK(r.priority > 0);
            if (r.min_range > 0.0f) CHECK(r.min_range < r.max_range);
        }
    }
}

TEST_CASE("class profiles: an unsupported class leaves the profile untouched") {
    CombatProfile p;
    p.class_short = "KEEP";
    std::string error;
    CHECK_FALSE(build_native_combat_profile("BRD", p, error));
    CHECK_FALSE(error.empty());
    CHECK(p.class_short == "KEEP");
}
