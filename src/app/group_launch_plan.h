#pragma once

// Launch planning shared by both group launch paths: the in-process launcher
// (ClientApp::launch_group_slot_process) and the session broker's --launch-all.
// One slot identity on both paths, one duplicate-online rule, keyed on the same
// session-bus roster the in-process launcher already consults. Without it the
// broker only knew about children it had spawned itself and would start a second
// copy of a slot another Hailstone window already had online.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "ipc/session_bus.h"

namespace hailstone::app {

// A roster row that names no session id is launched under the shared `slotNN`
// fallback, the same name core::format_slot_id gives it everywhere else.
std::string slot_launch_tag(std::string_view roster_session_id, int slot_number);

struct GroupLaunchSpec {
    int slot_number = 1;
    std::string session_id; // as the roster/profile wrote it; may be empty
    std::string server_id;
    std::string world;
    std::string account;
    std::string character;
    std::string settings_path;
    bool auto_login = true;
    bool auto_kick = true;
    bool headless = true;
    int bg_tick_hz = 0;
    std::string gpu_mode;
};

struct GroupLaunchOptions {
    std::filesystem::path hailstone_bin;
    std::filesystem::path data_dir;
    std::filesystem::path game_dir;
    std::filesystem::path profile_path; // empty -> no --profile/--slot pair
    std::string ipc_socket;
    // Group members are monitored through the controller, so the broker never
    // lets a child open an audio device.
    bool no_audio = false;
};

// argv for one child, args[0] the executable. Carries --session-id <tag> so the
// child's bus identity is the tag the supervisor records.
std::vector<std::string> build_group_launch_args(const GroupLaunchOptions& opts,
                                                 const GroupLaunchSpec& slot);

// Empty when the slot may launch; otherwise the refusal reason, worded as the
// in-process launcher words it. A peer counts only while it is online in the
// bus roster and carries this slot's tag.
std::string duplicate_online_refusal(const std::vector<ipc::SessionSnapshot>& bus_roster,
                                     const GroupLaunchSpec& slot);

} // namespace hailstone::app
