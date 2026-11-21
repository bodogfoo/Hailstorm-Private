#include "ipc/session_bus.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <sys/stat.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace hailstone::ipc {
namespace {

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

std::uint64_t now_epoch_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct Writer {
    std::vector<std::uint8_t> buf;
    void u8(std::uint8_t v) { buf.push_back(v); }
    void u16(std::uint16_t v) {
        buf.push_back(static_cast<std::uint8_t>(v & 0xff));
        buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) buf.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xff));
    }
    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) buf.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xff));
    }
    void str(std::string_view s) {
        const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(s.size(), 65535));
        u32(n);
        buf.insert(buf.end(), reinterpret_cast<const std::uint8_t*>(s.data()),
                   reinterpret_cast<const std::uint8_t*>(s.data()) + n);
    }
};

struct Reader {
    const std::uint8_t* p = nullptr;
    std::size_t rem = 0;
    bool ok = true;

    std::uint8_t u8() {
        if (rem < 1) { ok = false; return 0; }
        std::uint8_t v = *p++;
        --rem;
        return v;
    }
    std::uint16_t u16() {
        if (rem < 2) { ok = false; return 0; }
        std::uint16_t v = static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
        p += 2;
        rem -= 2;
        return v;
    }
    std::uint32_t u32() {
        if (rem < 4) { ok = false; return 0; }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (i * 8);
        p += 4;
        rem -= 4;
        return v;
    }
    std::uint64_t u64() {
        if (rem < 8) { ok = false; return 0; }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (i * 8);
        p += 8;
        rem -= 8;
        return v;
    }
    std::string str() {
        std::uint32_t n = u32();
        if (!ok || n > rem) { ok = false; return {}; }
        std::string out(reinterpret_cast<const char*>(p), n);
        p += n;
        rem -= n;
        return out;
    }
};

constexpr std::uint32_t kWireMagic = 0x34325348; // "HS24"

#if !defined(_WIN32)
bool set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool verify_peer_same_uid(int fd, std::uint32_t& out_pid) {
    struct ucred cred{};
    socklen_t len = sizeof(cred);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return false;
    if (cred.uid != ::geteuid()) return false;
    out_pid = static_cast<std::uint32_t>(cred.pid);
    return true;
}

bool drain_socket(int fd, std::vector<std::uint8_t>& rx_buf,
                  const std::function<void(MsgType, const std::uint8_t*, std::size_t)>& on_msg) {
    std::uint8_t tmp[16384];
    for (;;) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            if (rx_buf.size() + static_cast<std::size_t>(n) > kMaxBusFrameBytes * 2) return false;
            rx_buf.insert(rx_buf.end(), tmp, tmp + n);
        } else if (n == 0) {
            return false; // peer closed
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return false;
        }
    }

    while (rx_buf.size() >= 12) {
        Reader r{rx_buf.data(), 12};
        const std::uint32_t magic = r.u32();
        const std::uint16_t ver = r.u16();
        const MsgType type = static_cast<MsgType>(r.u16());
        const std::uint32_t body_len = r.u32();
        if (magic != kWireMagic || ver != kBusProtocolVersion || body_len > kMaxBusFrameBytes) return false;
        if (rx_buf.size() < 12 + body_len) break;
        on_msg(type, rx_buf.data() + 12, body_len);
        rx_buf.erase(rx_buf.begin(), rx_buf.begin() + static_cast<std::ptrdiff_t>(12 + body_len));
    }
    return true;
}

bool flush_outbox(int fd, std::deque<std::vector<std::uint8_t>>& outbox) {
    while (!outbox.empty()) {
        auto& front = outbox.front();
        ssize_t n = ::send(fd, front.data(), front.size(), MSG_NOSIGNAL);
        if (n > 0) {
            if (static_cast<std::size_t>(n) == front.size()) {
                outbox.pop_front();
            } else {
                front.erase(front.begin(), front.begin() + n);
                break;
            }
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return false;
        }
    }
    return true;
}
#endif

} // namespace

std::string default_bus_socket_path() {
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && *xdg) {
        return (std::filesystem::path(xdg) / "hailstone24" / "bus.sock").string();
    }
#if !defined(_WIN32)
    return "/tmp/hailstone24-" + std::to_string(static_cast<unsigned long>(::geteuid())) + "/bus.sock";
#else
    return "hailstone24-bus.sock";
#endif
}

std::vector<std::uint8_t> encode_frame(MsgType type, const std::vector<std::uint8_t>& body) {
    Writer w;
    w.buf.reserve(12 + body.size());
    w.u32(kWireMagic);
    w.u16(kBusProtocolVersion);
    w.u16(static_cast<std::uint16_t>(type));
    w.u32(static_cast<std::uint32_t>(body.size()));
    w.buf.insert(w.buf.end(), body.begin(), body.end());
    return std::move(w.buf);
}

std::vector<std::uint8_t> encode_snapshot(const SessionSnapshot& s) {
    Writer w;
    w.str(s.session_id);
    w.u32(s.pid);
    w.str(s.character_name);
    w.str(s.server_name);
    w.str(s.class_short);
    w.u8(s.level);
    w.str(s.zone_short);
    w.str(s.target_name);
    w.u32(s.target_id);
    w.u8(s.hp_pct);
    w.u8(s.mana_pct);
    w.u8(s.end_pct);
    w.u8(s.group_slot);
    w.str(s.group_leader);
    w.u8(static_cast<std::uint8_t>(s.exec_state));
    w.str(s.combat_state);
    w.str(s.script_health);
    w.u32(s.ping_ms);
    w.u32(s.rss_mib);
    w.u32(s.vram_mib);
    w.u32(s.last_switch_ms);
    w.u64(s.updated_ms);
    w.u8(s.online ? 1 : 0);
    w.str(s.script_name);
    w.str(s.script_state);
    w.str(s.active_profile);
    w.str(s.follow_target);
    w.str(s.connection_state);
    return std::move(w.buf);
}

bool decode_snapshot(const std::uint8_t* data, std::size_t size, SessionSnapshot& out) {
    Reader r{data, size};
    out.session_id = r.str();
    out.pid = r.u32();
    out.character_name = r.str();
    out.server_name = r.str();
    out.class_short = r.str();
    out.level = r.u8();
    out.zone_short = r.str();
    out.target_name = r.str();
    out.target_id = r.u32();
    out.hp_pct = r.u8();
    out.mana_pct = r.u8();
    out.end_pct = r.u8();
    out.group_slot = r.u8();
    out.group_leader = r.str();
    out.exec_state = static_cast<app::ExecutionState>(r.u8());
    out.combat_state = r.str();
    out.script_health = r.str();
    out.ping_ms = r.u32();
    out.rss_mib = r.u32();
    out.vram_mib = r.u32();
    out.last_switch_ms = r.u32();
    out.updated_ms = r.u64();
    out.online = r.u8() != 0;
    if (r.ok && r.rem > 0) {
        out.script_name = r.str();
        if (r.ok && r.rem > 0) out.script_state = r.str();
        if (r.ok && r.rem > 0) out.active_profile = r.str();
        if (r.ok && r.rem > 0) out.follow_target = r.str();
        if (r.ok && r.rem > 0) out.connection_state = r.str();
    }
    return r.ok;
}

std::vector<std::uint8_t> encode_command(const CommandPacket& cmd) {
    Writer w;
    w.u64(cmd.seq);
    w.u8(static_cast<std::uint8_t>(cmd.scope));
    w.str(cmd.target);
    w.str(cmd.sender_char);
    w.str(cmd.sender_zone);
    w.str(cmd.command);
    return std::move(w.buf);
}

bool decode_command(const std::uint8_t* data, std::size_t size, CommandPacket& out) {
    Reader r{data, size};
    out.seq = r.u64();
    out.scope = static_cast<CommandScope>(r.u8());
    out.target = r.str();
    out.sender_char = r.str();
    out.sender_zone = r.str();
    out.command = r.str();
    return r.ok;
}

std::vector<std::uint8_t> encode_command_ack(const CommandAckPacket& ack) {
    Writer w;
    w.u64(ack.seq);
    w.str(ack.session_id);
    w.u8(ack.ok ? 1 : 0);
    w.str(ack.message);
    return std::move(w.buf);
}

bool decode_command_ack(const std::uint8_t* data, std::size_t size, CommandAckPacket& out) {
    Reader r{data, size};
    out.seq = r.u64();
    out.session_id = r.str();
    out.ok = r.u8() != 0;
    out.message = r.str();
    return r.ok;
}

std::vector<std::uint8_t> encode_actor(const ActorPacket& pkt) {
    Writer w;
    w.str(pkt.sender_session);
    w.str(pkt.sender_char);
    w.str(pkt.sender_server);
    w.str(pkt.mailbox);
    w.str(pkt.script);
    w.str(pkt.target_server);
    w.str(pkt.target_char);
    w.str(pkt.payload_json);
    return std::move(w.buf);
}

bool decode_actor(const std::uint8_t* data, std::size_t size, ActorPacket& out) {
    Reader r{data, size};
    out.sender_session = r.str();
    out.sender_char = r.str();
    out.sender_server = r.str();
    out.mailbox = r.str();
    out.script = r.str();
    out.target_server = r.str();
    out.target_char = r.str();
    out.payload_json = r.str();
    return r.ok;
}

std::vector<std::uint8_t> encode_state_control(const StateControlPacket& ctrl) {
    Writer w;
    w.str(ctrl.target_session);
    w.u8(static_cast<std::uint8_t>(ctrl.desired_state));
    w.u8(ctrl.exclusive_foreground ? 1 : 0);
    return std::move(w.buf);
}

bool decode_state_control(const std::uint8_t* data, std::size_t size, StateControlPacket& out) {
    Reader r{data, size};
    out.target_session = r.str();
    out.desired_state = static_cast<app::ExecutionState>(r.u8());
    out.exclusive_foreground = (r.ok && r.rem > 0) ? (r.u8() != 0) : true;
    return r.ok;
}

namespace {
std::atomic<std::uint64_t> g_next_bus_cmd_seq{1};
constexpr std::size_t kMaxTrackedCommands = 64;

std::uint32_t estimate_expected_acks(const std::vector<SessionSnapshot>& roster,
                                     const std::string& self_session,
                                     const CommandPacket& cmd) {
    if (cmd.scope == CommandScope::Self || cmd.scope == CommandScope::TargetPeer) return 1;
    std::string self_leader;
    for (const auto& s : roster) {
        if (ieq(s.session_id, self_session)) {
            self_leader = s.group_leader;
            break;
        }
    }
    std::uint32_t matched = 0;
    for (const auto& s : roster) {
        if (!s.online) continue;
        const bool is_self = ieq(s.session_id, self_session);
        bool match = false;
        switch (cmd.scope) {
        case CommandScope::Self: match = is_self; break;
        case CommandScope::AllExceptSelf: match = !is_self; break;
        case CommandScope::AllIncludingSelf: match = true; break;
        case CommandScope::ZoneExceptSelf:
            match = !is_self && !cmd.sender_zone.empty() && ieq(s.zone_short, cmd.sender_zone);
            break;
        case CommandScope::ZoneIncludingSelf:
            match = !cmd.sender_zone.empty() && ieq(s.zone_short, cmd.sender_zone);
            break;
        case CommandScope::GroupExceptSelf:
            match = !is_self && !self_leader.empty() && ieq(s.group_leader, self_leader);
            break;
        case CommandScope::GroupIncludingSelf:
            match = !self_leader.empty() && ieq(s.group_leader, self_leader);
            break;
        case CommandScope::TargetPeer:
            match = ieq(s.character_name, cmd.target) || ieq(s.session_id, cmd.target);
            break;
        case CommandScope::Channel:
            match = ieq(s.class_short, cmd.target) || ieq(s.zone_short, cmd.target) ||
                    ieq(s.session_id, cmd.target);
            break;
        }
        if (match) ++matched;
    }
    return matched > 0 ? matched : 1;
}
} // namespace

// -----------------------------------------------------------------------------
// SessionBusClient
// -----------------------------------------------------------------------------
struct SessionBusClient::Impl {
    int fd = -1;
    std::string socket_path;
    std::string session_id;
    std::vector<std::uint8_t> rx;
    std::deque<std::vector<std::uint8_t>> tx;
    std::uint64_t last_reconnect_ms = 0;
    std::uint64_t command_timeout_ms = 3000;

    mutable std::mutex mutex;
    std::vector<SessionSnapshot> roster;
    std::deque<TrackedCommand> tracked_cmds;
    std::function<CommandAckPacket(const CommandPacket&)> on_command;
    std::function<void(const CommandAckPacket&)> on_command_ack;
    std::function<void(const ActorPacket&)> on_actor;
    std::function<void(const StateControlPacket&)> on_state_control;
    std::function<void(std::vector<SessionSnapshot>)> on_roster;

    void enqueue(MsgType t, const std::vector<std::uint8_t>& body) {
        if (fd < 0) return;
        if (tx.size() >= kMaxPendingInbound) tx.pop_front();
        tx.push_back(encode_frame(t, body));
    }

    void record_outbound_command_locked(const CommandPacket& cmd) {
        TrackedCommand tc;
        tc.seq = cmd.seq;
        tc.scope = cmd.scope;
        tc.target = cmd.target;
        tc.command = cmd.command;
        tc.sent_ms = now_epoch_ms();
        tc.updated_ms = tc.sent_ms;
        tc.expected_acks = estimate_expected_acks(roster, session_id, cmd);
        tc.status = CommandTrackStatus::Pending;
        tc.summary = "Pending";
        if (tracked_cmds.size() >= kMaxTrackedCommands) tracked_cmds.pop_front();
        tracked_cmds.push_back(std::move(tc));
    }

    void record_ack_locked(const CommandAckPacket& ack) {
        for (auto& tc : tracked_cmds) {
            if (tc.seq != ack.seq) continue;
            tc.acks.push_back(ack);
            tc.updated_ms = now_epoch_ms();
            std::size_t ok_count = 0;
            std::size_t fail_count = 0;
            std::string first_fail;
            std::string first_ok;
            for (const auto& a : tc.acks) {
                if (a.ok) {
                    ++ok_count;
                    if (first_ok.empty() && !a.message.empty()) first_ok = a.message;
                } else {
                    ++fail_count;
                    if (first_fail.empty()) {
                        first_fail = a.session_id.empty() ? a.message : (a.session_id + ": " + a.message);
                    }
                }
            }
            if (fail_count > 0 && ok_count == 0) {
                tc.status = CommandTrackStatus::Failed;
                tc.summary = first_fail.empty() ? "Command rejected" : first_fail;
            } else if (fail_count > 0 && ok_count > 0) {
                tc.status = CommandTrackStatus::Partial;
                tc.summary = std::to_string(ok_count) + " ok, " + std::to_string(fail_count) + " failed (" +
                             (first_fail.empty() ? "rejected" : first_fail) + ")";
            } else if (tc.acks.size() >= tc.expected_acks) {
                tc.status = CommandTrackStatus::Acked;
                tc.summary = first_ok.empty() ? "OK" : first_ok;
            } else {
                tc.status = CommandTrackStatus::Partial;
                tc.summary = "Acked " + std::to_string(ok_count) + "/" + std::to_string(tc.expected_acks);
            }
            return;
        }
    }

    void sweep_command_timeouts_locked() {
        const auto now = now_epoch_ms();
        for (auto& tc : tracked_cmds) {
            if (tc.status != CommandTrackStatus::Pending && tc.status != CommandTrackStatus::Partial) continue;
            bool has_failure = false;
            for (const auto& a : tc.acks) {
                if (!a.ok) { has_failure = true; break; }
            }
            if (has_failure) continue;
            if (now >= tc.sent_ms && (now - tc.sent_ms) >= command_timeout_ms) {
                tc.status = CommandTrackStatus::TimedOut;
                tc.updated_ms = now;
                if (tc.acks.empty()) {
                    tc.summary = "Timed out after " + std::to_string(command_timeout_ms) + "ms";
                } else {
                    tc.summary = "Timed out (" + std::to_string(tc.acks.size()) + "/" +
                                 std::to_string(tc.expected_acks) + " acked)";
                }
            }
        }
    }

    bool try_connect() {
#if defined(_WIN32)
        return false;
#else
        if (socket_path.empty() || socket_path.size() >= sizeof(sockaddr_un::sun_path)) return false;
        int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (s < 0) return false;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);
        if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(s);
            return false;
        }
        std::uint32_t srv_pid = 0;
        if (!verify_peer_same_uid(s, srv_pid)) {
            ::close(s);
            return false;
        }
        set_nonblocking(s);
        fd = s;
        rx.clear();
        tx.clear();
        Writer w;
        w.str(session_id);
        w.u32(static_cast<std::uint32_t>(::getpid()));
        enqueue(MsgType::Hello, w.buf);
        enqueue(MsgType::RosterQuery, {});
        flush_outbox(fd, tx);
        return true;
#endif
    }
};

SessionBusClient::SessionBusClient() = default;
SessionBusClient::~SessionBusClient() { disconnect(); }

bool SessionBusClient::connect(const std::string& socket_path, const std::string& session_id) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->socket_path = socket_path;
    impl_->session_id = session_id;
    return impl_->try_connect();
}

void SessionBusClient::disconnect() {
    if (!impl_) return;
#if !defined(_WIN32)
    if (impl_->fd >= 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
    }
#endif
}

bool SessionBusClient::connected() const noexcept { return impl_ && impl_->fd >= 0; }

std::string SessionBusClient::session_id() const {
    if (!impl_) return {};
    return impl_->session_id;
}

void SessionBusClient::set_on_command(std::function<CommandAckPacket(const CommandPacket&)> cb) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    std::lock_guard lock(impl_->mutex);
    impl_->on_command = std::move(cb);
}

void SessionBusClient::set_on_command_ack(std::function<void(const CommandAckPacket&)> cb) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    std::lock_guard lock(impl_->mutex);
    impl_->on_command_ack = std::move(cb);
}

void SessionBusClient::set_on_actor(std::function<void(const ActorPacket&)> cb) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    std::lock_guard lock(impl_->mutex);
    impl_->on_actor = std::move(cb);
}

void SessionBusClient::set_on_state_control(std::function<void(const StateControlPacket&)> cb) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    std::lock_guard lock(impl_->mutex);
    impl_->on_state_control = std::move(cb);
}

void SessionBusClient::set_on_roster(std::function<void(std::vector<SessionSnapshot>)> cb) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    std::lock_guard lock(impl_->mutex);
    impl_->on_roster = std::move(cb);
}

void SessionBusClient::poll() {
#if !defined(_WIN32)
    if (!impl_) return;
    if (impl_->fd < 0) {
        {
            std::lock_guard lock(impl_->mutex);
            impl_->sweep_command_timeouts_locked();
        }
        const auto now = now_epoch_ms();
        if (now - impl_->last_reconnect_ms >= 2000 && !impl_->socket_path.empty()) {
            impl_->last_reconnect_ms = now;
            impl_->try_connect();
        }
        return;
    }

    if (!flush_outbox(impl_->fd, impl_->tx)) {
        disconnect();
        return;
    }

    const bool ok = drain_socket(impl_->fd, impl_->rx, [&](MsgType type, const std::uint8_t* body, std::size_t len) {
        if (type == MsgType::CommandDispatch) {
            CommandPacket cmd;
            if (!decode_command(body, len, cmd)) return;
            std::function<CommandAckPacket(const CommandPacket&)> cb;
            {
                std::lock_guard lock(impl_->mutex);
                cb = impl_->on_command;
            }
            CommandAckPacket ack{cmd.seq, impl_->session_id, true, {}};
            if (cb) ack = cb(cmd);
            if (ack.seq == 0) ack.seq = cmd.seq;
            if (ack.session_id.empty()) ack.session_id = impl_->session_id;
            impl_->enqueue(MsgType::CommandAck, encode_command_ack(ack));
        } else if (type == MsgType::CommandAck) {
            CommandAckPacket ack;
            if (!decode_command_ack(body, len, ack)) return;
            std::function<void(const CommandAckPacket&)> cb;
            {
                std::lock_guard lock(impl_->mutex);
                impl_->record_ack_locked(ack);
                cb = impl_->on_command_ack;
            }
            if (cb) cb(ack);
        } else if (type == MsgType::ActorSend) {
            ActorPacket pkt;
            if (!decode_actor(body, len, pkt)) return;
            std::function<void(const ActorPacket&)> cb;
            {
                std::lock_guard lock(impl_->mutex);
                cb = impl_->on_actor;
            }
            if (cb) cb(pkt);
        } else if (type == MsgType::StateControl) {
            StateControlPacket ctrl;
            if (!decode_state_control(body, len, ctrl)) return;
            std::function<void(const StateControlPacket&)> cb;
            {
                std::lock_guard lock(impl_->mutex);
                cb = impl_->on_state_control;
            }
            if (cb) cb(ctrl);
        } else if (type == MsgType::RosterReply) {
            Reader r{body, len};
            const std::uint32_t count = r.u32();
            if (!r.ok || count > 128) return;
            std::vector<SessionSnapshot> list;
            list.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint32_t item_len = r.u32();
                if (!r.ok || item_len > r.rem) return;
                SessionSnapshot snap;
                if (!decode_snapshot(r.p, item_len, snap)) return;
                r.p += item_len;
                r.rem -= item_len;
                list.push_back(std::move(snap));
            }
            std::function<void(std::vector<SessionSnapshot>)> cb;
            {
                std::lock_guard lock(impl_->mutex);
                impl_->roster = list;
                cb = impl_->on_roster;
            }
            if (cb) cb(std::move(list));
        }
    });
    {
        std::lock_guard lock(impl_->mutex);
        impl_->sweep_command_timeouts_locked();
    }
    if (!ok) {
        disconnect();
        return;
    }
    if (!flush_outbox(impl_->fd, impl_->tx)) disconnect();
#endif
}

bool SessionBusClient::publish_telemetry(const SessionSnapshot& snapshot) {
    if (!impl_ || impl_->fd < 0) return false;
    impl_->enqueue(MsgType::Telemetry, encode_snapshot(snapshot));
    return true;
}

std::uint64_t SessionBusClient::send_command(CommandPacket cmd) {
    if (!impl_ || impl_->fd < 0) return 0;
    if (cmd.seq == 0) cmd.seq = g_next_bus_cmd_seq.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->record_outbound_command_locked(cmd);
    }
    impl_->enqueue(MsgType::CommandDispatch, encode_command(cmd));
    return cmd.seq;
}

bool SessionBusClient::send_actor(const ActorPacket& pkt) {
    if (!impl_ || impl_->fd < 0) return false;
    impl_->enqueue(MsgType::ActorSend, encode_actor(pkt));
    return true;
}

bool SessionBusClient::request_switch_foreground(const std::string& target_session_or_char) {
    return request_state_control(target_session_or_char, app::ExecutionState::ForegroundFull, true);
}

bool SessionBusClient::request_state_control(const std::string& target_session_or_char,
                                             app::ExecutionState desired_state,
                                             bool exclusive_foreground) {
    if (!impl_ || impl_->fd < 0) return false;
    impl_->enqueue(MsgType::StateControl,
                   encode_state_control({target_session_or_char, desired_state, exclusive_foreground}));
    return true;
}

bool SessionBusClient::request_roster() {
    if (!impl_ || impl_->fd < 0) return false;
    impl_->enqueue(MsgType::RosterQuery, {});
    return true;
}

void SessionBusClient::set_command_timeout_ms(std::uint64_t timeout_ms) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    std::lock_guard lock(impl_->mutex);
    impl_->command_timeout_ms = std::max<std::uint64_t>(timeout_ms, 50);
}

std::vector<TrackedCommand> SessionBusClient::tracked_commands() const {
    if (!impl_) return {};
    std::lock_guard lock(impl_->mutex);
    return {impl_->tracked_cmds.begin(), impl_->tracked_cmds.end()};
}

std::optional<TrackedCommand> SessionBusClient::find_tracked_command(std::uint64_t seq) const {
    if (!impl_) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    for (const auto& tc : impl_->tracked_cmds) {
        if (tc.seq == seq) return tc;
    }
    return std::nullopt;
}

void SessionBusClient::clear_tracked_commands() {
    if (!impl_) return;
    std::lock_guard lock(impl_->mutex);
    impl_->tracked_cmds.clear();
}

std::vector<SessionSnapshot> SessionBusClient::cached_roster() const {
    if (!impl_) return {};
    std::lock_guard lock(impl_->mutex);
    return impl_->roster;
}

std::optional<SessionSnapshot> SessionBusClient::find_peer(std::string_view name) const {
    if (!impl_) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    for (const auto& s : impl_->roster) {
        if (ieq(s.character_name, name) || ieq(s.session_id, name)) return s;
    }
    return std::nullopt;
}

// -----------------------------------------------------------------------------
// SessionBusServer
// -----------------------------------------------------------------------------
struct SessionBusServer::Impl {
    struct Peer {
        int fd = -1;
        std::uint32_t pid = 0;
        std::string session_id;
        SessionSnapshot snapshot;
        std::vector<std::uint8_t> rx;
        std::deque<std::vector<std::uint8_t>> tx;
    };

    struct PendingCommandOrigin {
        std::uint64_t seq = 0;
        int origin_fd = -1;
    };

    int listen_fd = -1;
    std::string socket_path;
    mutable std::mutex mutex;
    std::vector<Peer> peers;
    std::vector<CommandAckPacket> acks;
    std::deque<PendingCommandOrigin> pending_origins;
    std::string active_foreground_session;
    std::uint64_t next_seq = 1;

    void enqueue(Peer& p, MsgType t, const std::vector<std::uint8_t>& body) {
        if (p.tx.size() >= kMaxPendingInbound) p.tx.pop_front();
        p.tx.push_back(encode_frame(t, body));
    }

    static bool is_controller_id(std::string_view id) noexcept {
        return id.size() >= 2 && id[0] == '_' && id[1] == '_';
    }

    std::vector<uint8_t> build_roster_reply_locked() const {
        Writer w;
        std::uint32_t count = 0;
        for (const auto& p : peers) {
            if (!is_controller_id(p.session_id)) ++count;
        }
        w.u32(count);
        for (const auto& p : peers) {
            if (is_controller_id(p.session_id)) continue;
            auto blob = encode_snapshot(p.snapshot);
            w.u32(static_cast<std::uint32_t>(blob.size()));
            w.buf.insert(w.buf.end(), blob.begin(), blob.end());
        }
        return std::move(w.buf);
    }

    void broadcast_roster_locked() {
        auto body = build_roster_reply_locked();
        for (auto& p : peers) enqueue(p, MsgType::RosterReply, body);
    }

    void remember_origin_locked(std::uint64_t seq, int origin_fd) {
        if (seq == 0 || origin_fd < 0) return;
        for (auto& entry : pending_origins) {
            if (entry.seq == seq) {
                entry.origin_fd = origin_fd;
                return;
            }
        }
        if (pending_origins.size() >= 256) pending_origins.pop_front();
        pending_origins.push_back({seq, origin_fd});
    }

    void relay_ack_to_origin_locked(const CommandAckPacket& ack) {
        int target_fd = -1;
        for (const auto& entry : pending_origins) {
            if (entry.seq == ack.seq) {
                target_fd = entry.origin_fd;
                break;
            }
        }
        if (target_fd < 0) return;
        for (auto& p : peers) {
            if (p.fd == target_fd) {
                enqueue(p, MsgType::CommandAck, encode_command_ack(ack));
                break;
            }
        }
    }

    void route_command_locked(const CommandPacket& cmd, Peer* sender) {
        const auto body = encode_command(cmd);
        std::size_t matched = 0;
        for (auto& p : peers) {
            if (is_controller_id(p.session_id)) continue;
            const bool is_self = sender && p.fd == sender->fd;
            bool match = false;
            switch (cmd.scope) {
            case CommandScope::Self: match = is_self; break;
            case CommandScope::AllExceptSelf: match = !is_self; break;
            case CommandScope::AllIncludingSelf: match = true; break;
            case CommandScope::ZoneExceptSelf:
                match = !is_self && !cmd.sender_zone.empty() && ieq(p.snapshot.zone_short, cmd.sender_zone);
                break;
            case CommandScope::ZoneIncludingSelf:
                match = !cmd.sender_zone.empty() && ieq(p.snapshot.zone_short, cmd.sender_zone);
                break;
            case CommandScope::GroupExceptSelf:
                match = !is_self && sender && !sender->snapshot.group_leader.empty() &&
                        ieq(p.snapshot.group_leader, sender->snapshot.group_leader);
                break;
            case CommandScope::GroupIncludingSelf:
                match = sender && !sender->snapshot.group_leader.empty() &&
                        ieq(p.snapshot.group_leader, sender->snapshot.group_leader);
                break;
            case CommandScope::TargetPeer:
                match = ieq(p.snapshot.character_name, cmd.target) || ieq(p.session_id, cmd.target);
                break;
            case CommandScope::Channel:
                match = ieq(p.snapshot.class_short, cmd.target) || ieq(p.snapshot.zone_short, cmd.target) ||
                        ieq(p.session_id, cmd.target);
                break;
            }
            if (match) {
                enqueue(p, MsgType::CommandDispatch, body);
                ++matched;
            }
        }
        if (sender) {
            remember_origin_locked(cmd.seq, sender->fd);
            if (matched == 0) {
                CommandAckPacket miss;
                miss.seq = cmd.seq;
                miss.session_id = "bus";
                miss.ok = false;
                miss.message = cmd.target.empty()
                                   ? "No matching online client"
                                   : ("Target '" + cmd.target + "' is not online");
                enqueue(*sender, MsgType::CommandAck, encode_command_ack(miss));
            }
        }
    }

    void route_actor_locked(const ActorPacket& pkt) {
        const auto body = encode_actor(pkt);
        for (auto& p : peers) {
            if (is_controller_id(p.session_id)) continue;
            if (!pkt.target_char.empty() && !ieq(p.snapshot.character_name, pkt.target_char)) continue;
            if (!pkt.target_server.empty() && !p.snapshot.server_name.empty() &&
                !ieq(p.snapshot.server_name, pkt.target_server))
                continue;
            enqueue(p, MsgType::ActorSend, body);
        }
    }

    bool apply_state_control_locked(const StateControlPacket& ctrl) {
        Peer* chosen = nullptr;
        for (auto& p : peers) {
            if (is_controller_id(p.session_id)) continue;
            if (ieq(p.session_id, ctrl.target_session) || ieq(p.snapshot.character_name, ctrl.target_session)) {
                chosen = &p;
                break;
            }
        }
        if (!chosen) return false;
        if (ctrl.desired_state == app::ExecutionState::ForegroundFull && ctrl.exclusive_foreground) {
            active_foreground_session = chosen->session_id;
            for (auto& p : peers) {
                if (is_controller_id(p.session_id)) continue;
                const auto desired = (&p == chosen) ? app::ExecutionState::ForegroundFull
                                                    : app::ExecutionState::BackgroundHeadless;
                p.snapshot.exec_state = desired;
                enqueue(p, MsgType::StateControl, encode_state_control({p.session_id, desired, true}));
            }
            return true;
        }
        chosen->snapshot.exec_state = ctrl.desired_state;
        if (ctrl.desired_state == app::ExecutionState::ForegroundFull) {
            active_foreground_session = chosen->session_id;
        } else if (ieq(active_foreground_session, chosen->session_id)) {
            active_foreground_session.clear();
        }
        enqueue(*chosen, MsgType::StateControl,
                encode_state_control({chosen->session_id, ctrl.desired_state, ctrl.exclusive_foreground}));
        return true;
    }

    bool apply_foreground_switch_locked(const std::string& target) {
        return apply_state_control_locked({target, app::ExecutionState::ForegroundFull, true});
    }
};

SessionBusServer::SessionBusServer() = default;
SessionBusServer::~SessionBusServer() { stop(); }

bool SessionBusServer::start(const std::string& socket_path) {
#if defined(_WIN32)
    (void)socket_path;
    return false;
#else
    if (!impl_) impl_ = std::make_unique<Impl>();
    stop();
    if (socket_path.empty() || socket_path.size() >= sizeof(sockaddr_un::sun_path)) return false;

    std::error_code ec;
    const auto parent = std::filesystem::path(socket_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        ::chmod(parent.c_str(), 0700);
    }
    ::unlink(socket_path.c_str());

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 64) != 0) {
        ::close(fd);
        return false;
    }
    ::chmod(socket_path.c_str(), 0600);
    set_nonblocking(fd);
    impl_->listen_fd = fd;
    impl_->socket_path = socket_path;
    return true;
#endif
}

void SessionBusServer::stop() {
    if (!impl_) return;
#if !defined(_WIN32)
    std::lock_guard lock(impl_->mutex);
    for (auto& p : impl_->peers) {
        if (p.fd >= 0) ::close(p.fd);
    }
    impl_->peers.clear();
    impl_->pending_origins.clear();
    if (impl_->listen_fd >= 0) {
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
    }
    if (!impl_->socket_path.empty()) {
        ::unlink(impl_->socket_path.c_str());
        impl_->socket_path.clear();
    }
#endif
}

bool SessionBusServer::running() const noexcept { return impl_ && impl_->listen_fd >= 0; }

bool SessionBusServer::poll_once(int timeout_ms) {
#if defined(_WIN32)
    (void)timeout_ms;
    return false;
#else
    if (!impl_ || impl_->listen_fd < 0) return false;

    std::vector<pollfd> fds;
    {
        std::lock_guard lock(impl_->mutex);
        fds.reserve(1 + impl_->peers.size());
        fds.push_back(pollfd{impl_->listen_fd, POLLIN, 0});
        for (const auto& p : impl_->peers) {
            short ev = POLLIN;
            if (!p.tx.empty()) ev |= POLLOUT;
            fds.push_back(pollfd{p.fd, ev, 0});
        }
    }

    const int rc = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout_ms);
    if (rc <= 0) return false;

    std::lock_guard lock(impl_->mutex);
    bool did_work = false;
    bool roster_dirty = false;

    if (fds[0].revents & POLLIN) {
        for (;;) {
            int cfd = ::accept(impl_->listen_fd, nullptr, nullptr);
            if (cfd < 0) break;
            std::uint32_t pid = 0;
            if (!verify_peer_same_uid(cfd, pid)) {
                ::close(cfd);
                continue;
            }
            set_nonblocking(cfd);
            Impl::Peer peer;
            peer.fd = cfd;
            peer.pid = pid;
            peer.snapshot.pid = pid;
            peer.snapshot.online = true;
            peer.snapshot.updated_ms = now_epoch_ms();
            impl_->peers.push_back(std::move(peer));
            did_work = true;
        }
    }

    std::vector<int> dead_fds;
    for (std::size_t i = 0; i < impl_->peers.size() && (i + 1) < fds.size(); ++i) {
        auto& peer = impl_->peers[i];
        const short rev = fds[i + 1].revents;
        if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
            dead_fds.push_back(peer.fd);
            continue;
        }
        if (rev & POLLIN) {
            did_work = true;
            const bool alive = drain_socket(peer.fd, peer.rx, [&](MsgType type, const std::uint8_t* body, std::size_t len) {
                if (type == MsgType::Hello) {
                    Reader r{body, len};
                    peer.session_id = r.str();
                    peer.snapshot.session_id = peer.session_id;
                    peer.snapshot.pid = r.u32();
                    peer.snapshot.online = true;
                    peer.snapshot.updated_ms = now_epoch_ms();
                    impl_->enqueue(peer, MsgType::HelloAck, {});
                    roster_dirty = true;
                } else if (type == MsgType::Telemetry) {
                    SessionSnapshot s;
                    if (decode_snapshot(body, len, s)) {
                        s.pid = peer.pid;
                        s.online = true;
                        s.updated_ms = now_epoch_ms();
                        if (peer.session_id.empty()) peer.session_id = s.session_id;
                        peer.snapshot = std::move(s);
                        roster_dirty = true;
                    }
                } else if (type == MsgType::CommandDispatch) {
                    CommandPacket cmd;
                    if (decode_command(body, len, cmd)) impl_->route_command_locked(cmd, &peer);
                } else if (type == MsgType::CommandAck) {
                    CommandAckPacket ack;
                    if (decode_command_ack(body, len, ack)) {
                        if (impl_->acks.size() >= kMaxPendingInbound) impl_->acks.erase(impl_->acks.begin());
                        impl_->acks.push_back(ack);
                        impl_->relay_ack_to_origin_locked(ack);
                    }
                } else if (type == MsgType::ActorSend) {
                    ActorPacket pkt;
                    if (decode_actor(body, len, pkt)) impl_->route_actor_locked(pkt);
                } else if (type == MsgType::StateControl) {
                    StateControlPacket ctrl;
                    if (decode_state_control(body, len, ctrl)) {
                        if (impl_->apply_state_control_locked(ctrl)) roster_dirty = true;
                    }
                } else if (type == MsgType::RosterQuery) {
                    impl_->enqueue(peer, MsgType::RosterReply, impl_->build_roster_reply_locked());
                }
            });
            if (!alive) {
                dead_fds.push_back(peer.fd);
                continue;
            }
        }
        if ((rev & POLLOUT) || !peer.tx.empty()) {
            if (!flush_outbox(peer.fd, peer.tx)) dead_fds.push_back(peer.fd);
        }
    }

    if (!dead_fds.empty()) {
        for (int dfd : dead_fds) {
            ::close(dfd);
            std::erase_if(impl_->peers, [dfd](const Impl::Peer& p) { return p.fd == dfd; });
            std::erase_if(impl_->pending_origins,
                          [dfd](const Impl::PendingCommandOrigin& o) { return o.origin_fd == dfd; });
        }
        roster_dirty = true;
    }

    if (roster_dirty) impl_->broadcast_roster_locked();

    // Flush any newly enqueued replies/broadcasts immediately so peers observe them without an extra poll cycle.
    std::vector<int> flush_dead;
    for (auto& peer : impl_->peers) {
        if (!peer.tx.empty() && !flush_outbox(peer.fd, peer.tx)) {
            flush_dead.push_back(peer.fd);
        }
    }
    if (!flush_dead.empty()) {
        for (int dfd : flush_dead) {
            ::close(dfd);
            std::erase_if(impl_->peers, [dfd](const Impl::Peer& p) { return p.fd == dfd; });
            std::erase_if(impl_->pending_origins,
                          [dfd](const Impl::PendingCommandOrigin& o) { return o.origin_fd == dfd; });
        }
    }
    return did_work;
#endif
}

std::vector<SessionSnapshot> SessionBusServer::roster() const {
    if (!impl_) return {};
    std::lock_guard lock(impl_->mutex);
    std::vector<SessionSnapshot> out;
    out.reserve(impl_->peers.size());
    for (const auto& p : impl_->peers) {
        if (!Impl::is_controller_id(p.session_id)) out.push_back(p.snapshot);
    }
    return out;
}

bool SessionBusServer::switch_foreground(const std::string& target_session_or_char) {
    return set_peer_state(target_session_or_char, app::ExecutionState::ForegroundFull, true);
}

bool SessionBusServer::set_peer_state(const std::string& target_session_or_char,
                                      app::ExecutionState desired_state,
                                      bool exclusive_foreground) {
    if (!impl_) return false;
    std::lock_guard lock(impl_->mutex);
    const bool ok = impl_->apply_state_control_locked(
        {target_session_or_char, desired_state, exclusive_foreground});
    if (ok) impl_->broadcast_roster_locked();
    return ok;
}

std::uint64_t SessionBusServer::dispatch_command(CommandPacket cmd) {
    if (!impl_) return 0;
    std::lock_guard lock(impl_->mutex);
    if (cmd.seq == 0) cmd.seq = impl_->next_seq++;
    impl_->route_command_locked(cmd, nullptr);
    return cmd.seq;
}

std::vector<CommandAckPacket> SessionBusServer::take_acks() {
    if (!impl_) return {};
    std::lock_guard lock(impl_->mutex);
    auto out = std::move(impl_->acks);
    impl_->acks.clear();
    return out;
}

} // namespace hailstone::ipc
