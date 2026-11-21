// The identity check a SIGKILL needs: see platform/process_identity.h.

#include "platform/process_identity.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <unistd.h>
#endif

namespace hailstone::platform {

namespace {

#if !defined(_WIN32)

// /proc/<pid>/<name> as text, without the trailing newline; empty when it cannot be read (usually: the process
// is gone, or it is not ours to look at).
std::string read_proc_file(std::uint32_t pid, const char* name) {
    std::ifstream in(std::filesystem::path("/proc") / std::to_string(pid) / name, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

std::vector<std::string> split_nul(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\0', start);
        const std::size_t stop = end == std::string::npos ? text.size() : end;
        out.emplace_back(text.substr(start, stop - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

bool command_line_names_session(const std::string& cmdline, std::string_view session_id) {
    const std::vector<std::string> args = split_nul(cmdline);
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--session-id" && i + 1 < args.size()) {
            if (args[i + 1] == session_id) return true;
        } else if (args[i] == "--session-id=" + std::string(session_id)) {
            return true;
        }
    }
    return false;
}

#endif

} // namespace

bool verify_session_process(std::uint32_t pid, std::string_view expected_session_id,
                            std::string_view expected_program, std::string* why) {
    auto fail = [&](std::string reason) {
        if (why) *why = std::move(reason);
        return false;
    };
    if (pid <= 1) return fail("the roster's PID is not a group client");
    if (expected_session_id.empty()) return fail("the roster entry has no session id");
    if (expected_program.empty()) return fail("the client's own program name is unknown");
#if !defined(_WIN32)
    if (pid == static_cast<std::uint32_t>(::getpid())) return fail("refusing to signal this process");
    // comm is the executable's name (15 bytes at most, as the kernel keeps it).
    const std::string comm = read_proc_file(pid, "comm");
    if (comm.empty()) return fail("the process is gone");
    if (comm != expected_program) {
        return fail("the PID now belongs to another program (" + comm + ")");
    }
    const std::string cmdline = read_proc_file(pid, "cmdline");
    if (cmdline.empty()) return fail("the process has no readable command line");
    if (!command_line_names_session(cmdline, expected_session_id)) {
        return fail("the process runs another session");
    }
    return true;
#else
    (void)pid;
    return fail("process identity checks are only implemented on Linux");
#endif
}

bool kill_verified_session_process(std::uint32_t pid, std::string_view expected_session_id,
                                   std::string_view expected_program, std::string* why) {
    if (!verify_session_process(pid, expected_session_id, expected_program, why)) return false;
#if !defined(_WIN32)
    if (::kill(static_cast<pid_t>(pid), SIGKILL) != 0) {
        if (why) *why = "the signal was refused";
        return false;
    }
    return true;
#else
    if (why) *why = "process identity checks are only implemented on Linux";
    return false;
#endif
}

} // namespace hailstone::platform
