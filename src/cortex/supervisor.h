#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cortex/inference_provider.h"
#include "cortex/memory.h"
#include "cortex/observation.h"
#include "cortex/types.h"
#include "cortex/validator.h"

namespace hailstone::cortex {

struct SupervisorConfig {
    std::size_t worker_threads = 2;
    std::size_t max_queue_size = 48;
    std::uint32_t per_char_cooldown_ms = 2000;
    std::uint32_t inference_timeout_ms = 5000;
    std::uint32_t circuit_breaker_failures = 5;
    std::uint64_t circuit_breaker_cooldown_ms = 10000;
    ValidationConfig validation;
};

struct AgentSnapshot {
    std::string key;
    CharacterIdentity identity;
    std::string group_leader;
    BrainStatus status = BrainStatus::Disabled;
    OperatingMode mode = OperatingMode::Observe;
    bool enabled = false;
    std::string current_objective;
    Intention current_intention = Intention::None;
    Decision latest_decision;
    std::size_t memory_count = 0;
    std::uint32_t consecutive_errors = 0;
    std::string last_error;
};

struct SupervisorSnapshot {
    bool emergency_stop = false;
    bool inference_healthy = false;
    std::string model_name;
    std::string endpoint;
    std::size_t pending_queue_size = 0;
    std::uint64_t total_requests = 0;
    std::uint64_t total_decisions = 0;
    std::uint64_t total_errors = 0;
    std::vector<AgentSnapshot> agents;
};

class CortexSupervisor {
public:
    CortexSupervisor(std::shared_ptr<InferenceProvider> provider,
                     std::shared_ptr<MemoryStore> memory,
                     SupervisorConfig cfg = {});
    ~CortexSupervisor();

    CortexSupervisor(const CortexSupervisor&) = delete;
    CortexSupervisor& operator=(const CortexSupervisor&) = delete;

    void start();
    void stop();

    void set_provider(std::shared_ptr<InferenceProvider> provider);
    void set_mode(OperatingMode mode);
    void set_agent_enabled(const std::string& key, bool enabled);
    void set_group_enabled(const std::string& group_leader, bool enabled);
    void set_all_enabled(bool enabled);
    void set_objective(const std::string& key, const std::string& objective);
    void set_identity(const CharacterIdentity& id);

    void trigger_emergency_stop();
    void clear_emergency_stop();

    // Non-blocking: updates agent observation and enqueues inference if significant event detected.
    void submit_observation(const CharacterObservation& obs, bool force_think = false);

    // Inter-character communication.
    void send_message(AgentMessage msg);
    std::vector<AgentMessage> drain_messages(const std::string& key);

    SupervisorSnapshot snapshot() const;
    std::vector<Decision> recent_decisions(const std::string& key, std::size_t limit = 8) const;
    std::vector<MemoryEntry> recent_memories(const std::string& key, std::size_t limit = 8) const;

private:
    struct AgentState {
        CharacterIdentity identity;
        std::string group_leader;
        bool enabled = false;
        OperatingMode mode = OperatingMode::Observe;
        BrainStatus status = BrainStatus::Disabled;
        std::string current_objective = "support_group";
        Intention current_intention = Intention::None;
        Decision latest_decision;
        std::deque<Decision> decision_history;
        std::deque<AgentMessage> inbox;
        CharacterObservation last_obs;
        bool has_obs = false;
        std::uint64_t last_inference_ms = 0;
        std::uint32_t consecutive_errors = 0;
        std::string last_error;
        std::shared_ptr<std::atomic<bool>> active_cancel;
    };

    struct QueuedTask {
        std::string key;
        int priority = 0;
        std::uint64_t enqueued_ms = 0;
        CharacterObservation obs;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };

    bool is_significant_event(const AgentState& agent, const CharacterObservation& obs) const;
    std::string build_system_prompt(const AgentState& agent) const;
    std::string build_user_prompt(const AgentState& agent, const CharacterObservation& obs) const;
    void worker_loop();

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::shared_ptr<InferenceProvider> provider_;
    std::shared_ptr<MemoryStore> memory_;
    SupervisorConfig cfg_;

    std::unordered_map<std::string, AgentState> agents_;
    std::deque<QueuedTask> queue_;
    std::vector<std::thread> workers_;
    bool running_ = false;
    bool emergency_stop_ = false;
    OperatingMode global_mode_ = OperatingMode::Observe;

    std::atomic<std::uint64_t> next_msg_id_{1};
    std::atomic<std::uint64_t> total_requests_{0};
    std::atomic<std::uint64_t> total_decisions_{0};
    std::atomic<std::uint64_t> total_errors_{0};
    std::uint32_t cb_failures_ = 0;
    std::uint64_t cb_open_until_ms_ = 0;
};

} // namespace hailstone::cortex
