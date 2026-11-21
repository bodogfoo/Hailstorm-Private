#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "combat/combat_engine.h"
#include "combat/combat_profile.h"
#include "combat/combat_types.h"
#include "combat/movement_controller.h"
#include "combat/spell_manager.h"
#include "scratch_dir.h"

namespace fs = std::filesystem;
using namespace hailstone::combat;

namespace {

struct RecordedActions : public ICombatActions {
    int gameplay_calls = 0;
    std::vector<std::uint32_t> targets_set;
    std::vector<bool> auto_attacks;
    std::vector<std::uint32_t> pet_attacks;
    int pet_backs = 0;
    std::vector<int> casts;
    int stop_casts = 0;
    std::vector<std::string> abilities;
    std::vector<std::pair<int, int>> memorized_single;
    std::vector<std::vector<std::pair<int, std::uint32_t>>> memorized_batch;
    std::vector<bool> stand_states;
    std::vector<std::uint32_t> move_follows;
    std::vector<std::uint32_t> move_ranges;
    std::vector<std::pair<std::uint32_t, bool>> move_melees;
    int move_disengages = 0;
    std::vector<std::string> stop_moves;
    std::vector<std::pair<std::string, std::string>> logs;

    void set_target(std::uint32_t spawn_id) override {
        ++gameplay_calls;
        targets_set.push_back(spawn_id);
    }
    void set_auto_attack(bool on) override {
        ++gameplay_calls;
        auto_attacks.push_back(on);
    }
    void pet_attack(std::uint32_t target_id) override {
        ++gameplay_calls;
        pet_attacks.push_back(target_id);
    }
    void pet_back() override {
        ++gameplay_calls;
        ++pet_backs;
    }
    bool cast_gem(int gem_index) override {
        ++gameplay_calls;
        casts.push_back(gem_index);
        return true;
    }
    void stop_cast() override {
        ++gameplay_calls;
        ++stop_casts;
    }
    bool combat_ability(std::string_view ability_name) override {
        ++gameplay_calls;
        abilities.emplace_back(ability_name);
        return true;
    }
    bool memorize_gem(int book_slot, int gem_index) override {
        ++gameplay_calls;
        memorized_single.emplace_back(book_slot, gem_index);
        return true;
    }
    bool memorize_loadout(const std::vector<std::pair<int, std::uint32_t>>& gem_spells) override {
        ++gameplay_calls;
        memorized_batch.push_back(gem_spells);
        return true;
    }
    void set_stand_state(bool stand) override {
        ++gameplay_calls;
        stand_states.push_back(stand);
    }
    void move_follow(std::uint32_t target_id, float) override {
        ++gameplay_calls;
        move_follows.push_back(target_id);
    }
    void move_to_range(std::uint32_t target_id, float, float) override {
        ++gameplay_calls;
        move_ranges.push_back(target_id);
    }
    void move_melee(std::uint32_t target_id, float, bool behind) override {
        ++gameplay_calls;
        move_melees.emplace_back(target_id, behind);
    }
    void move_disengage(const Vec3&, float) override {
        ++gameplay_calls;
        ++move_disengages;
    }
    void stop_movement(std::string_view reason) override {
        ++gameplay_calls;
        stop_moves.emplace_back(reason);
    }
    void log_message(std::string_view category, std::string_view message) override {
        logs.emplace_back(std::string(category), std::string(message));
    }

    void reset_counts() {
        gameplay_calls = 0;
        targets_set.clear();
        auto_attacks.clear();
        pet_attacks.clear();
        pet_backs = 0;
        casts.clear();
        stop_casts = 0;
        abilities.clear();
        memorized_single.clear();
        memorized_batch.clear();
        stand_states.clear();
        move_follows.clear();
        move_ranges.clear();
        move_melees.clear();
        move_disengages = 0;
        stop_moves.clear();
    }
};

SpawnSnapshot make_mob(std::uint32_t id,
                       const char* name,
                       float x,
                       float y,
                       std::uint8_t hp = 95) {
    SpawnSnapshot mob;
    mob.id = id;
    mob.name = name;
    mob.position = {x, y, 0.0f};
    mob.level = 55;
    mob.hp_pct = hp;
    mob.hp_known = true;
    mob.is_npc = true;
    mob.targetable = true;
    return mob;
}

CombatSnapshot make_nec_snapshot(std::uint64_t now_ms = 1000) {
    CombatSnapshot snap;
    snap.now_ms = now_ms;
    snap.in_zone = true;
    snap.character_name = "Marrowveil";
    snap.class_short = "NEC";
    snap.server_name = "EZ Server";
    snap.zone_short = "hateplaneb";
    snap.level = 65;
    snap.hp_pct = 92;
    snap.mana_pct = 85;
    snap.mana = 2400;
    snap.max_mana = 2800;
    snap.usable_gems = 8;
    snap.me.id = 1;
    snap.me.name = "Marrowveil";
    snap.me.position = {0.0f, 0.0f, 0.0f};
    snap.me.hp_pct = 92;

    snap.pet_id = 50;
    snap.has_pet = true;
    snap.pet_hp_pct = 95;
    SpawnSnapshot pet;
    pet.id = 50;
    pet.name = "Zabek";
    pet.position = {12.0f, 0.0f, 0.0f};
    pet.hp_pct = 95;
    pet.is_npc = true;
    pet.is_pet = true;
    pet.pet_owner_id = 1;
    snap.spawns.push_back(pet);

    const std::vector<std::pair<std::uint32_t, const char*>> nec_spells = {
        {1002, "Splurt"},
        {1006, "Scent of Dusk"},
        {1005, "Engulfing Darkness"},
        {1001, "Drain Soul"},
        {1004, "Envenomed Bolt"},
        {1007, "Mend Bones"},
        {1008, "Lich"},
        {1009, "Feign Death"},
        {1003, "Pyrocruor"},
        {1010, "Cessation of Cor"},
        {1011, "Vexing Mordinia"},
    };
    for (int i = 0; i < static_cast<int>(nec_spells.size()); ++i) {
        KnownSpellInfo k;
        k.book_slot = i;
        k.spell_id = nec_spells[i].first;
        k.spell_name = nec_spells[i].second;
        k.mana_cost = 60;
        k.range = 200.0f;
        snap.spellbook.push_back(k);
    }

    for (int g = 0; g < 8; ++g) {
        SpellGemInfo gem;
        gem.gem_index = g;
        gem.usable = true;
        gem.ready = true;
        gem.mana_cost = 60;
        gem.range = 200.0f;
        gem.spell_id = nec_spells[static_cast<std::size_t>(g)].first;
        gem.spell_name = nec_spells[static_cast<std::size_t>(g)].second;
        snap.gems.push_back(gem);
    }
    return snap;
}

bool has_transition(const CombatStatusSnapshot& st, CombatState from, CombatState to) {
    for (const auto& t : st.transitions) {
        if (t.from_state == from && t.to_state == to && !t.reason.empty()) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("CombatEngine: all 9 states and transition reason logging") {
    CombatProfile profile = default_combat_profile_for_class("NEC");
    profile.assist_target = "Tanker";
    profile.retreat_hp_pct = 20;

    CombatEngine engine;
    engine.set_profile(profile);
    RecordedActions actions;

    // Group tank setup
    SpawnSnapshot tank;
    tank.id = 10;
    tank.name = "Tanker";
    tank.position = {10.0f, 0.0f, 0.0f};
    GroupMemberSnapshot gm;
    gm.name = "Tanker";
    gm.spawn_id = 10;
    gm.main_tank = true;
    gm.in_zone = true;

    // 1. Idle (stopped) -> Start -> Assist (waiting on tank)
    CombatSnapshot snap = make_nec_snapshot(1000);
    snap.group_members.push_back(gm);
    snap.spawns.push_back(tank);
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Idle);
    CHECK_FALSE(engine.status_snapshot().idle_reason.empty());

    engine.start("Begin assist");
    snap.now_ms = 1100;
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Assist);

    // 2. Assist -> Engage (far mob outside ranged_max_range 135)
    SpawnSnapshot far_mob = make_mob(200, "a_spite_golem", 160.0f, 0.0f, 90);
    snap.spawns.push_back(far_mob);
    snap.has_assist_target = true;
    snap.assist_target_id = 200;
    snap.now_ms = 1200;
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Engage);
    CHECK_FALSE(actions.move_ranges.empty());

    // 3. Engage -> Combat (moves inside ranged_max_range)
    snap.spawns.back().position = {80.0f, 0.0f, 0.0f};
    snap.nav_active = true;
    snap.now_ms = 1300;
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Combat);
    CHECK_FALSE(actions.pet_attacks.empty());
    CHECK_FALSE(actions.casts.empty());

    // 4. Combat -> Retreat (HP drops below threshold while casting)
    snap.casting = true;
    snap.hp_pct = 15;
    snap.me.hp_pct = 15;
    snap.now_ms = 1400;
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Retreat);
    CHECK(actions.stop_casts >= 1);
    CHECK(actions.move_disengages >= 1);

    // 5. Retreat -> Disengage -> Idle (recovered / safe)
    snap.casting = false;
    snap.hp_pct = 45;
    snap.me.hp_pct = 45;
    snap.now_ms = 1500;
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Disengage);
    snap.has_assist_target = false;
    snap.assist_target_id = 0;
    snap.now_ms = 1600;
    engine.tick(snap, actions);
    CHECK((engine.state() == CombatState::Idle || engine.state() == CombatState::Assist));

    // 6. Combat -> Dead -> Idle
    snap.hp_pct = 0;
    snap.me.hp_pct = 0;
    snap.me.dead = true;
    snap.now_ms = 1700;
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Dead);

    snap.me.dead = false;
    snap.hp_pct = 100;
    snap.me.hp_pct = 100;
    snap.now_ms = 1800;
    engine.tick(snap, actions);
    CHECK(engine.state() != CombatState::Dead);

    // 7. Pause -> Paused -> Resume
    engine.pause(&actions, "Checking buffs");
    CHECK(engine.state() == CombatState::Paused);
    snap.now_ms = 1900;
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Paused);
    engine.resume("Done checking");
    snap.now_ms = 2000;
    engine.tick(snap, actions);
    CHECK(engine.state() != CombatState::Paused);

    // 8. EmergencyStop -> Reset -> Idle
    engine.emergency_stop(actions, "Abort panic");
    CHECK(engine.state() == CombatState::EmergencyStop);
    CHECK(actions.pet_backs >= 1);
    engine.start("Try bypass while estopped");
    CHECK(engine.state() == CombatState::EmergencyStop);
    CHECK(engine.reset_emergency_stop("Cleared"));
    CHECK(engine.state() == CombatState::Idle);

    const CombatStatusSnapshot st = engine.status_snapshot(&snap);
    CHECK(has_transition(st, CombatState::Idle, CombatState::Assist));
    CHECK(has_transition(st, CombatState::Assist, CombatState::Engage));
    CHECK(has_transition(st, CombatState::Engage, CombatState::Combat));
    CHECK(has_transition(st, CombatState::Combat, CombatState::Retreat));
    CHECK(has_transition(st, CombatState::Retreat, CombatState::Disengage));
    CHECK(has_transition(st, CombatState::Dead, CombatState::Idle));
    CHECK(has_transition(st, CombatState::Paused, CombatState::Idle));
    CHECK(has_transition(st, CombatState::EmergencyStop, CombatState::Idle));
}

TEST_CASE("CombatEngine: target death, disappearance, out-of-range, and Stop safety") {
    CombatProfile profile = default_combat_profile_for_class("ROG");
    profile.assist_target.clear();
    profile.disengage_distance = 120.0f;

    CombatEngine engine;
    engine.set_profile(profile);
    engine.start("Engage test");
    RecordedActions actions;

    CombatSnapshot snap = make_nec_snapshot(1000);
    snap.class_short = "ROG";
    snap.has_pet = false;
    snap.pet_id = 0;
    snap.spawns.clear();
    SpawnSnapshot mob = make_mob(300, "a_skeleton", 8.0f, 0.0f, 80);
    snap.spawns.push_back(mob);
    snap.has_target = true;
    snap.target = mob;
    snap.allowed_to_attack_target = true;

    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Combat);
    REQUIRE_FALSE(actions.auto_attacks.empty());
    CHECK(actions.auto_attacks.back() == true);

    // Target dies -> immediately stop attack and movement, Disengage -> Idle
    actions.reset_counts();
    snap.spawns[0].dead = true;
    snap.spawns[0].hp_pct = 0;
    snap.target = snap.spawns[0];
    snap.auto_attack_on = true;
    snap.nav_active = true;
    snap.now_ms = 1100;
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Disengage);
    CHECK(actions.auto_attacks == std::vector<bool>{false});
    CHECK_FALSE(actions.stop_moves.empty());

    snap.auto_attack_on = false;
    snap.nav_active = false;
    snap.has_target = false;
    snap.spawns.clear();
    snap.now_ms = 1200;
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Idle);

    // Target runs beyond disengage_distance -> disengage safely
    SpawnSnapshot runner = make_mob(301, "a_runner", 10.0f, 0.0f, 50);
    snap.spawns.push_back(runner);
    snap.has_target = true;
    snap.target = runner;
    snap.allowed_to_attack_target = true;
    snap.now_ms = 1300;
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Combat);

    actions.reset_counts();
    snap.spawns[0].position = {145.0f, 0.0f, 0.0f}; // > 120.0f
    snap.target = snap.spawns[0];
    snap.auto_attack_on = true;
    snap.nav_active = true;
    snap.now_ms = 1400;
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Disengage);
    CHECK(actions.auto_attacks == std::vector<bool>{false});
    CHECK_FALSE(actions.stop_moves.empty());

    // Game cleared nav and auto-attack after disengage
    snap.auto_attack_on = false;
    snap.nav_active = false;
    snap.now_ms = 1500;
    engine.tick(snap, actions);
    snap.spawns[0].position = {10.0f, 0.0f, 0.0f};
    snap.target = snap.spawns[0];
    snap.now_ms = 1600;
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Combat);

    actions.reset_counts();
    engine.stop(&actions, "User hit Stop");
    CHECK_FALSE(engine.running());
    CHECK(engine.state() == CombatState::Idle);
    CHECK(actions.auto_attacks == std::vector<bool>{false});
    CHECK_FALSE(actions.stop_moves.empty());

    actions.reset_counts();
    snap.now_ms = 1700;
    engine.tick(snap, actions);
    CHECK(actions.gameplay_calls == 0);
}

TEST_CASE("CombatEngine: manual back-away holds override without snapping back") {
    CombatProfile profile = default_combat_profile_for_class("ROG");
    profile.assist_target.clear();
    profile.pause_on_manual_move = true;
    profile.manual_override_cooldown_ms = 3000;

    CombatEngine engine;
    engine.set_profile(profile);
    engine.start("Combat");
    RecordedActions actions;

    CombatSnapshot snap = make_nec_snapshot(1000);
    snap.class_short = "ROG";
    snap.has_pet = false;
    snap.pet_id = 0;
    snap.spawns.clear();
    SpawnSnapshot mob = make_mob(400, "orc_centurion", 9.0f, 0.0f, 90);
    snap.spawns.push_back(mob);
    snap.has_target = true;
    snap.target = mob;
    snap.allowed_to_attack_target = true;

    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Combat);

    // User backs away manually
    actions.reset_counts();
    snap.manual_move_backward = true;
    snap.player_moving_manually = true;
    snap.auto_attack_on = true;
    snap.nav_active = true;
    snap.now_ms = 2000;
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Paused);
    CHECK(actions.auto_attacks == std::vector<bool>{false});
    CHECK_FALSE(actions.stop_moves.empty());

    // User stops holding back; controller stays paused and does not move back in during cooldown
    actions.reset_counts();
    snap.manual_move_backward = false;
    snap.player_moving_manually = false;
    snap.auto_attack_on = false;
    snap.nav_active = false;
    snap.now_ms = 3500; // within 2000 + 3000
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Paused);
    CHECK(engine.status_snapshot(&snap).manual_override_active);
    CHECK(actions.move_melees.empty());
    CHECK(actions.auto_attacks.empty());

    // Cooldown expires -> returns to Idle then engages cleanly
    snap.now_ms = 5100;
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Idle);

    snap.now_ms = 5200;
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Combat);
}

TEST_CASE("SpellManager & CombatEngine: gem audit, manual gem protection, and safe rememorization") {
    CombatProfile profile = default_combat_profile_for_class("NEC");
    profile.auto_loadout = false;
    profile.rememorize_missing = true;

    CombatSnapshot snap = make_nec_snapshot(1000);
    // Gem 0 (index 0, Splurt) is empty/missing.
    snap.gems[0].spell_id = 0;
    snap.gems[0].spell_name.clear();
    // Gem 6 (index 6, Lich) was manually replaced by player with Vexing Mordinia!
    snap.gems[6].spell_id = 1011;
    snap.gems[6].spell_name = "Vexing Mordinia";

    LoadoutAudit audit = audit_loadout(snap, profile);
    CHECK(audit.any_missing);
    CHECK(audit.any_manual_conflict);
    CHECK(audit.slots[0].status == GemSyncKind::EmptyMissing);
    CHECK(audit.slots[6].status == GemSyncKind::ManuallyChanged);

    // While in combat, rememorize is blocked safely.
    snap.in_combat = true;
    RememorizePlan blocked = plan_rememorize(snap, profile, audit, true);
    CHECK_FALSE(blocked.allowed);

    // Out of combat with auto_loadout=false: rememorizes ONLY empty slot 1, NEVER overwrites slot 6!
    snap.in_combat = false;
    RememorizePlan safe_plan = plan_rememorize(snap, profile, audit, true);
    CHECK(safe_plan.allowed);
    REQUIRE(safe_plan.slots_to_memorize.size() == 1);
    CHECK(safe_plan.slots_to_memorize[0].gem_index == 0);

    // Clear empty slot 0 so only manually changed slot 6 differs -> still refuses overwrite when auto_loadout=false!
    snap.gems[0].spell_id = 1002;
    snap.gems[0].spell_name = "Splurt";
    LoadoutAudit audit_manual_only = audit_loadout(snap, profile);
    CHECK_FALSE(audit_manual_only.any_missing);
    CHECK(audit_manual_only.any_manual_conflict);
    RememorizePlan protect_plan = plan_rememorize(snap, profile, audit_manual_only, true);
    CHECK_FALSE(protect_plan.allowed);
    CHECK(protect_plan.slots_to_memorize.empty());

    // Now enable auto_loadout=true -> allowed to update slot 6 back to Lich!
    profile.auto_loadout = true;
    RememorizePlan auto_plan = plan_rememorize(snap, profile, audit_manual_only, false);
    CHECK(auto_plan.allowed);
    REQUIRE(auto_plan.slots_to_memorize.size() == 1);
    CHECK(auto_plan.slots_to_memorize[0].gem_index == 6);
    CHECK(auto_plan.slots_to_memorize[0].resolved_spell_name == "Lich");
}

TEST_CASE("CombatEngine: EmergencyStop while moving & casting + DryRun simulation mode") {
    CombatProfile profile = default_combat_profile_for_class("NEC");
    profile.dry_run = true;

    CombatEngine engine;
    engine.set_profile(profile);
    engine.start("Dry run start");
    RecordedActions actions;

    CombatSnapshot snap = make_nec_snapshot(1000);
    SpawnSnapshot mob = make_mob(500, "a_dread_guard", 170.0f, 0.0f, 95);
    snap.spawns.push_back(mob);
    snap.has_target = true;
    snap.target = mob;
    snap.allowed_to_attack_target = true;

    // Engage at 170 distance in dry run: zero gameplay calls!
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Engage);
    CHECK(actions.gameplay_calls == 0);
    CHECK_FALSE(actions.logs.empty());

    // Move inside range -> Combat in dry run: zero gameplay calls, Cast recorded in status log
    snap.spawns.back().position = {75.0f, 0.0f, 0.0f};
    snap.target = snap.spawns.back();
    snap.now_ms = 1100;
    engine.tick(snap, actions);
    CHECK(engine.state() == CombatState::Combat);
    CHECK(actions.gameplay_calls == 0);
    CHECK_FALSE(engine.status_snapshot(&snap).last_cast.empty());

    // Live mode EmergencyStop while casting & moving
    engine.set_dry_run(false);
    snap.casting = true;
    snap.nav_active = true;
    snap.auto_attack_on = true;
    actions.reset_counts();
    engine.emergency_stop(actions, "Emergency test");
    CHECK(engine.state() == CombatState::EmergencyStop);
    CHECK(actions.stop_casts == 1);
    CHECK(actions.auto_attacks == std::vector<bool>{false});
    CHECK(actions.pet_backs == 1);
    CHECK_FALSE(actions.stop_moves.empty());
}

TEST_CASE("Class profiles: NEC priority, ROG behind-backstab, CLR group heal, WAR fallback") {
    // 1. NEC: low HP selects Drain Soul lifetap over DoTs; pet hurt selects Mend Bones
    {
        CombatProfile nec = default_combat_profile_for_class("NEC");
        CombatEngine engine;
        engine.set_profile(nec);
        engine.start();
        RecordedActions actions;

        CombatSnapshot snap = make_nec_snapshot(1000);
        snap.hp_pct = 55; // Triggers EmergencyLifetap (priority 100, self_max_hp=65)
        SpawnSnapshot mob = make_mob(600, "a_ghoul", 70.0f, 0.0f, 95);
        snap.spawns.push_back(mob);
        snap.has_target = true;
        snap.target = mob;
        snap.allowed_to_attack_target = true;

        engine.tick(snap, actions);
        REQUIRE(actions.casts.size() == 1);
        CHECK(actions.casts[0] == 3); // Drain Soul (gem 3)
        CHECK(engine.status_snapshot(&snap).last_cast == "Drain Soul");

        // Now self HP high, pet hurt -> Mend Bones (gem 5)
        actions.reset_counts();
        snap.hp_pct = 98;
        snap.pet_hp_pct = 45;
        snap.spawns[0].hp_pct = 45; // pet is spawns[0]
        snap.now_ms = 2000;
        engine.tick(snap, actions);
        REQUIRE(actions.casts.size() == 1);
        CHECK(actions.casts[0] == 5); // Mend Bones (gem 5)
    }

    // 2. ROG: behind melee stick + Backstab combat ability
    {
        CombatProfile rog = default_combat_profile_for_class("ROG");
        rog.assist_target.clear();
        CHECK(rog.movement_mode == MovementMode::MeleeRange);
        CHECK(rog.melee_behind == true);
        CombatEngine engine;
        engine.set_profile(rog);
        engine.start();
        RecordedActions actions;

        CombatSnapshot snap;
        snap.now_ms = 1000;
        snap.in_zone = true;
        snap.character_name = "Stabbinyah";
        snap.class_short = "ROG";
        snap.me.id = 2;
        snap.me.position = {0.0f, 0.0f, 0.0f};
        SpawnSnapshot mob = make_mob(700, "a_gnoll", 8.0f, 0.0f, 95);
        snap.spawns.push_back(mob);
        snap.has_target = true;
        snap.target = mob;
        snap.allowed_to_attack_target = true;

        engine.tick(snap, actions);
        CHECK(engine.state() == CombatState::Combat);
        REQUIRE_FALSE(actions.move_melees.empty());
        CHECK(actions.move_melees[0].second == true); // behind target!
        CHECK(actions.auto_attacks == std::vector<bool>{true});
        REQUIRE_FALSE(actions.abilities.empty());
        CHECK(actions.abilities[0] == "Backstab");
    }

    // 3. CLR: heals injured main tank first
    {
        CombatProfile clr = default_combat_profile_for_class("CLR");
        clr.assist_target.clear();
        CombatEngine engine;
        engine.set_profile(clr);
        engine.start();
        RecordedActions actions;

        CombatSnapshot snap;
        snap.now_ms = 1000;
        snap.in_zone = true;
        snap.character_name = "Mender";
        snap.class_short = "CLR";
        snap.hp_pct = 95;
        snap.mana_pct = 90;
        snap.mana = 2500;
        snap.max_mana = 2700;
        snap.usable_gems = 8;
        snap.me.id = 3;
        snap.me.position = {0.0f, 0.0f, 0.0f};

        SpawnSnapshot tank;
        tank.id = 20;
        tank.name = "Bulwark";
        tank.position = {25.0f, 0.0f, 0.0f};
        tank.hp_pct = 30;
        snap.spawns.push_back(tank);

        GroupMemberSnapshot gm;
        gm.name = "Bulwark";
        gm.spawn_id = 20;
        gm.hp_pct = 30;
        gm.main_tank = true;
        gm.in_zone = true;
        snap.group_members.push_back(gm);

        SpawnSnapshot mob = make_mob(800, "a_dragon", 30.0f, 0.0f, 80);
        snap.spawns.push_back(mob);
        snap.has_target = true;
        snap.target = mob;
        snap.allowed_to_attack_target = true;

        SpellGemInfo rem;
        rem.gem_index = 0;
        rem.spell_id = 2001;
        rem.spell_name = "Remedy";
        rem.usable = true;
        rem.ready = true;
        rem.mana_cost = 150;
        snap.gems.push_back(rem);

        engine.tick(snap, actions);
        REQUIRE(actions.casts.size() == 1);
        CHECK(actions.casts[0] == 0);
        CHECK_FALSE(actions.targets_set.empty());
        CHECK(actions.targets_set.back() == 20); // Targeted injured tank to heal!
    }

    // 4. WAR: melee fallback with Taunt
    {
        CombatProfile war = default_combat_profile_for_class("WAR");
        CombatEngine engine;
        engine.set_profile(war);
        engine.start();
        RecordedActions actions;

        CombatSnapshot snap;
        snap.now_ms = 1000;
        snap.in_zone = true;
        snap.character_name = "Bulwark";
        snap.class_short = "WAR";
        snap.me.id = 4;
        snap.me.position = {0.0f, 0.0f, 0.0f};
        SpawnSnapshot mob = make_mob(900, "an_orc", 9.0f, 0.0f, 95);
        snap.spawns.push_back(mob);
        snap.has_target = true;
        snap.target = mob;
        snap.allowed_to_attack_target = true;

        engine.tick(snap, actions);
        CHECK(engine.state() == CombatState::Combat);
        CHECK(actions.auto_attacks == std::vector<bool>{true});
        REQUIRE_FALSE(actions.abilities.empty());
        CHECK(actions.abilities[0] == "Taunt");
    }
}

TEST_CASE("CombatProfile: INI persistence, secret rejection, confinement, group merge, and read-only migration") {
    hailstone::test::ScratchDir dir("hs24_combat_profile");
    const fs::path data_dir = dir.path() / "data";
    const fs::path profiles_dir = data_dir / "profiles";
    const fs::path char_dir = data_dir / "chars" / "EZ_Server" / "Marrowveil";
    fs::create_directories(profiles_dir);
    fs::create_directories(char_dir);

    // Path confinement check
    CHECK(is_path_within_root(data_dir, char_dir / "combat_profile.ini"));
    CHECK_FALSE(is_path_within_root(data_dir, dir.path() / "outside.ini"));
    CHECK_FALSE(is_path_within_root(data_dir, char_dir / ".." / ".." / ".." / ".." / "escape.ini"));

    // Secret key validation
    CHECK(is_combat_secret_key("password"));
    CHECK(is_combat_secret_key("mq_token"));
    CHECK_FALSE(is_combat_secret_key("retreat_hp_pct"));

    // Save group defaults & character override
    CombatProfile group_def = default_combat_profile_for_class("NEC");
    group_def.assist_target = "Tanker";
    group_def.retreat_hp_pct = 25;
    std::string err;
    REQUIRE(save_combat_profile(group_def, profiles_dir / "ez_group.combat.ini", err));

    CombatProfile char_ovr;
    char_ovr.profile_name = "Marrowveil Solo";
    char_ovr.class_short = "NEC";
    char_ovr.retreat_hp_pct = 15; // override group default
    char_ovr.auto_loadout = false;
    REQUIRE(save_combat_profile(char_ovr, char_dir / "combat_profile.ini", err));

    CombatProfile resolved;
    REQUIRE(resolve_character_combat_profile(data_dir, char_dir, "ez_group", "NEC", resolved, err));
    CHECK(resolved.assist_target == "Tanker");
    CHECK(resolved.retreat_hp_pct == 15);
    CHECK(resolved.auto_loadout == false);
    CHECK_FALSE(resolved.loadout.empty());

    // Read-only migration helper from legacy Hailstone ini inside data_dir
    const fs::path legacy = char_dir / "legacy_char.ini";
    {
        std::ofstream out(legacy);
        out << "[Combat]\n"
            << "assist_target=MainTankBob\n"
            << "melee_range=14\n"
            << "password=MUST_NOT_MIGRATE\n";
    }
    int imported = 0;
    CHECK(migrate_imported_combat_settings(data_dir, legacy, resolved, &imported));
    CHECK(imported >= 2);
    CHECK(resolved.assist_target == "MainTankBob");
    CHECK(resolved.melee_range == doctest::Approx(14.0f));
}


TEST_CASE("CombatEngine: set_follow_target toggles Follow movement mode safely") {
    CombatEngine engine;
    CombatProfile prof = default_combat_profile_for_class("NEC");
    prof.movement_mode = MovementMode::RangedRange;
    engine.set_profile(prof);

    // Clearing follow_target when not in Follow preserves RangedRange.
    engine.set_follow_target("");
    CHECK(engine.profile().movement_mode == MovementMode::RangedRange);

    // Setting a follow_target automatically switches movement_mode to Follow.
    engine.set_follow_target("Tanker");
    CHECK(engine.profile().follow_target == "Tanker");
    CHECK(engine.profile().movement_mode == MovementMode::Follow);

    // Clearing follow_target while in Follow returns movement_mode to None.
    engine.set_follow_target("");
    CHECK(engine.profile().follow_target.empty());
    CHECK(engine.profile().movement_mode == MovementMode::None);
}

TEST_CASE("SpellManager & CombatEngine: level restriction, missing spell data, idle-only auto-restore, and truthful failed memorization") {
    CombatProfile profile = default_combat_profile_for_class("NEC");
    profile.auto_loadout = false;
    profile.rememorize_missing = true;

    CombatSnapshot snap = make_nec_snapshot(1000);
    snap.level = 45;
    snap.in_combat = false;

    // Slot 0: missing gem whose known spellbook entry requires level 60 (> player level 45) -> LevelRestricted!
    snap.gems[0].spell_id = 0;
    snap.gems[0].spell_name.clear();
    snap.spellbook[0].min_level = 60;

    // Slot 1: missing gem whose known spellbook entry has missing spell data -> MissingSpellData!
    snap.gems[1].spell_id = 0;
    snap.gems[1].spell_name.clear();
    snap.spellbook[1].spell_data_missing = true;

    // Slot 2: missing gem that IS valid and memorizable (Engulfing Darkness, book_slot 2, req level 30)
    snap.gems[2].spell_id = 0;
    snap.gems[2].spell_name.clear();
    snap.spellbook[2].min_level = 30;

    // Slot 3: desired spell changed to a non-existent spell -> UnknownInBook
    profile.loadout[3].spell_name = "NonExistentPhantomSpell";
    snap.gems[3].spell_id = 0;
    snap.gems[3].spell_name.clear();

    const LoadoutAudit audit = audit_loadout(snap, profile);
    CHECK(audit.slots[0].status == GemSyncKind::LevelRestricted);
    CHECK(audit.slots[0].required_level == 60);
    CHECK_FALSE(audit.level_restricted_spells.empty());

    CHECK(audit.slots[1].status == GemSyncKind::MissingSpellData);
    CHECK_FALSE(audit.missing_spell_data.empty());

    CHECK(audit.slots[2].status == GemSyncKind::EmptyMissing);
    CHECK(audit.slots[3].status == GemSyncKind::UnknownInBook);
    CHECK_FALSE(audit.unknown_spells.empty());

    // Plan only includes slot 2 — never attempts to memorize LevelRestricted, MissingSpellData, or UnknownInBook.
    const RememorizePlan plan = plan_rememorize(snap, profile, audit, false);
    CHECK(plan.allowed);
    REQUIRE(plan.slots_to_memorize.size() == 1);
    CHECK(plan.slots_to_memorize[0].gem_index == 2);

    // CombatEngine idle auto-restore triggers out-of-combat when running & idle, and reports failed memorization truthfully
    // if the spell fails to land in the gem after the confirmation window!
    CombatEngine engine;
    engine.set_profile(profile);
    engine.start("Idle auto-restore test");
    RecordedActions actions;

    // Tick 1 (idle, out of combat): requests memorization of slot 2.
    snap.now_ms = 1000;
    engine.tick(snap, actions);
    CHECK((!actions.memorized_single.empty() || !actions.memorized_batch.empty()));

    // Server/client never applies the gem change; after 5 seconds engine MUST report failed_memorizations truthfully.
    snap.now_ms = 7000;
    engine.tick(snap, actions);
    const CombatStatusSnapshot st = engine.status_snapshot(&snap);
    CHECK_FALSE(st.loadout_audit.failed_memorizations.empty());
    CHECK_FALSE(st.loadout_audit.level_restricted_spells.empty());
    CHECK_FALSE(st.loadout_audit.missing_spell_data.empty());

    // Once the spell actually lands in the gem, failed_memorizations clears cleanly.
    snap.gems[2].spell_id = 1005;
    snap.gems[2].spell_name = "Engulfing Darkness";
    snap.now_ms = 8000;
    engine.tick(snap, actions);
    CHECK(engine.status_snapshot(&snap).loadout_audit.failed_memorizations.empty());
}

