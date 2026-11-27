#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "combat/class_profiles.h"
#include "combat/combat_types.h"
#include "combat/support_policy.h"

using namespace hailstone::combat;

namespace {

CombatSnapshot group_snapshot() {
    CombatSnapshot s;
    s.in_zone = true;
    s.in_combat = true;
    s.character_name = "Mercy";
    s.class_short = "CLR";
    s.level = 70;
    s.hp_pct = 100;
    s.mana_pct = 80;
    s.mana = 2000;
    s.max_mana = 2500;
    s.me.id = 1000;
    s.me.name = s.character_name;
    s.me.position = Vec3{0.0f, 0.0f, 0.0f};

    GroupMemberSnapshot self;
    self.name = "Mercy";
    self.spawn_id = 1000;
    self.hp_pct = 100;
    self.in_zone = true;
    self.is_leader = true;

    GroupMemberSnapshot tank;
    tank.name = "Bulwark";
    tank.spawn_id = 1001;
    tank.hp_pct = 100;
    tank.in_zone = true;
    tank.main_tank = true;

    s.group_members = {self, tank};
    return s;
}

SupportSettings cleric_settings() {
    SupportSettings s;
    s.heal_at_hp_pct = 78;
    s.critical_hp_pct = 55;
    s.heal_spell = "Superior Healing";
    s.critical_heal_spell = "Complete Heal";
    s.group_heal_spell = "Word of Healing";
    s.rez_spell = "Resuscitate";
    s.cure_spell = "Cure Poison";
    return s;
}

} // namespace

TEST_CASE("support: nobody needs a heal at full health") {
    const CombatSnapshot s = group_snapshot();
    CHECK_FALSE(choose_heal(s, cleric_settings()).has_value());
}

TEST_CASE("support: the lowest member gets a normal heal") {
    CombatSnapshot s = group_snapshot();
    s.group_members[1].hp_pct = 70;

    const auto d = choose_heal(s, cleric_settings());
    REQUIRE(d.has_value());
    CHECK(d->kind == SupportActionKind::Heal);
    CHECK(d->spell_spec == "Superior Healing");
    CHECK(d->target_name == "Bulwark");
    CHECK(d->target_spawn_id == 1001);
    CHECK(d->target == TargetKind::GroupMember);
    CHECK(d->target_hp_pct == 70);
}

TEST_CASE("support: a badly hurt member gets the big heal") {
    CombatSnapshot s = group_snapshot();
    s.group_members[1].hp_pct = 30;

    const auto d = choose_heal(s, cleric_settings());
    REQUIRE(d.has_value());
    CHECK(d->kind == SupportActionKind::CriticalHeal);
    CHECK(d->spell_spec == "Complete Heal");
    CHECK(d->priority > 100);
}

TEST_CASE("support: two hurt members prefer the group heal") {
    CombatSnapshot s = group_snapshot();
    s.group_members[0].hp_pct = 60;
    s.group_members[1].hp_pct = 70;
    s.hp_pct = 60;

    const auto d = choose_heal(s, cleric_settings());
    REQUIRE(d.has_value());
    CHECK(d->kind == SupportActionKind::GroupHeal);
    CHECK(d->spell_spec == "Word of Healing");
    CHECK(d->target == TargetKind::Self);
}

TEST_CASE("support: the healer can heal itself, and can be told not to") {
    CombatSnapshot s = group_snapshot();
    s.hp_pct = 60;

    const auto d = choose_heal(s, cleric_settings());
    REQUIRE(d.has_value());
    CHECK(d->target == TargetKind::Self);
    CHECK(d->target_name == "Mercy");

    SupportSettings no_self = cleric_settings();
    no_self.allow_self_heal = false;
    CHECK_FALSE(choose_heal(s, no_self).has_value());
}

TEST_CASE("support: an offline or out-of-zone member is not healed") {
    CombatSnapshot s = group_snapshot();
    s.group_members[1].hp_pct = 20;
    s.group_members[1].offline = true;
    CHECK_FALSE(choose_heal(s, cleric_settings()).has_value());

    s.group_members[1].offline = false;
    s.group_members[1].in_zone = false;
    CHECK_FALSE(choose_heal(s, cleric_settings()).has_value());
}

TEST_CASE("support: a dead member is rezzed with the highest priority") {
    CombatSnapshot s = group_snapshot();
    s.group_members[1].dead = true;
    s.group_members[1].hp_pct = 0;

    const auto rez = choose_rez(s, cleric_settings());
    REQUIRE(rez.has_value());
    CHECK(rez->kind == SupportActionKind::Resurrect);
    CHECK(rez->spell_spec == "Resuscitate");
    CHECK(rez->target_name == "Bulwark");
    CHECK(rez->priority == 150);

    // With both a heal and a rez available, the rez wins.
    s.group_members[0].hp_pct = 50;
    s.hp_pct = 50;
    const auto best = choose_support_action(s, cleric_settings(), {});
    REQUIRE(best.has_value());
    CHECK(best->kind == SupportActionKind::Resurrect);
}

TEST_CASE("support: rez respects mana, the flag and never targets the healer") {
    CombatSnapshot s = group_snapshot();
    s.group_members[1].dead = true;

    SupportSettings low_mana = cleric_settings();
    low_mana.rez_min_mana_pct = 90; // have 80
    CHECK_FALSE(choose_rez(s, low_mana).has_value());

    SupportSettings off = cleric_settings();
    off.allow_rez = false;
    CHECK_FALSE(choose_rez(s, off).has_value());

    CombatSnapshot self_dead = group_snapshot();
    self_dead.group_members[0].dead = true;
    CHECK_FALSE(choose_rez(self_dead, cleric_settings()).has_value());
}

TEST_CASE("support: a hurt pet is healed, a healthy or absent one is not") {
    CombatSnapshot s = group_snapshot();
    SupportSettings settings = cleric_settings();
    settings.pet_heal_spell = "Renew Elements";

    CHECK_FALSE(choose_pet_heal(s, settings).has_value()); // no pet

    s.has_pet = true;
    s.pet_id = 4004;
    s.pet_hp_pct = 90;
    CHECK_FALSE(choose_pet_heal(s, settings).has_value());

    s.pet_hp_pct = 25;
    const auto d = choose_pet_heal(s, settings);
    REQUIRE(d.has_value());
    CHECK(d->kind == SupportActionKind::PetHeal);
    CHECK(d->target == TargetKind::Pet);
    CHECK(d->target_spawn_id == 4004);
}

TEST_CASE("support: a buff is refreshed only when it is missing") {
    CombatSnapshot s = group_snapshot();
    const std::vector<std::string> specs = {"Clarity"};

    const auto d = choose_buff(s, specs);
    REQUIRE(d.has_value());
    CHECK(d->kind == SupportActionKind::Buff);
    CHECK(d->spell_spec == "Clarity");
    CHECK(d->target == TargetKind::Self);

    ActiveBuffInfo buff;
    buff.spell_name = "Clarity II";
    s.self_buffs.push_back(buff);
    CHECK_FALSE(choose_buff(s, specs).has_value());

    // A cast in progress is not interrupted for a buff.
    s.self_buffs.clear();
    s.casting = true;
    CHECK_FALSE(choose_buff(s, specs).has_value());
}

TEST_CASE("support: settings come from the class profile, not a second table") {
    CombatProfile clr;
    std::string error;
    REQUIRE(build_native_combat_profile("CLR", clr, error));
    const SupportSettings s = support_settings_from_profile(clr, 78, 40);
    CHECK(s.heal_spell.find("Healing") != std::string::npos);
    CHECK_FALSE(s.critical_heal_spell.empty());
    CHECK_FALSE(s.group_heal_spell.empty());
    CHECK_FALSE(s.rez_spell.empty());
    CHECK(s.allow_rez);
    CHECK_FALSE(s.cure_spell.empty());

    CombatProfile war;
    REQUIRE(build_native_combat_profile("WAR", war, error));
    const SupportSettings w = support_settings_from_profile(war, 60, 40);
    CHECK(w.heal_spell.empty());
    CHECK_FALSE(w.allow_rez);
    CHECK_FALSE(w.allow_pet_heal);
}
