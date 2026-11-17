#include "platform/gpu_ipc.h"

#include "core/byte_io.h"
#include "core/env.h"
#include "platform/gpu_ipc_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace hailstone::platform {
namespace gpu_ipc_detail {

namespace {

void validate_header(const gpu_ipc_wire::Header& h) {
    using K = gpu_ipc_wire::Kind;
    if (h.body_size > kGpuIpcMaxBody) invalid_frame();
    switch (h.kind) {
    case K::Authenticate:
        if (h.sequence != 0 || h.body_size != 32 || h.has_handle) invalid_frame();
        break;
    case K::Authenticated:
        if (h.sequence != 0 || h.body_size != 0 || h.has_handle) invalid_frame();
        break;
    case K::Request:
        if (h.sequence == 0 || h.has_handle) invalid_frame();
        break;
    case K::Response:
        if (h.sequence == 0) invalid_frame();
        break;
    default: invalid_frame();
    }
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

constexpr std::uint32_t kMagic = 0x50494748; // HGIP, assembled by ByteReader/ByteWriter.
constexpr std::uint16_t kVersion = 1;

#if defined(_WIN32)
bool transfer(HANDLE pipe, std::span<std::byte> bytes, bool write, HANDLE stop, HANDLE peer, Deadline deadline) {
    WinHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return false;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (stopped(stop, peer) || remaining_ms(deadline) == 0) return false;
        OVERLAPPED operation{};
        operation.hEvent = event.get();
        ::ResetEvent(event.get());
        DWORD transferred = 0;
        const DWORD count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1U << 20));
        const BOOL immediate = write ? ::WriteFile(pipe, bytes.data() + offset, count, &transferred, &operation)
                                     : ::ReadFile(pipe, bytes.data() + offset, count, &transferred, &operation);
        if (!immediate && (::GetLastError() != ERROR_IO_PENDING ||
                           !complete_io(pipe, operation, stop, peer, deadline, transferred))) return false;
        if (!transferred || transferred > count) return false;
        offset += transferred;
    }
    return true;
}
#endif

} // namespace

void invalid_frame() { throw std::runtime_error("GPU IPC protocol frame is invalid"); }
void transport_failure() { throw std::runtime_error("GPU IPC peer, cancellation or timeout failure"); }

std::array<std::byte, 32> token_bytes(std::string_view token) {
    if (token.size() != 64) throw std::runtime_error("GPU IPC session configuration is invalid");
    std::array<std::byte, 32> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int high = hex_digit(token[i * 2]), low = hex_digit(token[i * 2 + 1]);
        if (high < 0 || low < 0) throw std::runtime_error("GPU IPC session configuration is invalid");
        out[i] = static_cast<std::byte>((high << 4) | low);
    }
    return out;
}

void validate_session(const GpuIpcSession& session) {
#if defined(_WIN32)
    if (!session.endpoint.starts_with(kPipePrefix) || session.endpoint.size() != kPipePrefix.size() + 32)
        throw std::runtime_error("GPU IPC session configuration is invalid");
    for (char c : std::string_view(session.endpoint).substr(kPipePrefix.size()))
        if (hex_digit(c) < 0) throw std::runtime_error("GPU IPC session configuration is invalid");
#elif defined(__linux__)
    if (session.endpoint.empty() || session.endpoint.front() != '/' ||
        session.endpoint.size() >= sizeof(sockaddr_un::sun_path) ||
        session.endpoint.find("..") != std::string::npos ||
        session.endpoint.find(kUnixEndpointTag) == std::string::npos) {
        throw std::runtime_error("GPU IPC session configuration is invalid");
    }
#else
    (void)session;
    throw std::runtime_error("GPU IPC is not available on this platform");
#endif
    (void)token_bytes(session.token);
}

void validate_timeout(int timeout_ms) {
    if (timeout_ms < 1 || timeout_ms > 120000) throw std::invalid_argument("GPU IPC timeout is out of range");
}

#if defined(_WIN32)
DWORD remaining_ms(Deadline deadline) {
    if (deadline == Deadline::max()) return INFINITE;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (remaining <= 0) return 0;
    return static_cast<DWORD>(std::min<std::int64_t>(remaining, INFINITE - 1));
}

bool stopped(HANDLE stop, HANDLE peer) {
    return ::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 ||
           (peer && ::WaitForSingleObject(peer, 0) == WAIT_OBJECT_0);
}

bool complete_io(HANDLE pipe, OVERLAPPED& operation, HANDLE stop, HANDLE peer, Deadline deadline, DWORD& transferred) {
    HANDLE waits[]{operation.hEvent, stop, peer};
    const DWORD result = ::WaitForMultipleObjects(peer ? 3 : 2, waits, FALSE, remaining_ms(deadline));
    if (result == WAIT_OBJECT_0) return ::GetOverlappedResult(pipe, &operation, &transferred, FALSE) != 0;
    ::CancelIoEx(pipe, &operation);
    ::GetOverlappedResult(pipe, &operation, &transferred, TRUE);
    return false;
}

void read_bytes(HANDLE pipe, std::span<std::byte> bytes, HANDLE stop, HANDLE peer, Deadline deadline) {
    if (!transfer(pipe, bytes, false, stop, peer, deadline)) transport_failure();
}

void write_bytes(HANDLE pipe, std::span<const std::byte> bytes, HANDLE stop, HANDLE peer, Deadline deadline) {
    auto mutable_bytes = std::span<std::byte>(const_cast<std::byte*>(bytes.data()), bytes.size());
    if (!transfer(pipe, mutable_bytes, true, stop, peer, deadline)) transport_failure();
}

gpu_ipc_wire::Header read_header(HANDLE pipe, HANDLE stop, HANDLE peer, Deadline deadline, Deadline idle_deadline) {
    std::array<std::byte, gpu_ipc_wire::kHeaderSize> bytes{};
    if (idle_deadline != Deadline::min()) {
        read_bytes(pipe, std::span(bytes).first(1), stop, peer, idle_deadline);
        deadline = Clock::now() + std::chrono::seconds(15);
        read_bytes(pipe, std::span(bytes).subspan(1), stop, peer, deadline);
    } else {
        read_bytes(pipe, bytes, stop, peer, deadline);
    }
    return gpu_ipc_wire::decode_header(bytes);
}

void write_header(HANDLE pipe, const gpu_ipc_wire::Header& header, HANDLE stop, HANDLE peer, Deadline deadline) {
    const auto bytes = gpu_ipc_wire::encode_header(header);
    write_bytes(pipe, bytes, stop, peer, deadline);
}
#elif defined(__linux__)

int remaining_ms_linux(Deadline deadline) {
    if (deadline == Deadline::max()) return -1;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (remaining <= 0) return 0;
    return static_cast<int>(std::min<std::int64_t>(remaining, std::numeric_limits<int>::max()));
}

bool wait_fd(int fd, short events, int stop_fd, Deadline deadline) {
    while (true) {
        const int timeout = remaining_ms_linux(deadline);
        if (timeout == 0) return false;
        struct pollfd pfds[2]{};
        pfds[0].fd = fd;
        pfds[0].events = events;
        pfds[1].fd = stop_fd;
        pfds[1].events = POLLIN;
        const nfds_t nfds = stop_fd >= 0 ? 2 : 1;
        const int rc = ::poll(pfds, nfds, timeout);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (rc == 0) return false;
        if (stop_fd >= 0 && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) return false;
        if (pfds[0].revents & (POLLERR | POLLNVAL)) return false;
        if (pfds[0].revents & (events | POLLHUP)) return true;
    }
}

bool unix_read_bytes(int fd, std::span<std::byte> bytes, int stop_fd, Deadline deadline, int* received_fd) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (!wait_fd(fd, POLLIN, stop_fd, deadline)) return false;
        struct iovec iov{};
        iov.iov_base = bytes.data() + offset;
        iov.iov_len = std::min<std::size_t>(bytes.size() - offset, std::size_t{1} << 20);
        alignas(struct cmsghdr) char cmsg_buf[CMSG_SPACE(sizeof(int))]{};
        struct msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        if (received_fd && *received_fd < 0) {
            msg.msg_control = cmsg_buf;
            msg.msg_controllen = sizeof(cmsg_buf);
        }
        const ssize_t n = ::recvmsg(fd, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        if (n == 0) return false;
        if (msg.msg_controllen > 0) {
            for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
                    cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
                    int got_fd = -1;
                    std::memcpy(&got_fd, CMSG_DATA(cmsg), sizeof(int));
                    if (received_fd && *received_fd < 0) {
                        *received_fd = got_fd;
                    } else if (got_fd >= 0) {
                        ::close(got_fd);
                    }
                }
            }
        }
        offset += static_cast<std::size_t>(n);
    }
    return true;
}

bool unix_write_bytes(int fd, std::span<const std::byte> bytes, int stop_fd, Deadline deadline, int send_fd) {
    std::size_t offset = 0;
    bool fd_sent = (send_fd < 0);
    while (offset < bytes.size()) {
        if (!wait_fd(fd, POLLOUT, stop_fd, deadline)) return false;
        struct iovec iov{};
        iov.iov_base = const_cast<std::byte*>(bytes.data() + offset);
        iov.iov_len = std::min<std::size_t>(bytes.size() - offset, std::size_t{1} << 20);
        alignas(struct cmsghdr) char cmsg_buf[CMSG_SPACE(sizeof(int))]{};
        struct msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        if (!fd_sent && send_fd >= 0) {
            msg.msg_control = cmsg_buf;
            msg.msg_controllen = sizeof(cmsg_buf);
            struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(cmsg), &send_fd, sizeof(int));
            fd_sent = true;
        }
        const ssize_t n = ::sendmsg(fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        if (n == 0) return false;
        offset += static_cast<std::size_t>(n);
    }
    return true;
}

gpu_ipc_wire::Header unix_read_header(int fd, int stop_fd, Deadline deadline, Deadline idle_deadline, int* received_fd) {
    std::array<std::byte, gpu_ipc_wire::kHeaderSize> bytes{};
    if (idle_deadline != Deadline::min()) {
        if (!unix_read_bytes(fd, std::span(bytes).first(1), stop_fd, idle_deadline, received_fd)) transport_failure();
        deadline = Clock::now() + std::chrono::seconds(15);
        if (!unix_read_bytes(fd, std::span(bytes).subspan(1), stop_fd, deadline, received_fd)) transport_failure();
    } else {
        if (!unix_read_bytes(fd, bytes, stop_fd, deadline, received_fd)) transport_failure();
    }
    return gpu_ipc_wire::decode_header(bytes);
}

void unix_write_header(int fd, const gpu_ipc_wire::Header& header, int stop_fd, Deadline deadline, int send_fd) {
    const auto bytes = gpu_ipc_wire::encode_header(header);
    if (!unix_write_bytes(fd, bytes, stop_fd, deadline, send_fd)) transport_failure();
}
#endif

} // namespace gpu_ipc_detail

using namespace gpu_ipc_detail;

namespace gpu_ipc_wire {
std::vector<std::byte> encode_header(const Header& header) {
    validate_header(header);
    ByteWriter out;
    out.le(kMagic);
    out.le(kVersion);
    out.le(static_cast<std::uint8_t>(header.kind));
    out.le(static_cast<std::uint8_t>(header.has_handle ? 1 : 0));
    out.le(header.body_size);
    out.le(header.sequence);
    return out.take();
}

Header decode_header(std::span<const std::byte> bytes) {
    if (bytes.size() != kHeaderSize) invalid_frame();
    ByteReader in(bytes);
    if (in.le<std::uint32_t>() != kMagic || in.le<std::uint16_t>() != kVersion) invalid_frame();
    Header out;
    out.kind = static_cast<Kind>(in.le<std::uint8_t>());
    const auto flags = in.le<std::uint8_t>();
    if ((flags & ~1U) != 0) invalid_frame();
    out.has_handle = (flags & 1) != 0;
    out.body_size = in.le<std::uint32_t>();
    out.sequence = in.le<std::uint64_t>();
    validate_header(out);
    return out;
}
} // namespace gpu_ipc_wire

bool gpu_ipc_available() noexcept {
#if defined(_WIN32) || defined(__linux__)
    return true;
#else
    return false;
#endif
}

std::optional<GpuIpcSession> gpu_ipc_session_from_environment() {
    const char* endpoint = core::env("GPU_IPC_ENDPOINT");
    const char* token = core::env("GPU_IPC_TOKEN");
    const char* server = core::env("GPU_IPC_SERVER_PID");
    const bool has_endpoint = endpoint && *endpoint, has_token = token && *token, has_server = server && *server;
    if (!has_endpoint && !has_token && !has_server) return std::nullopt;
    if (!has_endpoint || !has_token) throw std::runtime_error("GPU IPC session environment is incomplete");
    GpuIpcSession session{endpoint, token, 0};
    validate_session(session);
    if (has_server) {
        const auto parsed = std::from_chars(server, server + std::char_traits<char>::length(server), session.server_process_id);
        if (parsed.ec != std::errc{} || *parsed.ptr || !session.server_process_id)
            throw std::runtime_error("GPU IPC session process pin is invalid");
    }
    return session;
}

TypedGpuIpcHandle::~TypedGpuIpcHandle() { reset(); }
TypedGpuIpcHandle::TypedGpuIpcHandle(TypedGpuIpcHandle&& other) noexcept : value_(std::exchange(other.value_, 0)) {}
TypedGpuIpcHandle& TypedGpuIpcHandle::operator=(TypedGpuIpcHandle&& other) noexcept {
    if (this != &other) { reset(); value_ = std::exchange(other.value_, 0); }
    return *this;
}
TypedGpuIpcHandle TypedGpuIpcHandle::adopt(void* native) {
    TypedGpuIpcHandle out;
    if (!native) return out;
#if defined(_WIN32)
    if (native == INVALID_HANDLE_VALUE) throw std::invalid_argument("GPU IPC handle is invalid");
    out.value_ = reinterpret_cast<std::uintptr_t>(native);
    return out;
#elif defined(__linux__)
    const std::uintptr_t encoded = reinterpret_cast<std::uintptr_t>(native);
    if (encoded == 0) return out;
    out.value_ = encoded;
    return out;
#else
    throw std::runtime_error("GPU IPC is not available on this platform");
#endif
}
void* TypedGpuIpcHandle::release() noexcept { return reinterpret_cast<void*>(std::exchange(value_, 0)); }
void TypedGpuIpcHandle::reset() noexcept {
#if defined(_WIN32)
    if (value_) ::CloseHandle(reinterpret_cast<HANDLE>(std::exchange(value_, 0)));
#elif defined(__linux__)
    if (value_) {
        const int fd = static_cast<int>(std::exchange(value_, 0)) - 1;
        if (fd >= 0) ::close(fd);
    }
#else
    value_ = 0;
#endif
}

#if defined(_WIN32)
struct GpuIpcClient::Impl {
    WinHandle pipe, stop_event, peer_process;
    std::mutex mutex;
    std::uint64_t sequence = 0;
};
#elif defined(__linux__)
struct GpuIpcClient::Impl {
    UniqueFd sock;
    UniqueFd stop_read, stop_write;
    std::mutex mutex;
    std::uint64_t sequence = 0;
    std::atomic<bool> closed{false};
};
#else
struct GpuIpcClient::Impl {};
#endif

GpuIpcClient::GpuIpcClient(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GpuIpcClient::~GpuIpcClient() { close(); }

std::unique_ptr<GpuIpcClient> GpuIpcClient::connect(const GpuIpcSession& session, int timeout_ms) {
    validate_session(session);
    validate_timeout(timeout_ms);
#if defined(_WIN32)
    auto impl = std::make_unique<Impl>();
    impl->stop_event = WinHandle(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!impl->stop_event) transport_failure();
    const std::wstring name(session.endpoint.begin(), session.endpoint.end());
    const Deadline deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        impl->pipe = WinHandle(::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                          FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (impl->pipe) break;
        if (::GetLastError() != ERROR_PIPE_BUSY || !remaining_ms(deadline)) transport_failure();
        ::WaitNamedPipeW(name.c_str(), std::min<DWORD>(remaining_ms(deadline), 100));
    }
    ULONG server_id = 0;
    if (!::GetNamedPipeServerProcessId(impl->pipe.get(), &server_id) || !server_id ||
        (session.server_process_id && server_id != session.server_process_id)) transport_failure();
    impl->peer_process = WinHandle(::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server_id));
    if (!impl->peer_process || ::GetProcessId(impl->peer_process.get()) != server_id) transport_failure();
    const auto token = token_bytes(session.token);
    write_header(impl->pipe.get(), {gpu_ipc_wire::Kind::Authenticate, 32, 0, false}, impl->stop_event.get(),
                 impl->peer_process.get(), deadline);
    write_bytes(impl->pipe.get(), token, impl->stop_event.get(), impl->peer_process.get(), deadline);
    const auto accepted = read_header(impl->pipe.get(), impl->stop_event.get(), impl->peer_process.get(), deadline);
    if (accepted.kind != gpu_ipc_wire::Kind::Authenticated) invalid_frame();
    return std::unique_ptr<GpuIpcClient>(new GpuIpcClient(std::move(impl)));
#elif defined(__linux__)
    auto impl = std::make_unique<Impl>();
    int pipefd[2]{-1, -1};
    if (::pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) != 0) transport_failure();
    impl->stop_read = UniqueFd(pipefd[0]);
    impl->stop_write = UniqueFd(pipefd[1]);

    impl->sock = UniqueFd(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!impl->sock) transport_failure();

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (session.endpoint.size() >= sizeof(addr.sun_path)) transport_failure();
    std::memcpy(addr.sun_path, session.endpoint.data(), session.endpoint.size() + 1);

    const Deadline deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    if (::connect(impl->sock.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) transport_failure();
        if (!wait_fd(impl->sock.get(), POLLOUT, impl->stop_read.get(), deadline)) transport_failure();
        int err = 0;
        socklen_t err_len = sizeof(err);
        if (::getsockopt(impl->sock.get(), SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 || err != 0) {
            transport_failure();
        }
    }

    struct ucred cred{};
    socklen_t cred_len = sizeof(cred);
    if (::getsockopt(impl->sock.get(), SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0 ||
        cred.uid != ::geteuid() || cred.pid <= 0 ||
        (session.server_process_id && static_cast<std::uint32_t>(cred.pid) != session.server_process_id)) {
        transport_failure();
    }

    const auto token = token_bytes(session.token);
    unix_write_header(impl->sock.get(), {gpu_ipc_wire::Kind::Authenticate, 32, 0, false}, impl->stop_read.get(), deadline);
    if (!unix_write_bytes(impl->sock.get(), token, impl->stop_read.get(), deadline)) transport_failure();
    const auto accepted = unix_read_header(impl->sock.get(), impl->stop_read.get(), deadline);
    if (accepted.kind != gpu_ipc_wire::Kind::Authenticated) invalid_frame();
    return std::unique_ptr<GpuIpcClient>(new GpuIpcClient(std::move(impl)));
#else
    (void)timeout_ms;
    throw std::runtime_error("GPU IPC is not available on this platform");
#endif
}

GpuIpcResult GpuIpcClient::call(std::span<const std::byte> body, int timeout_ms) {
    validate_timeout(timeout_ms);
    if (body.size() > kGpuIpcMaxBody) throw std::invalid_argument("GPU IPC request exceeds the body limit");
#if defined(_WIN32)
    std::lock_guard lock(impl_->mutex);
    if (!impl_->pipe || stopped(impl_->stop_event.get(), impl_->peer_process.get())) transport_failure();
    try {
        if (impl_->sequence == std::numeric_limits<std::uint64_t>::max()) transport_failure();
        const auto sequence = ++impl_->sequence;
        const Deadline deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        write_header(impl_->pipe.get(), {gpu_ipc_wire::Kind::Request, static_cast<std::uint32_t>(body.size()), sequence, false},
                     impl_->stop_event.get(), impl_->peer_process.get(), deadline);
        write_bytes(impl_->pipe.get(), body, impl_->stop_event.get(), impl_->peer_process.get(), deadline);
        const auto response = read_header(impl_->pipe.get(), impl_->stop_event.get(), impl_->peer_process.get(), deadline);
        if (response.kind != gpu_ipc_wire::Kind::Response || response.sequence != sequence) invalid_frame();
        GpuIpcResult result;
        if (response.has_handle) {
            std::array<std::byte, 8> bytes{};
            read_bytes(impl_->pipe.get(), bytes, impl_->stop_event.get(), impl_->peer_process.get(), deadline);
            const auto value = ByteReader(bytes).le<std::uint64_t>();
            if (!value || value > std::numeric_limits<std::uintptr_t>::max()) invalid_frame();
            result.handle = TypedGpuIpcHandle::adopt(reinterpret_cast<void*>(static_cast<std::uintptr_t>(value)));
        }
        result.body.resize(response.body_size);
        read_bytes(impl_->pipe.get(), result.body, impl_->stop_event.get(), impl_->peer_process.get(), deadline);
        return result;
    } catch (...) {
        ::SetEvent(impl_->stop_event.get());
        ::CancelIoEx(impl_->pipe.get(), nullptr);
        impl_->pipe.reset();
        throw;
    }
#elif defined(__linux__)
    std::lock_guard lock(impl_->mutex);
    if (!impl_->sock || impl_->closed.load()) transport_failure();
    try {
        if (impl_->sequence == std::numeric_limits<std::uint64_t>::max()) transport_failure();
        const auto sequence = ++impl_->sequence;
        const Deadline deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        unix_write_header(impl_->sock.get(),
                          {gpu_ipc_wire::Kind::Request, static_cast<std::uint32_t>(body.size()), sequence, false},
                          impl_->stop_read.get(), deadline);
        if (!unix_write_bytes(impl_->sock.get(), body, impl_->stop_read.get(), deadline)) transport_failure();

        int received_fd = -1;
        const auto response = unix_read_header(impl_->sock.get(), impl_->stop_read.get(), deadline,
                                               Deadline::min(), &received_fd);
        if (response.kind != gpu_ipc_wire::Kind::Response || response.sequence != sequence) {
            if (received_fd >= 0) ::close(received_fd);
            invalid_frame();
        }
        GpuIpcResult result;
        if (response.has_handle) {
            if (received_fd < 0) invalid_frame();
            result.handle = TypedGpuIpcHandle::adopt(reinterpret_cast<void*>(static_cast<std::uintptr_t>(received_fd) + 1));
        } else if (received_fd >= 0) {
            ::close(received_fd);
        }
        result.body.resize(response.body_size);
        if (!unix_read_bytes(impl_->sock.get(), result.body, impl_->stop_read.get(), deadline)) transport_failure();
        return result;
    } catch (...) {
        impl_->closed.store(true);
        if (impl_->stop_write) {
            const char b = 1;
            (void)::write(impl_->stop_write.get(), &b, 1);
        }
        impl_->sock.reset();
        throw;
    }
#else
    (void)body;
    throw std::runtime_error("GPU IPC is not available on this platform");
#endif
}

void GpuIpcClient::close() noexcept {
#if defined(_WIN32)
    if (!impl_) return;
    ::SetEvent(impl_->stop_event.get());
    std::lock_guard lock(impl_->mutex);
    if (impl_->pipe) ::CancelIoEx(impl_->pipe.get(), nullptr);
    impl_->pipe.reset();
#elif defined(__linux__)
    if (!impl_) return;
    impl_->closed.store(true);
    if (impl_->stop_write) {
        const char b = 1;
        (void)::write(impl_->stop_write.get(), &b, 1);
    }
    std::lock_guard lock(impl_->mutex);
    impl_->sock.reset();
#endif
}

} // namespace hailstone::platform
