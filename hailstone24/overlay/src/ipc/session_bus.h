#pragma once

// Hailstone 24 Local Session & Actor IPC Bus (Unix domain socket on Linux).
// Provides:
//   1. Exclusive foreground switching & ExecutionState coordination across 24 processes
//   2. Remote command dispatch (/bca, /bcaa, /bc, /rc, /dgt, /dquery, /dobserve, /switch) with explicit ACKs
//   3. MacroQuest `require('actors')` mailbox routing across all connected Hailstone 24 clients
//   4. Compact telemetry publishing (zone, target, HP/mana%, RG Mercs status, RAM/VRAM, ping)
//
// Security: local AF_UNIX socket with 0700 directory, 0600 socket permissions, and SO_PEERCRED same-UID verification.
// Never logs account credentials or tokens.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "app/execution_state.h"

namespace hailstone::ipc {

inline constexpr std::uint16_t kBusProtocolVersion = 1;
inline constexpr std::size_t kMaxBusFrameBytes = 1u << 20; // 1 MiB bounded frame
inline constexpr std::size_t kMaxPendingInbound = 256;

enum class MsgType : std::uint16_t {
    Hello = 1,
    HelloAck = 2,
    Telemetry = 3,
    CommandDispatch = 4,
    CommandAck = 5,
    ActorSend = 6,
    StateControl = 7,
    RosterQuery = 8,
    RosterReply = 9,
    ObserveQuery = 10,
    ObserveReply = 11,
};

enum class CommandScope : std::uint8_t {
    Self = 0,
    AllExceptSelf = 1,  // /bca
    AllIncludingSelf = 2, // /bcaa
    ZoneExceptSelf = 3, // /bcz / /rcz
    ZoneIncludingSelf = 4,
    GroupExceptSelf = 5,
    GroupIncludingSelf = 6,
    TargetPeer = 7,     // /rc <char> or /dquery <char>
    Channel = 8,        // /rc <channel>
};

struct SessionSnapshot {
    std::string session_id;
    std::uint32_t pid = 0;
    std::string character_name;
    std::string server_name;
    std::string class_short;
    std::uint8_t level = 0;
    std::string zone_short;
    std::string target_name;
    std::uint32_t target_id = 0;
    std::uint8_t hp_pct = 0;
    std::uint8_t mana_pct = 0;
    std::uint8_t end_pct = 0;
    std::uint8_t group_slot = 0;
    std::string group_leader;
    app::ExecutionState exec_state = app::ExecutionState::Disconnected;
    std::string rgmercs_state; // "Running", "Paused", "Stopped"
    std::string script_health; // "OK", "ERR:..."
    std::uint32_t ping_ms = 0;
    std::uint32_t rss_mib = 0;
    std::uint32_t vram_mib = 0;
    std::uint32_t last_switch_ms = 0;
    std::uint64_t updated_ms = 0;
    bool online = false;
};

struct ActorPacket {
    std::string sender_session;
    std::string sender_char;
    std::string sender_server;
    std::string mailbox;
    std::string script;
    std::string target_server;
    std::string target_char;
    std::string payload_json; // serialized Lua table/value
};

struct CommandPacket {
    std::uint64_t seq = 0;
    CommandScope scope = CommandScope::TargetPeer;
    std::string target;      // character or channel name
    std::string sender_char;
    std::string sender_zone;
    std::string command;     // e.g. "/lua run rgmercs" or "/sit"
};

struct CommandAckPacket {
    std::uint64_t seq = 0;
    std::string session_id;
    bool ok = true;
    std::string message;
};

struct StateControlPacket {
    std::string target_session;
    app::ExecutionState desired_state = app::ExecutionState::ForegroundFull;
};

// Default runtime socket path: $XDG_RUNTIME_DIR/hailstone24/bus.sock (fallback /tmp/hailstone24-<uid>/bus.sock).
std::string default_bus_socket_path();

// Wire serialization helpers (length-prefixed frames; bounds-checked).
std::vector<std::uint8_t> encode_frame(MsgType type, const std::vector<std::uint8_t>& body);
std::vector<std::uint8_t> encode_snapshot(const SessionSnapshot& s);
bool decode_snapshot(const std::uint8_t* data, std::size_t size, SessionSnapshot& out);
std::vector<std::uint8_t> encode_command(const CommandPacket& cmd);
bool decode_command(const std::uint8_t* data, std::size_t size, CommandPacket& out);
std::vector<std::uint8_t> encode_command_ack(const CommandAckPacket& ack);
bool decode_command_ack(const std::uint8_t* data, std::size_t size, CommandAckPacket& out);
std::vector<std::uint8_t> encode_actor(const ActorPacket& pkt);
bool decode_actor(const std::uint8_t* data, std::size_t size, ActorPacket& out);
std::vector<std::uint8_t> encode_state_control(const StateControlPacket& ctrl);
bool decode_state_control(const std::uint8_t* data, std::size_t size, StateControlPacket& out);

class SessionBusClient {
public:
    SessionBusClient() = default;
    ~SessionBusClient();

    SessionBusClient(const SessionBusClient&) = delete;
    SessionBusClient& operator=(const SessionBusClient&) = delete;

    bool connect(const std::string& socket_path, const std::string& session_id);
    void disconnect();
    bool connected() const noexcept;

    void set_on_command(std::function<CommandAckPacket(const CommandPacket&)> cb);
    void set_on_actor(std::function<void(const ActorPacket&)> cb);
    void set_on_state_control(std::function<void(const StateControlPacket&)> cb);
    void set_on_roster(std::function<void(std::vector<SessionSnapshot>)> cb);

    // Non-blocking pump called once per simulation tick. Reconnects gracefully if broker restarts.
    void poll();

    bool publish_telemetry(const SessionSnapshot& snapshot);
    std::uint64_t send_command(CommandPacket cmd);
    bool send_actor(const ActorPacket& pkt);
    bool request_switch_foreground(const std::string& target_session_or_char);
    bool request_roster();

    // Cached peer roster for DanNet TLO / peer queries without blocking.
    std::vector<SessionSnapshot> cached_roster() const;
    std::optional<SessionSnapshot> find_peer(std::string_view name) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class SessionBusServer {
public:
    SessionBusServer() = default;
    ~SessionBusServer();

    SessionBusServer(const SessionBusServer&) = delete;
    SessionBusServer& operator=(const SessionBusServer&) = delete;

    bool start(const std::string& socket_path);
    void stop();
    bool running() const noexcept;

    // Non-blocking poll/dispatch step; returns true if work was done.
    bool poll_once(int timeout_ms = 5);

    std::vector<SessionSnapshot> roster() const;
    bool switch_foreground(const std::string& target_session_or_char);
    std::uint64_t dispatch_command(CommandPacket cmd);
    std::vector<CommandAckPacket> take_acks();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hailstone::ipc
