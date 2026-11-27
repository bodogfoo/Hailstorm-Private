#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::combat {

enum class CombatState : std::uint8_t {
    Idle = 0,
    Assist,
    Engage,
    Combat,
    Disengage,
    Retreat,
    Dead,
    Paused,
    EmergencyStop,
};

inline constexpr std::string_view combat_state_name(CombatState s) noexcept {
    switch (s) {
    case CombatState::Idle: return "Idle";
    case CombatState::Assist: return "Assist";
    case CombatState::Engage: return "Engage";
    case CombatState::Combat: return "Combat";
    case CombatState::Disengage: return "Disengage";
    case CombatState::Retreat: return "Retreat";
    case CombatState::Dead: return "Dead";
    case CombatState::Paused: return "Paused";
    case CombatState::EmergencyStop: return "EmergencyStop";
    }
    return "Idle";
}

enum class MovementMode : std::uint8_t {
    None = 0,
    Follow,
    AssistRange,
    MeleeRange,
    RangedRange,
    DisengageDistance,
};

inline constexpr std::string_view movement_mode_name(MovementMode m) noexcept {
    switch (m) {
    case MovementMode::None: return "None";
    case MovementMode::Follow: return "Follow";
    case MovementMode::AssistRange: return "AssistRange";
    case MovementMode::MeleeRange: return "MeleeRange";
    case MovementMode::RangedRange: return "RangedRange";
    case MovementMode::DisengageDistance: return "DisengageDistance";
    }
    return "None";
}

enum class TargetKind : std::uint8_t {
    HostileTarget = 0,
    Self,
    Pet,
    GroupTank,
    GroupLowestHp,
    GroupMember,
};

inline constexpr std::string_view target_kind_name(TargetKind k) noexcept {
    switch (k) {
    case TargetKind::HostileTarget: return "HostileTarget";
    case TargetKind::Self: return "Self";
    case TargetKind::Pet: return "Pet";
    case TargetKind::GroupTank: return "GroupTank";
    case TargetKind::GroupLowestHp: return "GroupLowestHp";
    case TargetKind::GroupMember: return "GroupMember";
    }
    return "HostileTarget";
}

enum class CombatCondition : std::uint8_t {
    Always = 0,
    InCombatOnly,
    OutOfCombatOnly,
    EngagedTargetOnly,
};

inline constexpr std::string_view combat_condition_name(CombatCondition c) noexcept {
    switch (c) {
    case CombatCondition::Always: return "Always";
    case CombatCondition::InCombatOnly: return "InCombatOnly";
    case CombatCondition::OutOfCombatOnly: return "OutOfCombatOnly";
    case CombatCondition::EngagedTargetOnly: return "EngagedTargetOnly";
    }
    return "Always";
}

enum class SpellActionKind : std::uint8_t {
    CastSpell = 0,
    CombatAbility,
    PetAttack,
    MeleeAttack,
};

inline constexpr std::string_view spell_action_kind_name(SpellActionKind k) noexcept {
    switch (k) {
    case SpellActionKind::CastSpell: return "CastSpell";
    case SpellActionKind::CombatAbility: return "CombatAbility";
    case SpellActionKind::PetAttack: return "PetAttack";
    case SpellActionKind::MeleeAttack: return "MeleeAttack";
    }
    return "CastSpell";
}

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    float distance_2d(const Vec3& o) const noexcept {
        const float dx = x - o.x;
        const float dy = y - o.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    float distance_3d(const Vec3& o) const noexcept {
        const float dx = x - o.x;
        const float dy = y - o.y;
        const float dz = z - o.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
};

struct SpellGemInfo {
    int gem_index = 0; // 0-based (0..11)
    std::uint32_t spell_id = 0;
    std::string spell_name;
    bool usable = false;
    bool ready = false;
    bool detrimental = false;
    std::uint32_t mana_cost = 0;
    float range = 200.0f;
};

struct KnownSpellInfo {
    int book_slot = -1;
    std::uint32_t spell_id = 0;
    std::string spell_name;
    std::uint8_t min_level = 1;
    std::uint32_t mana_cost = 0;
    float range = 200.0f;
    bool detrimental = false;
    bool spell_data_missing = false;
};

struct ActiveBuffInfo {
    std::uint32_t spell_id = 0;
    std::string spell_name;
    int tics_left = 0;
};

struct SpawnSnapshot {
    std::uint32_t id = 0;
    std::string name;
    Vec3 position{};
    float heading = 0.0f; // 0..512 EQ heading
    float melee_radius = 6.0f;
    std::uint8_t level = 0;
    std::uint8_t class_id = 0;
    std::uint8_t hp_pct = 100;
    bool hp_known = false;
    bool is_npc = false;
    bool is_corpse = false;
    bool is_pet = false;
    bool is_mercenary = false;
    bool targetable = true;
    bool dead = false;
    std::uint32_t pet_owner_id = 0;
};

struct GroupMemberSnapshot {
    std::string name;
    std::uint32_t spawn_id = 0;
    std::uint8_t hp_pct = 100;
    std::uint8_t mana_pct = 100;
    bool main_tank = false;
    bool main_assist = false;
    bool puller = false;
    bool is_leader = false;
    bool offline = false;
    bool in_zone = false;
    // Corpse waiting for a resurrection. Set from the spawn's dead flag.
    bool dead = false;
    float distance = 0.0f;
};

struct CombatSnapshot {
    std::uint64_t now_ms = 0;
    std::uint32_t eq_time_ms = 0;
    bool in_zone = false;
    bool in_combat = false;
    bool auto_attack_on = false;
    bool sitting = false;
    // The player's pose is standing (net::kStandStanding). The original refuses
    // to start an attack from any other pose, and a sit/crouch must read back as
    // standing before the attack flag is asked for. Defaults true so a snapshot
    // that says nothing about the pose keeps the old behaviour.
    bool standing = true;
    bool stunned = false;
    bool casting = false;
    std::uint32_t casting_spell_id = 0;
    std::string casting_spell_name;
    bool memorizing = false;
    bool cursor_busy = false;
    bool player_moving_manually = false;
    bool manual_move_backward = false;
    bool nav_active = false;
    std::uint32_t nav_stick_spawn = 0;
    std::uint32_t follow_target_id = 0;
    std::uint8_t mana_pct = 100;
    std::uint32_t mana = 0;
    std::uint32_t max_mana = 0;
    std::uint8_t endurance_pct = 100;
    std::uint32_t endurance = 0;
    std::uint8_t hp_pct = 100;

    std::string character_name;
    std::string server_name;
    std::string zone_short;
    std::string class_short;
    std::uint8_t level = 1;

    SpawnSnapshot me{};
    SpawnSnapshot target{};
    bool has_target = false;
    bool allowed_to_attack_target = false;
    // The original's melee reach between `me` and `target` (game/mechanics/melee.h
    // __get_melee_range over the two spawns' MeleeRadius, model scale and
    // AvatarHeight), filled by the app layer from the live spawns. 0 means this
    // snapshot carries no spawn pair. This is the attack reach; the profile's
    // melee_range is only the distance the melee stick closes to (positioning).
    float melee_reach = 0.0f;
    // Distance between the two bodies: (radius*scale)_me + (radius*scale)_target,
    // out of the same spawn pair. melee_reach above adds 2 units and the height
    // gap, so a tall target reports a reach far beyond its body: the original
    // client turns auto-attack on from that distance, the server then answers
    // "Your target is too far away, get closer!". This is the hard ceiling on
    // both the stick distance and the attack gate. 0 means no spawn pair.
    float melee_contact = 0.0f;

    std::uint32_t assist_target_id = 0;
    std::string assist_target_name;
    bool has_assist_target = false;
    // The two raw candidates before validation, kept for the idle reason so a
    // next session can tell "the peer published nothing" from "the group's main
    // assist named a spawn this zone does not have".
    std::uint32_t assist_peer_target_id = 0;
    std::uint32_t assist_group_target_id = 0;

    std::uint32_t pet_id = 0;
    std::uint8_t pet_hp_pct = 100;
    bool has_pet = false;

    int usable_gems = 8;
    std::vector<SpellGemInfo> gems;
    std::vector<KnownSpellInfo> spellbook;
    std::vector<ActiveBuffInfo> self_buffs;
    std::vector<ActiveBuffInfo> target_buffs;
    std::vector<ActiveBuffInfo> pet_buffs;
    std::vector<GroupMemberSnapshot> group_members;
    std::vector<SpawnSnapshot> spawns;
};

struct CombatSpellRule {
    std::string name;
    std::string spell_name; // Supports "Primary|Fallback1|Fallback2" alternative matching
    int preferred_gem = 0;  // 1..12; 0 = match any memorized gem / combat ability
    SpellActionKind action_kind = SpellActionKind::CastSpell;
    TargetKind target = TargetKind::HostileTarget;
    CombatCondition combat_condition = CombatCondition::InCombatOnly;
    int priority = 50;
    int min_mana_pct = 0;
    int max_mana_pct = 100;
    int target_min_hp_pct = 0;
    int target_max_hp_pct = 100;
    int self_max_hp_pct = 100;
    int self_min_hp_pct = 0;
    float min_range = 0.0f;
    float max_range = 200.0f;
    std::uint64_t cooldown_ms = 0;
    bool require_buff_missing = false;
    bool detrimental = true;
    bool enabled = true;
};

struct DesiredGemEntry {
    int gem_index = 0; // 0-based (0..11)
    std::string spell_name; // Supports "Primary|Fallback1|Fallback2"
    bool required = true;
};

struct CombatProfile {
    std::string profile_name = "Default";
    std::string profile_path; // effective profile file used for reload/save
    std::string class_short = "NEC";
    std::string role;
    std::string mode = "solo";

    bool enabled = true;
    bool dry_run = false;
    // Safety rule: never overwrite manually changed gems unless auto_loadout is enabled.
    bool auto_loadout = false;
    bool rememorize_missing = true;
    bool allow_melee_attack = true;
    bool allow_pet_attack = true;
    bool use_abilities = true;

    std::string assist_target; // e.g., "main_assist", "group_tank", "group_leader", or character name
    int engage_at_hp_pct = 99;
    float max_engage_distance = 180.0f;

    std::string follow_target; // e.g., "", "group_leader", "main_tank", "assist", or character name
    float follow_distance = 15.0f;

    MovementMode movement_mode = MovementMode::RangedRange;
    float assist_range = 90.0f;
    float melee_range = 12.0f;
    float ranged_min_range = 25.0f;
    float ranged_max_range = 135.0f;
    float disengage_distance = 180.0f;
    bool melee_behind = false;
    bool pause_on_manual_move = true;
    std::uint64_t manual_override_cooldown_ms = 3500;

    int retreat_hp_pct = 18;
    float retreat_distance = 45.0f;
    bool stop_cast_on_retreat = true;
    bool stop_attack_on_target_loss = true;

    std::vector<DesiredGemEntry> loadout;
    std::vector<CombatSpellRule> rules;

    std::string source_path;
    std::string group_defaults_path;
};

enum class GemSyncKind : std::uint8_t {
    Match = 0,
    EmptyMissing,
    ManuallyChanged,
    UnknownInBook,
    SlotLocked,
    UnusedSlot,
    LevelRestricted,
    MissingSpellData,
};

inline constexpr std::string_view gem_sync_kind_name(GemSyncKind k) noexcept {
    switch (k) {
    case GemSyncKind::Match: return "Match";
    case GemSyncKind::EmptyMissing: return "Missing";
    case GemSyncKind::ManuallyChanged: return "ManualChange";
    case GemSyncKind::UnknownInBook: return "NotInBook";
    case GemSyncKind::SlotLocked: return "Locked";
    case GemSyncKind::UnusedSlot: return "Unused";
    case GemSyncKind::LevelRestricted: return "LevelRestricted";
    case GemSyncKind::MissingSpellData: return "MissingSpellData";
    }
    return "Unused";
}

struct GemAuditEntry {
    int gem_index = 0; // 0-based
    std::string desired_spec;
    std::string resolved_spell_name;
    std::uint32_t resolved_spell_id = 0;
    int book_slot = -1;
    std::string current_spell_name;
    std::uint32_t current_spell_id = 0;
    std::uint8_t required_level = 0;
    std::string diagnostic;
    GemSyncKind status = GemSyncKind::UnusedSlot;
};

struct LoadoutAudit {
    std::vector<GemAuditEntry> slots;
    std::vector<std::string> missing_spells;
    std::vector<std::string> manually_changed_gems;
    std::vector<std::string> unknown_spells;
    std::vector<std::string> level_restricted_spells;
    std::vector<std::string> missing_spell_data;
    std::vector<std::string> failed_memorizations;
    bool any_missing = false;
    bool any_manual_conflict = false;
};

struct RememorizePlan {
    bool allowed = false;
    std::string blocked_reason;
    bool use_batch_set = false;
    std::vector<GemAuditEntry> slots_to_memorize;
};

struct TransitionLogEntry {
    std::uint64_t timestamp_ms = 0;
    CombatState from_state = CombatState::Idle;
    CombatState to_state = CombatState::Idle;
    std::string reason;
};

struct ActionLogEntry {
    std::uint64_t timestamp_ms = 0;
    bool dry_run = false;
    std::string category;
    std::string detail;
};

struct CombatStatusSnapshot {
    CombatState state = CombatState::Idle;
    MovementMode movement_mode = MovementMode::None;
    bool running = false;
    bool dry_run = false;
    bool auto_loadout = false;
    bool manual_override_active = false;
    std::uint64_t manual_override_remaining_ms = 0;
    bool auto_attack_active = false;

    std::string character_name;
    std::string class_short;
    std::string server_name;
    std::string zone_short;
    std::string profile_path;

    std::uint32_t target_id = 0;
    std::string target_name;
    std::uint8_t target_hp_pct = 0;
    float target_distance = 0.0f;

    std::uint32_t assist_target_id = 0;
    std::string assist_target_name;

    std::string transition_reason;
    std::string idle_reason;
    std::string last_action;
    std::string last_cast;
    std::string last_movement_decision;

    LoadoutAudit loadout_audit{};
    std::vector<TransitionLogEntry> transitions;
    std::vector<ActionLogEntry> actions;
};

class ICombatActions {
public:
    virtual ~ICombatActions() = default;

    virtual void set_target(std::uint32_t spawn_id) = 0;
    virtual void set_auto_attack(bool on) = 0;
    virtual void pet_attack(std::uint32_t target_id) = 0;
    virtual void pet_back() = 0;
    virtual bool cast_gem(int gem_index) = 0; // 0-based
    virtual void stop_cast() = 0;
    virtual bool combat_ability(std::string_view ability_name) = 0;
    virtual bool memorize_gem(int book_slot, int gem_index) = 0;
    virtual bool memorize_loadout(const std::vector<std::pair<int, std::uint32_t>>& gem_spells) = 0;
    virtual void set_stand_state(bool stand) = 0;

    virtual void move_follow(std::uint32_t target_id, float distance) = 0;
    virtual void move_to_range(std::uint32_t target_id, float min_range, float max_range) = 0;
    virtual void move_melee(std::uint32_t target_id, float distance, bool behind) = 0;
    virtual void move_disengage(const Vec3& target_pos, float disengage_distance) = 0;
    virtual void stop_movement(std::string_view reason) = 0;

    virtual void log_message(std::string_view category, std::string_view message) = 0;
};

} // namespace hailstone::combat
