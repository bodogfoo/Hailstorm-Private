// The broker's --launch-all duplicate-online guard and slot identity, tested on the
// shared launch-plan seam and against the real broker process with a stub child.

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app/group_launch_plan.h"
#include "ipc/session_bus.h"
#include "scratch_dir.h"

#if !defined(_WIN32)
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace hailstone;
namespace fs = std::filesystem;

TEST_CASE("Group launch plan: an unnamed slot is launched as its shared slotNN id") {
    app::GroupLaunchOptions opts;
    opts.hailstone_bin = "/bin/true";
    opts.data_dir = "/tmp/hailstone-test-data";
    opts.ipc_socket = "/tmp/hailstone-test-bus.sock";
    opts.no_audio = true;

    app::GroupLaunchSpec slot;
    slot.slot_number = 7; // session_id left empty, as a roster row with no id
    const auto args = app::build_group_launch_args(opts, slot);
    const auto it = std::find(args.begin(), args.end(), "--session-id");
    REQUIRE(it != args.end());
    REQUIRE(std::next(it) != args.end());
    CHECK(*std::next(it) == "slot07");
    CHECK(app::slot_launch_tag(slot.session_id, slot.slot_number) == "slot07");
    CHECK(std::find(args.begin(), args.end(), "slot07") != args.end());

    // A named slot keeps its own id on both the argument and the supervisor tag.
    slot.session_id = "raid-mt";
    const auto named = app::build_group_launch_args(opts, slot);
    CHECK(app::slot_launch_tag(slot.session_id, slot.slot_number) == "raid-mt");
    CHECK(std::find(named.begin(), named.end(), "raid-mt") != named.end());
    CHECK(std::find(named.begin(), named.end(), "slot07") == named.end());

    // The fallback clamps to the same 1..24 range core::format_slot_id does.
    CHECK(app::slot_launch_tag("", 0) == "slot01");
    CHECK(app::slot_launch_tag("", 99) == "slot24");
}

TEST_CASE("Group launch plan: only an online peer holding the slot tag refuses a launch") {
    app::GroupLaunchSpec slot;
    slot.slot_number = 3;

    ipc::SessionSnapshot online;
    online.session_id = "slot03";
    online.online = true;
    ipc::SessionSnapshot offline = online;
    offline.online = false;
    ipc::SessionSnapshot other;
    other.session_id = "slot04";
    other.online = true;

    CHECK(app::duplicate_online_refusal({}, slot).empty());
    CHECK(app::duplicate_online_refusal({offline}, slot).empty());
    CHECK(app::duplicate_online_refusal({other}, slot).empty());

    const auto refusal = app::duplicate_online_refusal({online}, slot);
    CHECK_FALSE(refusal.empty());
    CHECK(refusal.find("slot03") != std::string::npos);
    CHECK(refusal.find("Slot 3") != std::string::npos);

    // A custom roster id is what a peer must match, not the slotNN fallback.
    slot.session_id = "raid-mt";
    ipc::SessionSnapshot custom;
    custom.session_id = "raid-mt";
    custom.online = true;
    CHECK_FALSE(app::duplicate_online_refusal({custom}, slot).empty());
    CHECK(app::duplicate_online_refusal({online}, slot).empty());
}

#if !defined(_WIN32) && defined(HAILSTONE_BROKER_BIN)
namespace {

// sockaddr_un::sun_path is about 107 bytes and a ScratchDir under a deep TMPDIR can
// exceed that, so the test's bus socket gets a short name directly under /tmp.
class ShortSocket {
public:
    explicit ShortSocket(const std::string& tag) {
        static std::atomic<unsigned> counter{0};
        path_ = "/tmp/hsbrk_" + std::to_string(static_cast<long>(::getpid())) + "_" +
                std::to_string(counter++) + "_" + tag + ".sock";
    }
    ~ShortSocket() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    ShortSocket(const ShortSocket&) = delete;
    ShortSocket& operator=(const ShortSocket&) = delete;
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

class ChildProcess {
public:
    ~ChildProcess() { stop(); }
    pid_t pid = -1;

    // Waits up to the deadline; returns true when it exited on its own.
    bool wait_for_exit(int timeout_ms) {
        if (pid <= 0) return true;
        for (int waited = 0; waited < timeout_ms; waited += 20) {
            int status = 0;
            const pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                pid = -1;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

    void stop() {
        if (pid <= 0) return;
        ::kill(pid, SIGTERM);
        if (!wait_for_exit(2000)) {
            ::kill(pid, SIGKILL);
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
        pid = -1;
    }
};

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

} // namespace

TEST_CASE("Broker --launch-all: bus identity and guard, real child argv, no duplicate slot") {
    test::ScratchDir work("broker_launch");
    const fs::path root = work.path();
    const fs::path data_dir = root / "data";
    fs::create_directories(data_dir);

    const fs::path roster = root / "roster.ini";
    write_text(roster,
               "[slot05]\n"
               "slot_number=5\n"
               "launch_order=1\n"
               "account=stubfive\n"
               "character=Stubfive\n"
               "headless=0\n"
               "\n"
               "[slot07]\n"
               "slot_number=7\n"
               "launch_order=2\n"
               "account=stubseven\n"
               "character=Stubseven\n"
               "headless=0\n");

    const fs::path argv_log = root / "argv.log";
    const fs::path stub = root / "stub-child.sh";
    write_text(stub,
               "#!/bin/sh\n"
               "if [ -n \"$HAILSTONE_TEST_ARGV_LOG\" ]; then\n"
               "  printf 'ARG %s|' \"$@\" >> \"$HAILSTONE_TEST_ARGV_LOG\"\n"
               "  printf '\\n' >> \"$HAILSTONE_TEST_ARGV_LOG\"\n"
               "fi\n"
               "exit 0\n");
    fs::permissions(stub, fs::perms::owner_all, fs::perm_options::replace);

    const fs::path broker_out = root / "broker.out";
    ShortSocket bus_socket("brokerlaunch");

    std::vector<std::string> argv{
        HAILSTONE_BROKER_BIN,
        "--data-dir", data_dir.string(),
        "--socket", bus_socket.path(),
        "--roster", roster.string(),
        "--hailstone-bin", stub.string(),
        "--stagger-ms", "2000",
        "--status-interval-s", "0",
        "--launch-all",
        "--exit-when-empty",
    };
    std::vector<char*> cargv;
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    ChildProcess broker;
    broker.pid = ::fork();
    REQUIRE(broker.pid >= 0);
    if (broker.pid == 0) {
        const int fd = ::open(broker_out.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            ::close(fd);
        }
        ::setenv("HAILSTONE_TEST_ARGV_LOG", argv_log.string().c_str(), 1);
        ::execv(cargv[0], cargv.data());
        ::_exit(127);
    }

    // The peer claims slot07, which the roster names with no session id, before the
    // broker reaches it in the stagger window.
    bool saw_socket = false;
    for (int i = 0; i < 500 && !saw_socket; ++i) {
        std::error_code ec;
        saw_socket = fs::exists(bus_socket.path(), ec);
        if (!saw_socket) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(saw_socket);

    ipc::SessionBusClient peer;
    REQUIRE(peer.connect(bus_socket.path(), "slot07"));
    // The client buffers its Hello: pump until the broker's 2 s stagger window ends
    // so the guard reads a roster that already carries slot07 as online.
    for (int i = 0; i < 20; ++i) {
        peer.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const bool exited = broker.wait_for_exit(20000);
    const std::string out = read_text(broker_out);
    std::error_code ec;
    const bool have_argv = fs::exists(argv_log, ec);
    const std::string argv_text = have_argv ? read_text(argv_log) : std::string{};
    peer.disconnect();

    INFO("broker output:\n" << out);
    INFO("child argv:\n" << argv_text);
    REQUIRE(have_argv);
    REQUIRE(exited);

    // Exactly one child: slot05 launched, slot07 refused because a peer holds that tag.
    std::size_t launched = 0;
    bool slot05_ok = false;
    bool slot07_seen = false;
    std::istringstream lines(argv_text);
    for (std::string line; std::getline(lines, line);) {
        if (!line.starts_with("ARG ")) continue;
        ++launched;
        // The stub prefixes every argument with "ARG "; strip it so the joins match.
        std::string norm = line;
        for (auto at = norm.find("ARG "); at != std::string::npos; at = norm.find("ARG ", at)) {
            norm.erase(at, 4);
        }
        if (norm.find("|--session-id|slot05|") != std::string::npos) slot05_ok = true;
        if (norm.find("slot07") != std::string::npos) slot07_seen = true;
        // The child's bus identity is the isolated test bus, not the default one.
        CHECK(norm.find("|--ipc-socket|" + bus_socket.path() + "|") != std::string::npos);
        CHECK(norm.find("|--account|stubfive|") != std::string::npos);
    }
    CHECK(launched == 1);
    CHECK(slot05_ok);
    CHECK_FALSE(slot07_seen);

    // The supervisor records the same tag it put in the child's argv.
    CHECK(out.find("Launched order=1 slot=5 (slot05)") != std::string::npos);
    CHECK(out.find("Skipped order=2 slot=7 (slot07)") != std::string::npos);
    CHECK(out.find("already online in the session bus") != std::string::npos);
}
#endif
