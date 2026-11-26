#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "combat/combat_types.h"

namespace hailstone::combat {

// Built-in Hailstone-native defaults by EQ 3-letter class code (NEC first, then ROG, CLR, WAR, etc.).
CombatProfile default_combat_profile_for_class(std::string_view class_short);

// Validates that a key name is never a credential/secret field before saving.
bool is_combat_secret_key(std::string_view key);

// Ensures `candidate` resolves inside `root` (lexical containment + symlink-safe canonical check when existing).
bool is_path_within_root(const std::filesystem::path& root, const std::filesystem::path& candidate);

// Loads a standalone Hailstone combat profile INI file.
bool load_combat_profile(const std::filesystem::path& path, CombatProfile& out, std::string& error);

// Saves a Hailstone combat profile INI file (atomic write, zero credentials written).
bool save_combat_profile(const CombatProfile& profile, const std::filesystem::path& path, std::string& error);

// Overlays non-empty / explicitly set fields from `override_path` onto `base`.
bool apply_combat_profile_overrides(CombatProfile& base, const std::filesystem::path& override_path, std::string& error);

// Resolves effective profile: built-in class defaults -> group-level defaults (profiles/<group>.combat.ini)
// -> character-specific combat_profile.ini, confined inside `data_dir`.
bool resolve_character_combat_profile(const std::filesystem::path& data_dir,
                                      const std::filesystem::path& character_dir,
                                      std::string_view group_id,
                                      std::string_view class_short,
                                      CombatProfile& out,
                                      std::string& error);

// Read-only migration helper from existing Hailstone/imported character INI settings inside `data_dir`.
bool migrate_imported_combat_settings(const std::filesystem::path& data_dir,
                                      const std::filesystem::path& legacy_ini_path,
                                      CombatProfile& in_out,
                                      int* imported_fields = nullptr);

MovementMode parse_movement_mode(std::string_view text, MovementMode fallback = MovementMode::RangedRange);
TargetKind parse_target_kind(std::string_view text, TargetKind fallback = TargetKind::HostileTarget);
CombatCondition parse_combat_condition(std::string_view text, CombatCondition fallback = CombatCondition::InCombatOnly);
SpellActionKind parse_spell_action_kind(std::string_view text, SpellActionKind fallback = SpellActionKind::CastSpell);

} // namespace hailstone::combat
