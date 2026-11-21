#pragma once

#include <cstdint>
#include <string_view>

#include "combat/combat_types.h"

namespace hailstone::ipc {
class SessionBusClient;
}
namespace hailstone::nav {
class NavControl;
}
namespace hailstone::net {
class GameConnection;
}
namespace hailstone::game::mechanics {
class Character;
}

namespace hailstone::app {

struct UiHost;

combat::CombatSnapshot build_combat_snapshot(const net::GameConnection& conn,
                                             const nav::NavControl* nav,
                                             const ipc::SessionBusClient* bus,
                                             const game::mechanics::Character* character,
                                             const combat::CombatProfile& profile,
                                             bool player_moving_manually,
                                             bool manual_move_backward,
                                             std::string_view server_name,
                                             std::uint64_t now_ms);

class HailstoneCombatAdapter final : public combat::ICombatActions {
public:
    HailstoneCombatAdapter(net::GameConnection* conn,
                           nav::NavControl* nav,
                           UiHost* ui_host) noexcept
        : conn_(conn), nav_(nav), ui_host_(ui_host) {}

    void set_target(std::uint32_t spawn_id) override;
    void set_auto_attack(bool on) override;
    void pet_attack(std::uint32_t target_id) override;
    void pet_back() override;
    bool cast_gem(int gem_index) override;
    void stop_cast() override;
    bool combat_ability(std::string_view ability_name) override;
    bool memorize_gem(int book_slot, int gem_index) override;
    bool memorize_loadout(const std::vector<std::pair<int, std::uint32_t>>& gem_spells) override;
    void set_stand_state(bool stand) override;
    void move_follow(std::uint32_t target_id, float distance) override;
    void move_to_range(std::uint32_t target_id, float min_range, float max_range) override;
    void move_melee(std::uint32_t target_id, float distance, bool behind) override;
    void move_disengage(const combat::Vec3& target_pos, float disengage_distance) override;
    void stop_movement(std::string_view reason) override;
    void log_message(std::string_view category, std::string_view message) override;

private:
    net::GameConnection* conn_ = nullptr;
    nav::NavControl* nav_ = nullptr;
    UiHost* ui_host_ = nullptr;
};

} // namespace hailstone::app
