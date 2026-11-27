#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

#include "app/hailstone_combat_adapter.h"
#include "combat/assist_selection.h"
#include "combat/class_profiles.h"
#include "combat/combat_engine.h"
#include "combat/combat_types.h"
#include "combat/engine_selection.h"
#include "combat/support_policy.h"
#include "core/config.h"
#include "game/mechanics/fpu.h"
#include "game/mechanics/melee.h"
#include "mock_server/harness.h"
#include "net/group_packets.h"

// Integration tests: a class table profile, a real snapshot and the engine's
// own tick, with the actions recorded where the game would receive them. These
// are the paths the client uses, not the pure policy functions on their own.

using namespace hailstone;
using namespace hailstone::combat;

namespace {

struct RecordedActions : public ICombatActions {
    std::vector<std::uint32_t> targets;
    std::vector<int> casts;
    std::vector<std::pair<int, int>> memorized_single;
    std::vector<std::pair<int, std::uint32_t>> memorized_batch;
    std::vector<std::string> messages;

    void set_target(std::uint32_t id) override { targets.push_back(id); }
    void set_auto_attack(bool) override {}
    void pet_attack(std::uint32_t) override {}
    void pet_back() override {}
    bool cast_gem(int gem) override {
        casts.push_back(gem);
        return true;
    }
    void stop_cast() override {}
    bool combat_ability(std::string_view) override { return true; }
    bool memorize_gem(int book_slot, int gem) override {
        memorized_single.emplace_back(book_slot, gem);
        return true;
    }
    bool memorize_loadout(const std::vector<std::pair<int, std::uint32_t>>& batch) override {
        memorized_batch = batch;
        return true;
    }
    void set_stand_state(bool) override {}
    void move_follow(std::uint32_t, float) override {}
    void move_to_range(std::uint32_t, float, float) override {}
    void move_melee(std::uint32_t, float, bool) override {}
    void move_disengage(const Vec3&, float) override {}
    void stop_movement(std::string_view) override {}
    void log_message(std::string_view, std::string_view text) override {
        messages.emplace_back(text);
    }

    bool memorized_anything() const { return !memorized_single.empty() || !memorized_batch.empty(); }
    bool memorized_gem(int gem) const {
        for (const auto& [book, index] : memorized_single) {
            (void)book;
            if (index == gem) return true;
        }
        for (const auto& [index, spell] : memorized_batch) {
            (void)spell;
            if (index == gem) return true;
        }
        return false;
    }
};

// Eight usable, ready gems so readiness is decided by the spell, not the gem.
void fill_gems(CombatSnapshot& snap, const std::vector<std::pair<int, std::string>>& spells) {
    snap.usable_gems = 8;
    snap.gems.clear();
    for (int g = 0; g < 8; ++g) {
        SpellGemInfo gem;
        gem.gem_index = g;
        gem.usable = true;
        gem.ready = true;
        gem.range = 200.0f;
        gem.mana_cost = 40;
        if (g < static_cast<int>(spells.size())) {
            gem.spell_id = static_cast<std::uint32_t>(spells[static_cast<std::size_t>(g)].first);
            gem.spell_name = spells[static_cast<std::size_t>(g)].second;
        }
        snap.gems.push_back(gem);
    }
}

// The spellbook those spells are known from: readiness refuses a spell that is not
// in it, whatever the gems hold.
void fill_known(CombatSnapshot& snap, const std::vector<std::pair<int, std::string>>& spells) {
    snap.spellbook.clear();
    for (const auto& [spell_id, name] : spells) {
        KnownSpellInfo known;
        known.book_slot = static_cast<int>(snap.spellbook.size()) + 1;
        known.spell_id = static_cast<std::uint32_t>(spell_id);
        known.spell_name = name;
        known.min_level = 0;
        known.mana_cost = 40;
        known.range = 200.0f;
        snap.spellbook.push_back(known);
    }
}

// The profile's own loadout, primary alternative only ("Primary|Fallback"). The
// spell ids stand in for the game's, so a gem and its book entry agree.
std::vector<std::pair<int, std::string>> loadout_spells(const CombatProfile& profile) {
    std::vector<std::pair<int, std::string>> spells;
    for (const auto& desired : profile.loadout) {
        spells.emplace_back(desired.gem_index,
                            desired.spell_name.substr(0, desired.spell_name.find('|')));
    }
    return spells;
}

// Gems as the profile asks for them, so nothing is left to memorize and the tick
// reaches the rule or the support pass.
void fill_loadout(CombatSnapshot& snap, const CombatProfile& profile) {
    const auto spells = loadout_spells(profile);
    fill_gems(snap, spells);
    fill_known(snap, spells);
}

int gem_of(const CombatProfile& profile, std::string_view spell) {
    for (const auto& [gem_index, name] : loadout_spells(profile)) {
        if (name.find(spell) != std::string::npos) return gem_index;
    }
    return -1;
}

CombatSnapshot base_snapshot(const char* class_short, std::uint64_t now_ms) {
    CombatSnapshot snap;
    snap.now_ms = now_ms;
    snap.in_zone = true;
    snap.character_name = "Box";
    snap.class_short = class_short;
    snap.server_name = "Fixture";
    snap.zone_short = "qeynos";
    snap.level = 65;
    snap.me.id = 1000;
    snap.me.name = "Box";
    snap.me.hp_pct = 100;
    snap.me.position = {0.0f, 0.0f, 0.0f};
    snap.mana = 5000;
    snap.max_mana = 5000;
    snap.mana_pct = 100;
    return snap;
}

SpawnSnapshot make_mob(std::uint32_t id, const char* name, float x, float hp_pct) {
    SpawnSnapshot mob;
    mob.id = id;
    mob.name = name;
    mob.position = {x, 0.0f, 0.0f};
    mob.hp_pct = static_cast<std::uint8_t>(hp_pct);
    mob.hp_known = true;
    mob.is_npc = true;
    mob.targetable = true;
    mob.level = 50;
    return mob;
}

GroupMemberSnapshot make_member(const char* name, std::uint32_t id, int hp_pct, bool dead) {
    GroupMemberSnapshot member;
    member.name = name;
    member.spawn_id = id;
    member.hp_pct = static_cast<std::uint8_t>(hp_pct);
    member.in_zone = true;
    member.dead = dead;
    member.distance = 10.0f;
    return member;
}

CombatProfile cleric_profile() {
    CombatProfile profile;
    std::string error;
    REQUIRE(build_native_combat_profile("CLR", profile, error));
    CHECK(error.empty());
    return profile;
}

} // namespace

TEST_CASE("native integration: a class profile's own rules drive the support thresholds") {
    const CombatProfile cleric = cleric_profile();
    const SupportSettings settings = support_settings_for_profile(cleric);
    CHECK(settings.heal_at_hp_pct == 78);          // the Cleric table's Heal rule
    CHECK(settings.critical_hp_pct == 55);         // HealCritical
    CHECK(settings.pet_heal_at_hp_pct == 40);      // no PetHeal rule, so the default
    CHECK(settings.heal_spell.find("Superior Healing") != std::string::npos);
    CHECK(settings.rez_spell.find("Resuscitate") != std::string::npos);

    CombatProfile magician;
    std::string error;
    REQUIRE(build_native_combat_profile("MAG", magician, error));
    const SupportSettings mag_settings = support_settings_for_profile(magician);
    CHECK(mag_settings.pet_heal_at_hp_pct == 40);  // the Magician's PetHeal rule
    CHECK(mag_settings.pet_heal_spell.find("Renew Elements") != std::string::npos);
    CHECK(mag_settings.rez_spell.empty());         // a Magician does not rez
}

TEST_CASE("native integration: a Cleric class profile resurrects a dead group member") {
    const CombatProfile profile = cleric_profile();
    CombatEngine engine;
    engine.set_profile(profile);
    engine.start("native integration test");
    RecordedActions actions;

    CombatSnapshot snap = base_snapshot("CLR", 1000);
    fill_loadout(snap, profile);
    snap.group_members.push_back(make_member("Boxclr", 1000, 100, false));
    snap.group_members.push_back(make_member("Boxtank", 4001, 0, true));

    engine.tick(snap, actions);

    CHECK(snap.group_members[1].dead);
    REQUIRE_FALSE(actions.casts.empty());
    // The gem the class table puts Resuscitate in, not a number guessed here.
    const int rez_gem = gem_of(profile, "Resuscitate");
    REQUIRE(rez_gem >= 0);
    CHECK(actions.casts.front() == rez_gem);
    REQUIRE_FALSE(actions.targets.empty());
    CHECK(actions.targets.front() == 4001); // the corpse, not the living member
    CHECK(engine.status_snapshot(&snap).last_cast.find("Resurrect") != std::string::npos);
}

TEST_CASE("native integration: a Cleric class profile heals the lowest wounded member") {
    CombatProfile profile = cleric_profile();
    // The class table ships assist mode ("main_assist"); this fixture has no
    // assist, so it runs solo on the player's own target.
    profile.assist_target.clear();
    CombatEngine engine;
    engine.set_profile(profile);
    engine.start("native integration test");
    RecordedActions actions;

    CombatSnapshot snap = base_snapshot("CLR", 1000);
    fill_loadout(snap, profile);
    snap.in_combat = true;
    snap.group_members.push_back(make_member("Boxclr", 1000, 100, false));
    snap.group_members.push_back(make_member("Boxtank", 4001, 100, false));
    // One wounded member only: two or more would legitimately pick the group heal
    // (support_policy.cpp group_heal_min_members).
    snap.group_members.push_back(make_member("Boxrog", 4002, 30, false));

    engine.tick(snap, actions);

    const CombatStatusSnapshot status = engine.status_snapshot(&snap);
    INFO("idle=" << status.idle_reason << " last_cast=" << status.last_cast << " state="
                 << static_cast<int>(status.state));
    REQUIRE_FALSE(actions.casts.empty());
    const int critical_gem = gem_of(profile, "Complete Heal");
    REQUIRE(critical_gem >= 0);
    CHECK(actions.casts.front() == critical_gem);
    REQUIRE_FALSE(actions.targets.empty());
    CHECK(actions.targets.front() == 4002);
}

TEST_CASE("native integration: a Magician memorizes Child of Fire, then casts it") {
    CombatProfile profile;
    std::string error;
    REQUIRE(build_native_combat_profile("MAG", profile, error));
    // Solo on the player's target, not the class table's assist mode.
    profile.assist_target.clear();
    CombatEngine engine;
    engine.set_profile(profile);
    engine.start("native integration test");
    RecordedActions actions;

    CombatSnapshot snap = base_snapshot("MAG", 1000);
    // The class table's loadout, except gem 1, which the magician has not
    // memorized yet: Child of Fire is the gem the table puts it in.
    fill_loadout(snap, profile);
    snap.gems[0].spell_id = 0;
    snap.gems[0].spell_name.clear();
    REQUIRE(snap.spellbook.size() > 0);

    // Out of combat with the spellbook entry present: the engine memorizes it.
    engine.tick(snap, actions);
    CHECK(actions.memorized_anything());
    CHECK(actions.memorized_gem(0));

    // The server applies the memorization, then a hostile is engaged at range and
    // already hurt: the class table's nuke is for a target at 90% or less.
    snap.gems[0].spell_id = static_cast<std::uint32_t>(100);
    snap.gems[0].spell_name = "Child of Fire";
    snap.gems[0].detrimental = true;
    snap.gems[0].mana_cost = 40;
    snap.in_combat = true;
    snap.has_target = true;
    snap.target = make_mob(5001, "a_skeleton", 60.0f, 80);
    snap.spawns.push_back(snap.target);
    snap.allowed_to_attack_target = true;
    snap.now_ms = 6000;

    engine.tick(snap, actions);
    REQUIRE_FALSE(actions.casts.empty());
    CHECK(actions.casts.back() == 0);
    CHECK(engine.status_snapshot(&snap).last_cast == "Child of Fire");
}

TEST_CASE("native integration: a Lua script's command line follows the ownership gate") {
    // The client asks combat::legacy_script_action_allowed from Automation's script
    // command path (app/automation.cpp run_script), so this covers that seam.
    ActionIssuerGate unset_gate;
    CHECK(legacy_script_action_allowed(std::nullopt, unset_gate)); // no flag: unchanged behaviour
    CHECK(unset_gate.violation_count() == 0);

    hailstone::Config legacy_only;
    legacy_only.set("combat.native.native_combat", "off");
    const auto legacy_selection = resolve_engine_from_settings(legacy_only, "boxgroup", "Box", true);
    REQUIRE(legacy_selection.has_value());
    ActionIssuerGate legacy_gate;
    legacy_gate.begin_tick(*legacy_selection, 1000);
    CHECK(legacy_script_action_allowed(legacy_selection, legacy_gate));
    CHECK(legacy_gate.violation_count() == 0);

    hailstone::Config native_config;
    native_config.set("combat.native.native_combat", "on");
    const auto native_selection = resolve_engine_from_settings(native_config, "boxgroup", "Box", true);
    REQUIRE(native_selection.has_value());
    ActionIssuerGate native_gate;
    native_gate.begin_tick(*native_selection, 2000);
    CHECK_FALSE(legacy_script_action_allowed(native_selection, native_gate));
    CHECK(native_gate.violation_count() == 1);
    CHECK(native_gate.last_violation().find("Legacy") != std::string::npos);
}

TEST_CASE("native integration: the ownership gate keeps the legacy add-on out when native is selected") {
    // Nothing configured: the selection stays unset so the client keeps the
    // behaviour it had before these flags existed.
    hailstone::Config empty;
    CHECK_FALSE(resolve_engine_from_settings(empty, "boxgroup", "Box", true).has_value());

    // Native combat on for one group, and the legacy script asking to act too.
    hailstone::Config config;
    config.set("combat.native.native_combat", "on");
    config.set("combat.native.group.boxgroup.native_class_profiles", "on");
    const auto selection = resolve_engine_from_settings(config, "boxgroup", "Box", true);
    REQUIRE(selection.has_value());
    CHECK(selection->kind == CombatEngineKind::NativeHailstone);
    CHECK(selection->conflict);
    CHECK(selection->effective_flags.native_class_profiles);
    CHECK_FALSE(selection->suppressed_reason.empty());
    CHECK(native_may_issue_actions(*selection));
    CHECK_FALSE(legacy_may_issue_actions(*selection));

    ActionIssuerGate gate;
    gate.begin_tick(*selection, 1000);
    CHECK(gate.allow_native());
    CHECK_FALSE(gate.allow_legacy());
    CHECK(gate.violation_count() == 1);
    CHECK(gate.last_violation().find("Legacy") != std::string::npos);

    // With the master switch off, the legacy engine keeps the character.
    hailstone::Config legacy_only;
    legacy_only.set("combat.native.native_combat", "off");
    const auto legacy_selection = resolve_engine_from_settings(legacy_only, "boxgroup", "Box", true);
    REQUIRE(legacy_selection.has_value());
    CHECK(legacy_selection->kind == CombatEngineKind::LegacyRGMercs);
    CHECK(legacy_may_issue_actions(*legacy_selection));
}

// The wiring the engine depends on: build_combat_snapshot (the path the client
// runs every frame) must fill CombatSnapshot::melee_reach from the live player
// and target spawns, out of __get_melee_range, while the profile's melee_range
// stays the stick distance the movement controller closes to. The engine's own
// unit tests compute that reach with a local helper over melee_range(); this
// case proves the adapter actually reads the WorldState pair, over the mock
// server's real zone-in spawns.
TEST_CASE("native integration: the adapter fills snapshot melee_reach from the live spawn pair") {
    using namespace hailstone::test;

    MockConfig config = MockConfig::standard();
    // Same size and race as the zone's other rat (so the height term is
    // unchanged), but a much larger melee radius: only spawn-derived reach
    // can tell the two targets apart.
    net::Spawn fat = make_spawn("a_fat001", 103, 1, 54, -290.0f, -145.0f, 3.0f, 64.0f);
    fat.melee_radius = 20.0f;
    config.zones[2].spawns.push_back(fat);

    Rig rig(std::move(config));
    REQUIRE(rig.to_zone());
    const game::WorldState& world = rig.conn.world();
    REQUIRE(world.player() != nullptr);
    REQUIRE(world.spawns.contains(101));
    REQUIRE(world.spawns.contains(103));

    auto expected_reach = [&](std::uint32_t target_id) {
        const game::Spawn& me = *world.player();
        const game::Spawn& tgt = world.spawns.at(target_id);
        const game::mechanics::melee::RangeSpawn a{
            me.melee_radius, me.heights.model_scale, me.heights.avatar_height};
        const game::mechanics::melee::RangeSpawn b{
            tgt.melee_radius, tgt.heights.model_scale, tgt.heights.avatar_height};
        return game::mechanics::melee::melee_range(&a, &b, game::mechanics::Fpu{});
    };

    CombatProfile profile;
    profile.melee_range = 3.0f; // the ~3-unit stick distance

    auto snapshot_for = [&](const CombatProfile& p) {
        return hailstone::app::build_combat_snapshot(rig.conn, nullptr, nullptr, nullptr, p,
                                                     false, false, "Mock EQEmu", 1234);
    };

    // No target: nothing to measure, so no reach is invented (the engine falls
    // back to the stick distance rather than attacking at a guess).
    rig.conn.set_target(0);
    const CombatSnapshot none = snapshot_for(profile);
    CHECK_FALSE(none.has_target);
    CHECK(none.melee_reach == doctest::Approx(0.0f));

    // A small target: the reach is the spawns' own, far above the 3-unit stick.
    rig.conn.set_target(101);
    const CombatSnapshot thin = snapshot_for(profile);
    REQUIRE(thin.in_zone);
    REQUIRE(thin.has_target);
    CHECK(thin.melee_reach > 10.0f);
    CHECK(thin.melee_reach == doctest::Approx(expected_reach(101)));
    CHECK(thin.melee_reach != doctest::Approx(profile.melee_range));

    // The stick distance is positioning only: widening it does not move the reach.
    CombatProfile wide = profile;
    wide.melee_range = 10.0f;
    const CombatSnapshot wide_snap = snapshot_for(wide);
    CHECK(wide_snap.melee_reach == doctest::Approx(thin.melee_reach));

    // A bigger target: same snapshot, different spawn pair, larger reach.
    rig.conn.set_target(103);
    const CombatSnapshot fat_snap = snapshot_for(profile);
    REQUIRE(fat_snap.has_target);
    CHECK(fat_snap.melee_reach > thin.melee_reach);
    CHECK(fat_snap.melee_reach == doctest::Approx(expected_reach(103)));
}

// The assist bug the live session hit: the group's main assist
// (OP_SetGroupTarget number 0, GroupAssistNPC[0]) had named a mob this zone did
// not hold, the adapter took that id without checking, and every follower stayed
// in "Assisting" while the peer published a target of its own. The configured
// assist wins; a candidate the local spawn table cannot offer as a live,
// attackable NPC is skipped instead of blocking the other source.
TEST_CASE("native integration: an unusable group main-assist id is skipped, a live one is used") {
    using namespace hailstone::test;

    MockConfig config = MockConfig::standard();
    net::Spawn corpse = make_spawn("a_rat001", 104, 1, 54, -300.0f, -150.0f, 3.0f, 30.0f);
    corpse.npc = 2; // 2/3 is a corpse: present in the zone, not attackable
    config.zones[2].spawns.push_back(corpse);

    Rig rig(std::move(config));
    REQUIRE(rig.to_zone());
    REQUIRE(rig.conn.world().spawns.contains(101));
    REQUIRE(rig.conn.world().spawns.contains(104));
    REQUIRE(rig.conn.world().player() != nullptr);

    CombatProfile profile;
    profile.assist_target = "Yavaha";

    auto snapshot = [&] {
        return hailstone::app::build_combat_snapshot(rig.conn, nullptr, nullptr, nullptr, profile,
                                                     false, false, "Mock EQEmu", 1234);
    };
    auto set_group_assist = [&](std::uint32_t id) {
        rig.server.zone.send_reliable(
            net::encode_mark_npc(net::Op::SetGroupTarget, net::MarkNpc{id, 0, ""}));
        REQUIRE(rig.until([&] { return rig.conn.leadership().assist_targets[0] == id; }));
    };

    // Nothing named yet: no target, and both candidates are reported as zero.
    const CombatSnapshot none = snapshot();
    CHECK(none.assist_target_name == "Yavaha");
    CHECK_FALSE(none.has_assist_target);
    CHECK(none.assist_group_target_id == 0);
    CHECK(none.assist_peer_target_id == 0);

    // The group's main assist names an id this zone does not have: skipped.
    set_group_assist(9999);
    const CombatSnapshot stale = snapshot();
    CHECK(stale.assist_group_target_id == 9999);
    CHECK(stale.assist_target_id == 0);
    CHECK_FALSE(stale.has_assist_target);

    // Present but not assistable: a corpse.
    set_group_assist(104);
    CHECK_FALSE(snapshot().has_assist_target);

    // Never the player himself.
    set_group_assist(rig.conn.world().player()->id);
    CHECK_FALSE(snapshot().has_assist_target);

    // A live mob this zone holds: taken.
    set_group_assist(101);
    const CombatSnapshot live = snapshot();
    CHECK(live.assist_target_id == 101);
    CHECK(live.has_assist_target);
}

// The ordering itself, without a zone: the configured peer's published target
// outranks the group's main-assist target, and an unusable candidate is passed
// over rather than ending the search.
TEST_CASE("native integration: assist candidates are the configured peer, then the group") {
    auto usable = [](std::uint32_t id) { return id == 101 || id == 103; };
    CHECK(select_assist_target(103, 101, usable) == 103); // configured wins
    CHECK(select_assist_target(0, 101, usable) == 101);   // nothing published: the group's
    CHECK(select_assist_target(999, 103, usable) == 103);  // stale peer skipped
    CHECK(select_assist_target(101, 999, usable) == 101);
    CHECK(select_assist_target(999, 998, usable) == 0);    // neither useable

    AssistSpawnFacts facts;
    facts.present = true;
    facts.is_npc = true;
    CHECK(assist_spawn_usable(facts));
    facts.dead = true;
    CHECK_FALSE(assist_spawn_usable(facts));
    facts.dead = false;
    facts.is_corpse = true;
    CHECK_FALSE(assist_spawn_usable(facts));
    facts.is_corpse = false;
    facts.is_pet = true;
    CHECK_FALSE(assist_spawn_usable(facts));
    facts.is_pet = false;
    facts.is_mercenary = true;
    CHECK_FALSE(assist_spawn_usable(facts));
    facts.is_mercenary = false;
    facts.is_npc = false;
    CHECK_FALSE(assist_spawn_usable(facts));
    facts.is_npc = true;
    facts.present = false;
    CHECK_FALSE(assist_spawn_usable(facts));
}
