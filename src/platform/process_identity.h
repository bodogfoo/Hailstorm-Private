#pragma once

// Whether a roster PID still is the Hailstone session it was reported as. A roster entry outlives the process it
// describes and a PID is reused, so nothing may signal a cached PID before this check agrees.

#include <cstdint>
#include <string>
#include <string_view>

namespace hailstone::platform {

// True when `pid` is a live process whose executable's name is `expected_program` and whose command line carries
// `--session-id <expected_session_id>` (or `--session-id=<expected_session_id>`). False, with `why` saying which
// check failed, when the process is gone, is another program, or runs another session. Ends the process's own
// PID and the two lowest PIDs are refused outright.
bool verify_session_process(std::uint32_t pid, std::string_view expected_session_id,
                            std::string_view expected_program, std::string* why = nullptr);

// SIGKILL, only after verify_session_process() agrees. True when the signal was sent.
bool kill_verified_session_process(std::uint32_t pid, std::string_view expected_session_id,
                                   std::string_view expected_program, std::string* why = nullptr);

} // namespace hailstone::platform
