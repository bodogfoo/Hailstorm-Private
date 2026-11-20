#include "app/hailstone_combat_adapter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

#include <glm/glm.hpp>

#include "app/lua_ui.h"
#include "core/log.h"
#include "game/eq_names.h"
#include "game/group.h"
#include "game/mechanics/character.h"
#include "game/spells.h"
#include "game/world_state.h"
#include "ipc/session_bus.h"
#include "nav/nav_control.h"
#include "net/game_connection.h"
#include "net/pet_packets.h"

namespace hailstone::app {
namespace {

bool iequals_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

combat::SpawnSnapshot convert_spawn(const game::Spawn& sp) {
    combat::SpawnSnapshot out;
    out.id = sp.id;
    out.name = game::display_name(sp.name);
    out.position = {sp.position.x, sp.position.y, sp.position.z};
    out.heading = sp.heading;
    out.melee_radius = sp.melee_radius > 0.0f ? sp.melee_radius : 6.0f;
    out.level = sp.level;
    out.class_id = sp.class_id;
    out.hp_pct = sp.hp_percent;
    out.hp_known = sp.hp_known;
    out.is_npc = sp.is_npc;
    out.is_corpse = sp.is_corpse;
    out.is_pet = sp.summoned || sp.temp_pet || sp.pet_owner_id != 0;
    out.is_mercenary = sp.mercenary;
    out.targetable = sp.targetable;
    out.dead = sp.is_corpse || sp.stand_state == 115 || (sp.hp_known && sp.hp_percent == 0);
    out.pet_owner_id = sp.pet_owner_id;
    return out;
}

std::string resolve_assist_member_name(const combat::CombatProfile& profile,
                                       const game::GroupState& grp) {
    if (profile.assist_target.empty()) {
        return {};
    }
    if (iequals_ascii(profile.assist_target, "main_assist") ||
        iequals_ascii(profile.assist_target, "assist")) {
        for (const auto& slot : grp.slots) {
            if (!slot.name.empty() && slot.main_assist) {
                return game::display_name(slot.name);
            }
        }
    } else if (iequals_ascii(profile.assist_target, "group_tank") ||
               iequals_ascii(profile.assist_target, "main_tank") ||
               iequals_ascii(profile.assist_target, "tank")) {
        for (const auto& slot : grp.slots) {
            if (!slot.name.empty() && slot.main_tank) {
                return game::display_name(slot.name);
            }
        }
    } else if (iequals_ascii(profile.assist_target, "group_leader") ||
               iequals_ascii(profile.assist_target, "leader")) {
        if (!grp.leader.empty()) {
            return game::display_name(grp.leader);
        }
    }
    return game::display_name(profile.assist_target);
}

} // namespace

combat::CombatSnapshot build_combat_snapshot(const net::GameConnection& conn,
                                             const nav::NavControl* nav,
                                             const ipc::SessionBusClient* bus,
                                             const game::mechanics::Character* character,
                                             const combat::CombatProfile& profile,
                                             bool player_moving_manually,
                                             bool manual_move_backward,
                                             std::string_view server_name,
                                             std::uint64_t now_ms) {
    combat::CombatSnapshot snap;
    snap.now_ms = now_ms;
    snap.eq_time_ms = net::GameConnection::eq_time();
    snap.in_zone = (conn.state() == net::ConnectionState::InZone);
    snap.server_name = std::string(server_name);
    if (!snap.in_zone) {
        return snap;
    }

    const game::WorldState& world = conn.world();
    snap.zone_short = world.zone_short_name;
    snap.in_combat = conn.in_combat();
    snap.auto_attack_on = conn.auto_attack();
    snap.sitting = conn.sitting();
    snap.stunned = conn.stunned();
    snap.casting = conn.spell_state().casting.casting();
    snap.casting_spell_id = conn.spell_state().casting.spell != game::kNoSpell
                                ? conn.spell_state().casting.spell
                                : 0;
    if (snap.casting_spell_id != 0) {
        if (const game::SpellData* sd = conn.spell_table().get(snap.casting_spell_id)) {
            snap.casting_spell_name = sd->name;
        }
    }
    snap.memorizing = conn.spell_state().memorizing.has_value() || conn.memorizing_set();
    snap.cursor_busy = (conn.inventory().cursor() != nullptr) || (conn.cursor_scroll_spell() != 0);
    snap.player_moving_manually = player_moving_manually;
    snap.manual_move_backward = manual_move_backward;

    if (nav) {
        const nav::NavStatus ns = nav->status();
        snap.nav_active = ns.active || (ns.stick_spawn != 0);
        snap.nav_stick_spawn = ns.stick_spawn;
    }
    snap.follow_target_id = conn.follow_target();

    const net::PlayerStatus& ps = conn.player_status();
    const auto own_hp = conn.own_hp();
    if (own_hp.max > 0) {
        snap.hp_pct = static_cast<std::uint8_t>(
            std::clamp<std::int64_t>((own_hp.cur * 100) / own_hp.max, 0, 100));
    } else if (const game::Spawn* me = world.player()) {
        snap.hp_pct = me->hp_percent;
    }
    snap.mana = ps.mana;
    snap.max_mana = ps.max_mana;
    snap.mana_pct = ps.max_mana > 0
                        ? static_cast<std::uint8_t>(std::clamp<std::uint64_t>(
                              (static_cast<std::uint64_t>(ps.mana) * 100ULL) / ps.max_mana, 0, 100))
                        : 100;
    snap.endurance = ps.endurance;
    snap.endurance_pct = ps.max_endurance > 0
                             ? static_cast<std::uint8_t>(std::clamp<std::uint64_t>(
                                   (static_cast<std::uint64_t>(ps.endurance) * 100ULL) / ps.max_endurance,
                                   0, 100))
                             : 100;

    if (const game::Spawn* me = world.player()) {
        snap.me = convert_spawn(*me);
        snap.me.hp_pct = snap.hp_pct;
        snap.me.dead = conn.player_down();
        snap.character_name = snap.me.name;
        snap.level = me->level;
        snap.class_short = game::class_short_name(me->class_id);
    } else if (character) {
        snap.character_name = game::display_name(conn.character_name());
        snap.level = static_cast<std::uint8_t>(character->sheet().level);
        snap.class_short = game::class_short_name(character->sheet().class_id);
        snap.me.dead = conn.player_down();
    }

    // Spawns list
    snap.spawns.reserve(world.spawns.size());
    for (const auto& [id, sp] : world.spawns) {
        snap.spawns.push_back(convert_spawn(sp));
    }

    // Active target
    if (world.target_id != 0) {
        if (auto it = world.spawns.find(world.target_id); it != world.spawns.end()) {
            snap.has_target = true;
            snap.target = convert_spawn(it->second);
            snap.allowed_to_attack_target = conn.allowed_to_attack(world.target_id, 0);
        }
    }

    // Pet
    snap.pet_id = conn.pets().focused;
    if (snap.pet_id != 0) {
        if (auto it = world.spawns.find(snap.pet_id); it != world.spawns.end()) {
            snap.has_pet = !it->second.is_corpse && it->second.stand_state != 115;
            snap.pet_hp_pct = it->second.hp_percent;
        }
    }

    // Usable gems & memorized gems
    snap.usable_gems = std::clamp(conn.usable_gems(), 0, game::kSpellGems);
    const game::SpellState& spells = conn.spell_state();
    const game::SpellTable& table = conn.spell_table();

    for (int i = 0; i < game::kSpellGems; ++i) {
        combat::SpellGemInfo g;
        g.gem_index = i;
        g.usable = (i < snap.usable_gems);
        const std::uint32_t sid = spells.gems[static_cast<std::size_t>(i)].spell;
        if (sid != game::kNoSpell && sid != 0) {
            g.spell_id = sid;
            g.ready = g.usable && spells.gem_castable(i, snap.eq_time_ms);
            if (const game::SpellData* sd = table.get(sid)) {
                g.spell_name = sd->name;
                g.detrimental = (sd->spell_type == 0);
                g.mana_cost = sd->mana > 0 ? static_cast<std::uint32_t>(sd->mana) : 0;
                g.range = sd->range > 0.0f ? sd->range : 200.0f;
            }
        }
        snap.gems.push_back(std::move(g));
    }

    // Spellbook
    for (int slot = 0; slot < game::kSpellBookSize; ++slot) {
        const std::uint32_t sid = spells.book[static_cast<std::size_t>(slot)];
        if (sid == game::kNoSpell || sid == 0) {
            continue;
        }
        combat::KnownSpellInfo info;
        info.book_slot = slot;
        info.spell_id = sid;
        if (const game::SpellData* sd = table.get(sid)) {
            info.spell_name = sd->name;
            info.mana_cost = sd->mana > 0 ? static_cast<std::uint32_t>(sd->mana) : 0;
            info.range = sd->range > 0.0f ? sd->range : 200.0f;
            info.detrimental = (sd->spell_type == 0);
            if (snap.me.class_id >= 1 && snap.me.class_id <= 16) {
                info.min_level = sd->class_level[snap.me.class_id - 1];
            }
        }
        snap.spellbook.push_back(std::move(info));
    }

    // Self buffs
    for (const auto& b : spells.buffs) {
        if (b.empty()) {
            continue;
        }
        combat::ActiveBuffInfo bi;
        bi.spell_id = b.spell;
        bi.tics_left = b.tics;
        if (const game::SpellData* sd = table.get(b.spell)) {
            bi.spell_name = sd->name;
        }
        snap.self_buffs.push_back(std::move(bi));
    }

    // Target buffs (if current target matches target_buffs_entity)
    if (snap.has_target && spells.target_buffs_entity == snap.target.id) {
        for (const auto& [slot_idx, b] : spells.target_buffs) {
            (void)slot_idx;
            if (b.empty()) {
                continue;
            }
            combat::ActiveBuffInfo bi;
            bi.spell_id = b.spell;
            bi.tics_left = b.tics;
            if (const game::SpellData* sd = table.get(b.spell)) {
                bi.spell_name = sd->name;
            }
            snap.target_buffs.push_back(std::move(bi));
        }
    }

    // Group members
    const game::GroupState& grp = conn.group();
    for (const auto& slot : grp.slots) {
        if (slot.name.empty()) {
            continue;
        }
        combat::GroupMemberSnapshot gm;
        gm.name = game::display_name(slot.name);
        gm.main_tank = slot.main_tank;
        gm.main_assist = slot.main_assist;
        gm.puller = slot.puller;
        gm.offline = slot.offline;
        gm.is_leader = grp.is_leader(slot.name);

        for (const auto& [sid, sp] : world.spawns) {
            if (!sp.is_npc && iequals_ascii(game::display_name(sp.name), gm.name)) {
                gm.spawn_id = sid;
                gm.hp_pct = sp.hp_percent;
                gm.in_zone = true;
                gm.distance = snap.me.position.distance_2d(
                    {sp.position.x, sp.position.y, sp.position.z});
                if (auto mit = grp.mana_percent.find(sid); mit != grp.mana_percent.end()) {
                    gm.mana_pct = mit->second;
                }
                break;
            }
        }
        snap.group_members.push_back(std::move(gm));
    }

    // Assist target resolution: EQ leadership target first, then native Hailstone SessionBus peer target.
    const std::string assist_name = resolve_assist_member_name(profile, grp);
    if (!assist_name.empty()) {
        snap.assist_target_name = assist_name;
        if (conn.leadership().assist_targets[0] != 0) {
            snap.assist_target_id = conn.leadership().assist_targets[0];
            snap.has_assist_target = true;
        } else if (bus) {
            for (const auto& peer : bus->cached_roster()) {
                if (peer.online && peer.target_id != 0 &&
                    iequals_ascii(peer.character_name, assist_name) &&
                    (peer.zone_short.empty() || iequals_ascii(peer.zone_short, snap.zone_short))) {
                    snap.assist_target_id = peer.target_id;
                    snap.has_assist_target = true;
                    break;
                }
            }
        }
    }

    return snap;
}

void HailstoneCombatAdapter::set_target(std::uint32_t spawn_id) {
    if (conn_) {
        conn_->set_target(spawn_id);
    }
}

void HailstoneCombatAdapter::set_auto_attack(bool on) {
    if (conn_) {
        conn_->set_auto_attack(on);
    }
}

void HailstoneCombatAdapter::pet_attack(std::uint32_t target_id) {
    if (!conn_) {
        return;
    }
    if (target_id != 0 && conn_->target() != target_id) {
        conn_->set_target(target_id);
    }
    conn_->pet_command(net::kPetAttack, target_id);
}

void HailstoneCombatAdapter::pet_back() {
    if (conn_) {
        conn_->pet_command(net::kPetBackOff, 0);
    }
}

bool HailstoneCombatAdapter::cast_gem(int gem_index) {
    if (!conn_ || gem_index < 0 || gem_index >= conn_->usable_gems()) {
        return false;
    }
    if (conn_->sitting()) {
        conn_->set_stand_state(net::kStandStanding);
    }
    conn_->cast_gem(gem_index);
    return true;
}

void HailstoneCombatAdapter::stop_cast() {
    if (conn_) {
        conn_->stop_cast();
    }
}

bool HailstoneCombatAdapter::combat_ability(std::string_view ability_name) {
    if (!ui_host_ || !conn_) {
        return false;
    }
    auto res = use_ability_named(*ui_host_, ability_name);
    return res.has_value() && *res;
}

bool HailstoneCombatAdapter::memorize_gem(int book_slot, int gem_index) {
    if (!conn_ || book_slot < 0 || gem_index < 0 || gem_index >= conn_->usable_gems()) {
        return false;
    }
    conn_->memorize(book_slot, gem_index);
    return true;
}

bool HailstoneCombatAdapter::memorize_loadout(
    const std::vector<std::pair<int, std::uint32_t>>& gem_spells) {
    if (!conn_ || gem_spells.empty()) {
        return false;
    }
    std::array<std::int32_t, game::kLoadoutGems> set_arr{};
    set_arr.fill(-1);
    bool any_in_set = false;
    bool all_ok = true;
    for (const auto& [gem, sid] : gem_spells) {
        if (gem >= 0 && gem < game::kLoadoutGems) {
            set_arr[static_cast<std::size_t>(gem)] = static_cast<std::int32_t>(sid);
            any_in_set = true;
        } else if (gem >= game::kLoadoutGems && gem < game::kSpellGems) {
            conn_->memorize_from_cursor(sid, gem);
        }
    }
    if (any_in_set) {
        all_ok = conn_->memorize_set(set_arr);
    }
    return all_ok;
}

void HailstoneCombatAdapter::set_stand_state(bool stand) {
    if (conn_) {
        conn_->set_stand_state(stand ? net::kStandStanding : net::kStandSitting);
    }
}

void HailstoneCombatAdapter::move_follow(std::uint32_t target_id, float distance) {
    if (nav_ && target_id != 0) {
        nav_->go_to_spawn(target_id, std::max(2.0f, distance));
    }
}

void HailstoneCombatAdapter::move_to_range(std::uint32_t target_id, float min_range, float max_range) {
    if (!nav_ || target_id == 0) {
        return;
    }
    const float desired = std::max(min_range + 4.0f, max_range * 0.85f);
    nav_->go_to_spawn(target_id, desired);
}

void HailstoneCombatAdapter::move_melee(std::uint32_t target_id, float distance, bool behind) {
    if (!nav_ || target_id == 0) {
        return;
    }
    nav::StickOptions opt;
    opt.distance = std::max(0.0f, distance);
    opt.behind = behind;
    nav_->stick(target_id, opt);
}

void HailstoneCombatAdapter::move_disengage(const combat::Vec3& target_pos, float disengage_distance) {
    if (!nav_ || !conn_) {
        return;
    }
    const game::Spawn* me = conn_->world().player();
    if (!me) {
        return;
    }
    const float dx = me->position.x - target_pos.x;
    const float dy = me->position.y - target_pos.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    float ux = 1.0f;
    float uy = 0.0f;
    if (len > 0.001f) {
        ux = dx / len;
        uy = dy / len;
    }
    const float dist = std::max(25.0f, disengage_distance + 8.0f);
    const glm::vec3 dest(target_pos.x + ux * dist, target_pos.y + uy * dist, me->position.z);
    nav_->go_to_point(dest, 4.0f, "Combat disengage/retreat", true);
}

void HailstoneCombatAdapter::stop_movement(std::string_view reason) {
    if (nav_) {
        nav_->stop(reason);
    }
}

void HailstoneCombatAdapter::log_message(std::string_view category, std::string_view message) {
    log::info("combat [{}]: {}", category, message);
}

} // namespace hailstone::app
