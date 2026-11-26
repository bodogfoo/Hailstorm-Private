// platform/process_identity.cpp: the check "force stop all" needs before it signals a PID a roster remembered.

#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <thread>

#include "platform/process_identity.h"

#if !defined(_WIN32)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace hailstone;

#if !defined(_WIN32)
TEST_CASE("Process identity: a PID is signalled only while it is that same session client") {
    std::string why;

    // Nothing a roster could hold may be signalled blindly.
    CHECK_FALSE(platform::verify_session_process(0, "hs-session", "sh", &why));
    CHECK_FALSE(platform::verify_session_process(1, "hs-session", "sh", &why));
    CHECK_FALSE(platform::verify_session_process(static_cast<std::uint32_t>(::getpid()), "hs-session", "sh", &why));
    CHECK_FALSE(platform::verify_session_process(999999, "hs-session", "sh", &why));
    CHECK_FALSE(platform::verify_session_process(static_cast<std::uint32_t>(::getpid()), "", "sh", &why));

    // A child that really does carry --session-id <id>.
    const std::string id = "hs-test-session-42";
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ::execl("/bin/sh", "sh", "-c", "sleep 10", "sh", "--session-id", id.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    bool verified = false;
    for (int i = 0; i < 200 && !verified; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        verified = platform::verify_session_process(static_cast<std::uint32_t>(child), id, "sh", &why);
    }
    REQUIRE(verified);

    // A reused PID would now carry another program, and a different session id is another client.
    CHECK_FALSE(platform::verify_session_process(static_cast<std::uint32_t>(child), id, "bash", &why));
    CHECK_FALSE(platform::verify_session_process(static_cast<std::uint32_t>(child), "hs-other", "sh", &why));
    CHECK_FALSE(platform::kill_verified_session_process(static_cast<std::uint32_t>(child), "hs-other", "sh", &why));

    // The verified kill lands, and is a kill rather than a request.
    CHECK(platform::kill_verified_session_process(static_cast<std::uint32_t>(child), id, "sh", &why));
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGKILL);

    // Once reaped the PID is gone, so a stale roster entry cannot make a second kill happen.
    CHECK_FALSE(platform::kill_verified_session_process(static_cast<std::uint32_t>(child), id, "sh", &why));
}
#endif
