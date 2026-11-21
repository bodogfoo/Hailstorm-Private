#pragma once

// Shared by the GPU IPC client (gpu_ipc.cpp, in the client) and its server (gpu_ipc_server.cpp, only in the broker
// tool and the tests). Not for use outside src/platform/.

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <string_view>

#include "platform/gpu_ipc.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace hailstone::platform::gpu_ipc_detail {

inline constexpr std::string_view kPipePrefix = R"(\\.\pipe\hailstone-gpu-)";
inline constexpr std::string_view kUnixEndpointTag = "hailstone-gpu-";

[[noreturn]] void invalid_frame();
[[noreturn]] void transport_failure();
std::array<std::byte, 32> token_bytes(std::string_view token);
void validate_session(const GpuIpcSession& session);
void validate_timeout(int timeout_ms);

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

#if defined(_WIN32)

class WinHandle {
public:
    WinHandle() = default;
    explicit WinHandle(HANDLE h) : h_(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    ~WinHandle() { reset(); }
    WinHandle(WinHandle&& other) noexcept : h_(other.h_) { other.h_ = nullptr; }
    WinHandle& operator=(WinHandle&& other) noexcept {
        if (this != &other) {
            reset();
            h_ = other.h_;
            other.h_ = nullptr;
        }
        return *this;
    }
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }
    void reset() noexcept {
        if (h_) ::CloseHandle(h_);
        h_ = nullptr;
    }

private:
    HANDLE h_ = nullptr;
};

DWORD remaining_ms(Deadline deadline);
bool stopped(HANDLE stop, HANDLE peer = nullptr);
bool complete_io(HANDLE pipe, OVERLAPPED& operation, HANDLE stop, HANDLE peer, Deadline deadline, DWORD& transferred);
void read_bytes(HANDLE pipe, std::span<std::byte> bytes, HANDLE stop, HANDLE peer, Deadline deadline);
void write_bytes(HANDLE pipe, std::span<const std::byte> bytes, HANDLE stop, HANDLE peer, Deadline deadline);
gpu_ipc_wire::Header read_header(HANDLE pipe, HANDLE stop, HANDLE peer, Deadline deadline,
                                 Deadline idle_deadline = Deadline::min());
void write_header(HANDLE pipe, const gpu_ipc_wire::Header& header, HANDLE stop, HANDLE peer, Deadline deadline);

#elif defined(__linux__)

class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    int get() const noexcept { return fd_; }
    int release() noexcept {
        const int out = fd_;
        fd_ = -1;
        return out;
    }
    explicit operator bool() const noexcept { return fd_ >= 0; }
    void reset() noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
};

int remaining_ms_linux(Deadline deadline);
bool unix_read_bytes(int fd, std::span<std::byte> bytes, int stop_fd, Deadline deadline, int* received_fd = nullptr);
bool unix_write_bytes(int fd, std::span<const std::byte> bytes, int stop_fd, Deadline deadline, int send_fd = -1);
gpu_ipc_wire::Header unix_read_header(int fd, int stop_fd, Deadline deadline,
                                      Deadline idle_deadline = Deadline::min(), int* received_fd = nullptr);
void unix_write_header(int fd, const gpu_ipc_wire::Header& header, int stop_fd, Deadline deadline, int send_fd = -1);

#endif

} // namespace hailstone::platform::gpu_ipc_detail
