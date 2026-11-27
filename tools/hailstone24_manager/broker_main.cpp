// Hailstone 24 Session Broker & Child Process Supervisor.
// Runs the local AF_UNIX IPC hub (hailstone::ipc::SessionBusServer), optionally imports MacroQuest
// AutoLogin credentials into Hailstone's isolated data directory/keyring, and supervises up to 24
// independent Hailstone client processes (1 foreground, 23 headless background).

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "app/execution_state.h"
#include "app/group_launch_plan.h"
#include "core/group_profile.h"
#include "core/ini_file.h"
#include "core/mq_login_import.h"
#include "core/wipe.h"
#include "ipc/session_bus.h"
#include "platform/paths.h"

namespace fs = std::filesystem;
using namespace hailstone;

namespace {

std::atomic<bool> g_stop_requested{false};

void handle_signal(int) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

struct RosterSlot {
    int slot_number = 1;
    int launch_order = 1;
    std::string section;
    bool enabled = true;
    std::string session_id;
    std::string server_id;
    std::string world;
    std::string account;
    std::string character;
    std::string class_role;
    std::string settings_path;
    std::string group_leader;
    bool auto_login = true;
    bool headless = true;
    bool auto_kick = true;
    int bg_tick_hz = 0;
    std::string gpu_mode;
};

struct SupervisedChild {
#if !defined(_WIN32)
    pid_t pid = -1;
#endif
    std::string session_id;
    std::string character;
    bool running = false;
};

struct BrokerOptions {
    std::string socket_path;
    fs::path data_dir;
    fs::path game_dir;
    fs::path roster_path;
    std::string profile_arg;
    fs::path hailstone_bin;
    fs::path gpu_broker_bin;
    fs::path import_mq_path;
    std::string mq_master_pass;
    std::string server_override;
    std::string world_override;
    std::string gpu_mode = "cached";
    int bg_tick_hz = 25;
    int stagger_ms = 350;
    int status_interval_s = 15;
    bool launch_all = false;
    bool shared_gpu = false;
    bool exit_when_empty = false;
};

void usage() {
    std::printf(
        "usage: hailstone24_broker [options]\n"
        "  --socket PATH             local AF_UNIX bus socket (default: $XDG_RUNTIME_DIR/hailstone24/bus.sock)\n"
        "  --data-dir DIR            isolated Hailstone data directory for all 24 sessions\n"
        "  --game-dir DIR            read-only EverQuest RoF2 game directory forwarded to clients\n"
        "  --profile NAME_OR_PATH    group profile id/name/file in profiles/ (.ini or .json) to launch\n"
        "  --roster PATH             hailstone24_roster.ini file listing slot01..slot24\n"
        "  --hailstone-bin PATH      path to hailstone client executable (default: sibling hailstone)\n"
        "  --import-mq-logins PATH   import MQ login.db / MQ2AutoLogin.ini into isolated keyring & write roster/profiles\n"
        "  --mq-master-pass PASS     optional MacroQuest login.db master password (also $MQ_MASTER_PASSWORD)\n"
        "  --server ID_OR_NAME       override login server profile id/name for import and launches\n"
        "  --world NAME_OR_KEYWORD   override world server name, id, or unique keyword filter for launches\n"
        "  --launch-all              stagger-launch enabled slots in (launch_order, slot_number) order\n"
        "  --bg-tick-hz N            override background simulation Hz (10..60, default 25)\n"
        "  --gpu-mode MODE           deferred | cached | shared-broker (default: cached)\n"
        "  --shared-gpu              set --gpu-mode shared-broker and start hailstone_gpu_cache_broker if present\n"
        "  --stagger-ms N            delay in ms between launching child clients (default 350)\n"
        "  --status-interval-s N     periodic telemetry summary interval in seconds (0 = quiet, default 15)\n"
        "  --exit-when-empty         exit automatically once launched children all exit\n"
        "  -h, --help                show this help\n");
}

BrokerOptions parse_args(int argc, char** argv) {
    BrokerOptions o;
    if (const char* env_data = std::getenv("HAILSTONE_DATA_DIR"); env_data && *env_data) {
        o.data_dir = env_data;
    }
    if (const char* env_pw = std::getenv("MQ_MASTER_PASSWORD"); env_pw && *env_pw) {
        o.mq_master_pass = env_pw;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto need = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for %s\n", argv[i]);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--socket") o.socket_path = need();
        else if (a == "--data-dir") o.data_dir = need();
        else if (a == "--game-dir") o.game_dir = need();
        else if (a == "--profile") o.profile_arg = need();
        else if (a == "--roster") o.roster_path = need();
        else if (a == "--hailstone-bin") o.hailstone_bin = need();
        else if (a == "--import-mq-logins") o.import_mq_path = need();
        else if (a == "--mq-master-pass") o.mq_master_pass = need();
        else if (a == "--server") o.server_override = need();
        else if (a == "--world") o.world_override = need();
        else if (a == "--gpu-mode") o.gpu_mode = need();
        else if (a == "--bg-tick-hz") o.bg_tick_hz = std::clamp(std::atoi(need()), 10, 60);
        else if (a == "--stagger-ms") o.stagger_ms = std::clamp(std::atoi(need()), 0, 10000);
        else if (a == "--status-interval-s") o.status_interval_s = std::max(0, std::atoi(need()));
        else if (a == "--launch-all") o.launch_all = true;
        else if (a == "--shared-gpu") {
            o.shared_gpu = true;
            o.gpu_mode = "shared-broker";
        } else if (a == "--exit-when-empty") {
            o.exit_when_empty = true;
        } else if (a == "--help" || a == "-h") {
            usage();
            std::exit(0);
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            std::exit(2);
        }
    }
    return o;
}

std::vector<RosterSlot> load_roster(const BrokerOptions& opts, fs::path& used_profile_path,
                                    std::string& load_error) {
    std::vector<RosterSlot> out;
    fs::path source_path;
    bool using_group_profile = false;

    if (!opts.profile_arg.empty()) {
        auto found = core::find_group_profile_file(platform::profiles_dir(), opts.profile_arg);
        if (!found) {
            load_error = std::format("Group profile '{}' not found in {}", opts.profile_arg,
                                     platform::profiles_dir().string());
            return out;
        }
        source_path = *found;
        using_group_profile = true;
    } else {
        source_path = opts.roster_path;
    }

    core::GroupProfile gp;
    if (!core::load_group_profile(source_path, gp, &load_error)) {
        return out;
    }
    if (using_group_profile) used_profile_path = gp.source_path;

    for (const auto& s : gp.slots) {
        if (!s.enabled || s.account.empty()) continue;
        RosterSlot slot;
        slot.slot_number = s.slot_number;
        slot.launch_order = s.launch_order;
        slot.section = std::format("slot{:02d}", s.slot_number);
        slot.enabled = s.enabled;
        slot.session_id = s.session_id;
        slot.server_id = !opts.server_override.empty()
                             ? opts.server_override
                             : (!s.server_id.empty() ? s.server_id : gp.default_server_id);
        slot.world = !opts.world_override.empty()
                         ? opts.world_override
                         : (!s.world.empty() ? s.world : gp.default_world);
        slot.account = s.account;
        slot.character = s.character;
        slot.class_role = s.class_role;
        slot.auto_login = s.auto_login;
        slot.headless = s.headless;
        slot.auto_kick = s.auto_kick;
        slot.bg_tick_hz = s.bg_tick_hz > 0 ? s.bg_tick_hz : opts.bg_tick_hz;
        slot.gpu_mode = s.gpu_mode.empty() ? opts.gpu_mode : s.gpu_mode;
        slot.group_leader = s.group_leader;
        if (auto eff = core::resolve_effective_settings_path(platform::data_dir(), gp, s)) {
            slot.settings_path = eff->string();
        }
        out.push_back(std::move(slot));
        if (out.size() >= 24) break;
    }
    return out;
}

#if !defined(_WIN32)
pid_t spawn_process(const std::vector<std::string>& args) {
    if (args.empty()) return -1;
    std::vector<char*> c_args;
    c_args.reserve(args.size() + 1);
    for (const auto& s : args) c_args.push_back(const_cast<char*>(s.c_str()));
    c_args.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        ::execvp(c_args[0], c_args.data());
        std::fprintf(stderr, "execvp(%s) failed: %s\n", c_args[0], std::strerror(errno));
        ::_exit(127);
    }
    return pid;
}

void terminate_children(std::vector<SupervisedChild>& children, pid_t& gpu_pid) {
    for (const auto& c : children) {
        if (c.running && c.pid > 0) ::kill(c.pid, SIGTERM);
    }
    if (gpu_pid > 0) ::kill(gpu_pid, SIGTERM);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        bool any_alive = false;
        for (auto& c : children) {
            if (!c.running || c.pid <= 0) continue;
            int status = 0;
            const pid_t r = ::waitpid(c.pid, &status, WNOHANG);
            if (r == c.pid) {
                c.running = false;
            } else {
                any_alive = true;
            }
        }
        if (gpu_pid > 0) {
            int status = 0;
            if (::waitpid(gpu_pid, &status, WNOHANG) == gpu_pid) {
                gpu_pid = -1;
            } else {
                any_alive = true;
            }
        }
        if (!any_alive) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    for (auto& c : children) {
        if (c.running && c.pid > 0) {
            ::kill(c.pid, SIGKILL);
            int status = 0;
            ::waitpid(c.pid, &status, 0);
            c.running = false;
        }
    }
    if (gpu_pid > 0) {
        ::kill(gpu_pid, SIGKILL);
        int status = 0;
        ::waitpid(gpu_pid, &status, 0);
        gpu_pid = -1;
    }
}
#endif

} // namespace

int main(int argc, char** argv) {
    BrokerOptions opts = parse_args(argc, argv);

    if (!opts.data_dir.empty()) {
        std::error_code ec;
        fs::create_directories(opts.data_dir, ec);
        platform::use_data_dir(opts.data_dir, "--data-dir");
    }
    if (opts.socket_path.empty()) opts.socket_path = ipc::default_bus_socket_path();
    if (opts.roster_path.empty()) opts.roster_path = platform::data_dir() / "hailstone24_roster.ini";

    if (opts.hailstone_bin.empty()) {
        fs::path self_dir = platform::install_dir();
        fs::path candidate = self_dir / "hailstone";
        if (fs::exists(candidate)) opts.hailstone_bin = candidate;
        else opts.hailstone_bin = "hailstone";
    }
    if (opts.gpu_broker_bin.empty()) {
        fs::path candidate = platform::install_dir() / "hailstone_gpu_cache_broker";
        if (fs::exists(candidate)) opts.gpu_broker_bin = candidate;
    }

    if (!opts.import_mq_path.empty()) {
        core::MqImportOptions imp;
        imp.source_path = opts.import_mq_path;
        imp.master_password = opts.mq_master_pass;
        imp.target_server_id = opts.server_override;
        imp.servers_dir = platform::servers_dir();
        imp.roster_out = opts.roster_path;
        imp.profiles_dir = platform::profiles_dir();
        imp.write_group_profiles = true;
        const auto res = core::import_mq_logins(imp);
        core::wipe(imp.master_password);
        core::wipe(opts.mq_master_pass);
        if (!res.ok) {
            std::fprintf(stderr, "[broker] MQ login import failed (%s): %s\n",
                         res.source_kind.c_str(), res.error.c_str());
            return 1;
        }
        std::printf("[broker] Imported %zu/%zu MQ logins (%zu keyring passwords, %zu characters, %zu profiles) -> %s\n",
                    res.accounts_imported, res.accounts_found, res.passwords_saved, res.characters_linked,
                    res.group_profiles_written.size(), opts.roster_path.string().c_str());
        for (const auto& w : res.warnings) std::printf("[broker] warning: %s\n", w.c_str());
    }
    core::wipe(opts.mq_master_pass);

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
#if !defined(_WIN32)
    std::signal(SIGPIPE, SIG_IGN);
#endif

    ipc::SessionBusServer bus;
    if (!bus.start(opts.socket_path)) {
        std::fprintf(stderr, "[broker] Failed to bind session IPC socket: %s\n", opts.socket_path.c_str());
        return 1;
    }
    std::printf("[broker] Listening on %s (data-dir=%s)\n",
                opts.socket_path.c_str(), platform::data_dir().string().c_str());
    std::fflush(stdout);

    std::vector<SupervisedChild> children;
#if !defined(_WIN32)
    pid_t gpu_broker_pid = -1;
    if (opts.shared_gpu && !opts.gpu_broker_bin.empty() && fs::exists(opts.gpu_broker_bin)) {
        gpu_broker_pid = spawn_process({opts.gpu_broker_bin.string(), "--serve"});
        if (gpu_broker_pid > 0) {
            std::printf("[broker] Started hailstone_gpu_cache_broker (pid=%d)\n", static_cast<int>(gpu_broker_pid));
        }
    }

    if (opts.launch_all) {
        fs::path profile_path;
        std::string load_error;
        const auto slots = load_roster(opts, profile_path, load_error);
        if (slots.empty()) {
            std::fprintf(stderr, "[broker] --launch-all set but no enabled slots loaded (%s)\n",
                         load_error.empty() ? opts.roster_path.string().c_str() : load_error.c_str());
        }
        // The bus is the shared authority on which slots are already online: the
        // in-process launcher (client_app_login.cpp) refuses a slot whose tag is
        // online there, and the broker must apply the same rule instead of only
        // checking the children it spawned itself.
        auto settle_bus = [&bus] {
            // A peer that connected during the last stagger window may not have
            // been accepted, or its Hello read, when poll_once returned. Wait a
            // short beat for the connection, then drain the Hello that follows.
            for (int i = 0; i < 4; ++i) {
                if (!bus.poll_once(i == 0 ? 50 : 0)) break;
            }
        };
        app::GroupLaunchOptions launch_opts;
        launch_opts.hailstone_bin = opts.hailstone_bin;
        launch_opts.data_dir = platform::data_dir();
        launch_opts.game_dir = opts.game_dir;
        launch_opts.profile_path = profile_path;
        launch_opts.ipc_socket = opts.socket_path;
        launch_opts.no_audio = true;

        settle_bus();
        for (std::size_t idx = 0; idx < slots.size() && !g_stop_requested.load(std::memory_order_relaxed); ++idx) {
            const auto& slot = slots[idx];
            // The in-process launcher launches a slot under its `slotNN` id when
            // the roster row names no session id; app::slot_launch_tag is that same
            // fallback, and the child, the supervisor and the guard all use it.
            app::GroupLaunchSpec spec;
            spec.slot_number = slot.slot_number;
            spec.session_id = slot.session_id;
            spec.server_id = slot.server_id;
            spec.world = slot.world;
            spec.account = slot.account;
            spec.character = slot.character;
            spec.settings_path = slot.settings_path;
            spec.auto_login = slot.auto_login;
            spec.auto_kick = slot.auto_kick;
            spec.headless = slot.headless;
            spec.bg_tick_hz = slot.bg_tick_hz;
            spec.gpu_mode = slot.gpu_mode;
            const std::string tag = app::slot_launch_tag(spec.session_id, spec.slot_number);

            settle_bus();
            std::string refusal = app::duplicate_online_refusal(bus.roster(), spec);
            if (refusal.empty()) {
                for (const auto& c : children) {
                    if (c.running && c.session_id == tag) {
                        refusal = std::format("Slot {} ({}) is already running.", slot.slot_number, tag);
                        break;
                    }
                }
            }
            if (!refusal.empty()) {
                std::printf("[broker] Skipped order=%d slot=%d (%s): %s\n", slot.launch_order,
                            slot.slot_number, tag.c_str(), refusal.c_str());
                std::fflush(stdout);
                continue;
            }

            const auto args = app::build_group_launch_args(launch_opts, spec);

            const pid_t cpid = spawn_process(args);
            if (cpid > 0) {
                children.push_back(SupervisedChild{cpid, tag, slot.character, true});
                std::printf("[broker] Launched order=%d slot=%d (%s) char=%s mode=%s (pid=%d)\n",
                            slot.launch_order, slot.slot_number, tag.c_str(),
                            slot.character.empty() ? "<select>" : slot.character.c_str(),
                            slot.headless ? "BACKGROUND_HEADLESS" : "FOREGROUND_FULL",
                            static_cast<int>(cpid));
            } else {
                std::fprintf(stderr, "[broker] Failed to spawn %s\n", tag.c_str());
            }
            if (idx + 1 < slots.size() && opts.stagger_ms > 0) {
                bus.poll_once(opts.stagger_ms);
            }
        }
        std::fflush(stdout);
    }
#endif

    auto last_summary = std::chrono::steady_clock::now();
    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        bus.poll_once(20);
        (void)bus.take_acks();

#if !defined(_WIN32)
        int active_children = 0;
        for (auto& c : children) {
            if (!c.running || c.pid <= 0) continue;
            int status = 0;
            const pid_t r = ::waitpid(c.pid, &status, WNOHANG);
            if (r == c.pid) {
                c.running = false;
                const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                std::printf("[broker] Child %s (%s) exited (status=%d)\n",
                            c.session_id.c_str(), c.character.c_str(), code);
                std::fflush(stdout);
            } else {
                ++active_children;
            }
        }
        if (opts.exit_when_empty && !children.empty() && active_children == 0) {
            std::printf("[broker] All launched sessions exited; stopping.\n");
            break;
        }
#endif

        if (opts.status_interval_s > 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_summary >= std::chrono::seconds(opts.status_interval_s)) {
                last_summary = now;
                const auto roster = bus.roster();
                std::uint64_t total_rss_mib = 0;
                std::uint64_t total_vram_mib = 0;
                std::string fg = "none";
                for (const auto& s : roster) {
                    total_rss_mib += s.rss_mib;
                    total_vram_mib += s.vram_mib;
                    if (s.exec_state == app::ExecutionState::ForegroundFull) {
                        fg = s.character_name.empty() ? s.session_id : s.character_name;
                    }
                }
                std::printf("[broker] Connected=%zu/24 Foreground=%s RSS=%llu MiB EstVRAM=%llu MiB\n",
                            roster.size(), fg.c_str(),
                            static_cast<unsigned long long>(total_rss_mib),
                            static_cast<unsigned long long>(total_vram_mib));
                std::fflush(stdout);
            }
        }
    }

#if !defined(_WIN32)
    terminate_children(children, gpu_broker_pid);
#endif
    bus.stop();
    std::printf("[broker] Shutdown complete.\n");
    return 0;
}
