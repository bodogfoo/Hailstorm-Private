#include "app/group_launch_plan.h"

#include <cctype>
#include <format>

#include "core/group_profile.h"

namespace hailstone::app {
namespace {

bool tag_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

} // namespace

std::string slot_launch_tag(std::string_view roster_session_id, int slot_number) {
    return roster_session_id.empty() ? core::format_slot_id(slot_number)
                                     : std::string(roster_session_id);
}

std::vector<std::string> build_group_launch_args(const GroupLaunchOptions& opts,
                                                 const GroupLaunchSpec& slot) {
    std::vector<std::string> args;
    args.push_back(opts.hailstone_bin.string());
    args.push_back("--data-dir");
    args.push_back(opts.data_dir.string());
    if (!opts.game_dir.empty()) {
        args.push_back("--game-dir");
        args.push_back(opts.game_dir.string());
    }
    if (!opts.profile_path.empty()) {
        args.push_back("--profile");
        args.push_back(opts.profile_path.string());
        args.push_back("--slot");
        args.push_back(std::to_string(slot.slot_number));
    }
    if (!slot.server_id.empty()) {
        args.push_back("--server");
        args.push_back(slot.server_id);
    }
    if (!slot.world.empty()) {
        args.push_back("--world");
        args.push_back(slot.world);
    }
    args.push_back("--account");
    args.push_back(slot.account);
    if (!slot.character.empty()) {
        args.push_back("--character");
        args.push_back(slot.character);
    }
    if (!slot.settings_path.empty()) {
        args.push_back("--settings-path");
        args.push_back(slot.settings_path);
    }
    if (slot.auto_login) args.push_back("--auto-login");
    if (slot.auto_kick) args.push_back("--auto-kick");
    args.push_back("--session-id");
    args.push_back(slot_launch_tag(slot.session_id, slot.slot_number));
    if (!opts.ipc_socket.empty()) {
        args.push_back("--ipc-socket");
        args.push_back(opts.ipc_socket);
    }
    if (slot.bg_tick_hz > 0) {
        args.push_back("--bg-tick-hz");
        args.push_back(std::to_string(slot.bg_tick_hz));
    }
    if (!slot.gpu_mode.empty()) {
        args.push_back("--gpu-mode");
        args.push_back(slot.gpu_mode);
    }
    if (opts.no_audio) args.push_back("--no-audio");
    if (slot.headless) args.push_back("--headless-bg");
    return args;
}

std::string duplicate_online_refusal(const std::vector<ipc::SessionSnapshot>& bus_roster,
                                     const GroupLaunchSpec& slot) {
    const std::string tag = slot_launch_tag(slot.session_id, slot.slot_number);
    for (const auto& peer : bus_roster) {
        if (peer.online && !peer.session_id.empty() && tag_equal(peer.session_id, tag)) {
            return std::format("Slot {} ({}) is already online in the session bus.",
                               slot.slot_number, tag);
        }
    }
    return {};
}

} // namespace hailstone::app
