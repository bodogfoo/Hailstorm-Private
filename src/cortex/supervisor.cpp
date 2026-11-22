#include "cortex/supervisor.h"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace hailstone::cortex {

namespace {

std::uint64_t steady_now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

CortexSupervisor::CortexSupervisor(std::shared_ptr<InferenceProvider> provider,
                                   std::shared_ptr<MemoryStore> memory,
                                   SupervisorConfig cfg)
    : provider_(std::move(provider)), memory_(std::move(memory)), cfg_(cfg) {}

CortexSupervisor::~CortexSupervisor() {
    stop();
}

void CortexSupervisor::start() {
    std::lock_guard<std::mutex> lock(mu_);
    if (running_) return;
    running_ = true;
    // The cache starts empty: the first answer is the refresh thread's.
    health_cached_ = false;
    std::size_t n = std::max<std::size_t>(1, cfg_.worker_threads);
    for (std::size_t i = 0; i < n; ++i) {
        workers_.emplace_back(&CortexSupervisor::worker_loop, this);
    }
    if (provider_ && !health_thread_.joinable()) {
        health_thread_ = std::thread(&CortexSupervisor::health_loop, this);
    }
}

void CortexSupervisor::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!running_) return;
        running_ = false;
        for (auto& task : queue_) {
            if (task.cancelled) task.cancelled->store(true);
        }
        queue_.clear();
    }
    cv_.notify_all();
    health_cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();
    // Joins only after the in-flight probe returns, so shutdown waits at most
    // one probe, never a refresh period.
    if (health_thread_.joinable()) health_thread_.join();
}

void CortexSupervisor::set_provider(std::shared_ptr<InferenceProvider> provider) {
    std::lock_guard<std::mutex> lock(mu_);
    provider_ = std::move(provider);
    if (running_ && provider_ && !health_thread_.joinable()) {
        health_thread_ = std::thread(&CortexSupervisor::health_loop, this);
    }
}

void CortexSupervisor::set_mode(OperatingMode mode) {
    std::lock_guard<std::mutex> lock(mu_);
    global_mode_ = mode;
    for (auto& [k, a] : agents_) {
        a.mode = mode;
    }
}

void CortexSupervisor::set_agent_enabled(const std::string& key, bool enabled) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& a = agents_[key];
    a.enabled = enabled;
    a.mode = global_mode_;
    if (!enabled) {
        a.status = BrainStatus::Disabled;
        if (a.active_cancel) a.active_cancel->store(true);
    } else if (a.status == BrainStatus::Disabled) {
        a.status = BrainStatus::Observing;
    }
}

void CortexSupervisor::set_group_enabled(const std::string& group_leader, bool enabled) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [k, a] : agents_) {
        if (a.group_leader == group_leader) {
            a.enabled = enabled;
            a.mode = global_mode_;
            if (!enabled) {
                a.status = BrainStatus::Disabled;
                if (a.active_cancel) a.active_cancel->store(true);
            } else if (a.status == BrainStatus::Disabled) {
                a.status = BrainStatus::Observing;
            }
        }
    }
}

void CortexSupervisor::set_all_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [k, a] : agents_) {
        a.enabled = enabled;
        a.mode = global_mode_;
        if (!enabled) {
            a.status = BrainStatus::Disabled;
            if (a.active_cancel) a.active_cancel->store(true);
        } else if (a.status == BrainStatus::Disabled) {
            a.status = BrainStatus::Observing;
        }
    }
}

void CortexSupervisor::set_objective(const std::string& key, const std::string& objective) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& a = agents_[key];
    a.current_objective = objective;
    if (memory_ && memory_->is_open() && !a.identity.character_name.empty()) {
        memory_->upsert_profile(a.identity, objective);
    }
}

void CortexSupervisor::set_identity(const CharacterIdentity& id) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string key = id.key();
    auto& a = agents_[key];
    a.identity = id;
    remember_profile(key);
}

// mu_ held. The first time a character's identity is seen its profile is written, so the store
// knows who this agent is; a profile an earlier run left behind is loaded back instead, because
// this run's objective is empty until set_objective() is called and an empty one would wipe it.
void CortexSupervisor::remember_profile(const std::string& key) {
    if (!memory_ || !memory_->is_open()) return;
    auto it = agents_.find(key);
    if (it == agents_.end() || it->second.identity.character_name.empty()) return;
    auto& a = it->second;
    StoredProfile prof;
    if (memory_->load_profile(a.identity.server_name, a.identity.character_name, prof) &&
        !prof.current_objective.empty()) {
        a.current_objective = prof.current_objective;
    } else {
        memory_->upsert_profile(a.identity, a.current_objective);
    }
}

void CortexSupervisor::trigger_emergency_stop() {
    std::lock_guard<std::mutex> lock(mu_);
    emergency_stop_ = true;
    for (auto& task : queue_) {
        if (task.cancelled) task.cancelled->store(true);
    }
    queue_.clear();
    for (auto& [k, a] : agents_) {
        if (a.active_cancel) a.active_cancel->store(true);
        a.current_intention = Intention::None;
        if (a.enabled) a.status = BrainStatus::Waiting;
    }
}

void CortexSupervisor::clear_emergency_stop() {
    std::lock_guard<std::mutex> lock(mu_);
    emergency_stop_ = false;
    for (auto& [k, a] : agents_) {
        if (a.enabled && a.status == BrainStatus::Waiting) {
            a.status = BrainStatus::Observing;
        }
    }
}

bool CortexSupervisor::is_significant_event(const AgentState& agent,
                                            const CharacterObservation& obs) const {
    if (!agent.has_obs) return true;
    const auto& prev = agent.last_obs;
    if (obs.hp_pct <= 25 && prev.hp_pct > 25) return true;
    if (obs.mana_pct <= 20 && prev.mana_pct > 20) return true;
    if (obs.combat_state != prev.combat_state) return true;
    if (obs.target_name != prev.target_name) return true;
    if (obs.zone_short != prev.zone_short) return true;
    if (!agent.inbox.empty()) return true;
    for (const auto& peer : obs.group) {
        if (peer.hp_pct <= 25 || peer.mana_pct <= 15) return true;
    }
    return false;
}

void CortexSupervisor::submit_observation(const CharacterObservation& obs, bool force_think) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string key = obs.server_name + ":" + obs.character_name;
    auto& a = agents_[key];
    if (a.identity.character_name.empty()) {
        a.identity.server_name = obs.server_name;
        a.identity.character_name = obs.character_name;
        a.identity.class_short = obs.class_short;
        a.identity.level = obs.level;
        // Once per character per process: the profile is written the first time this agent is
        // seen, or the one an earlier run left behind is loaded back.
        remember_profile(key);
    }
    a.group_leader = obs.group_leader;

    bool significant = force_think || is_significant_event(a, obs);
    a.last_obs = obs;
    a.has_obs = true;

    if (!a.enabled || emergency_stop_ || global_mode_ == OperatingMode::Disabled) {
        return;
    }
    if (!significant) {
        if (a.status == BrainStatus::Disabled) a.status = BrainStatus::Observing;
        return;
    }

    std::uint64_t now = steady_now_ms();
    if (!force_think && (now - a.last_inference_ms) < cfg_.per_char_cooldown_ms) {
        return;
    }
    if (now < cb_open_until_ms_) {
        a.status = BrainStatus::Waiting;
        return;
    }

    // Deduplicate: if this character already has a queued task, update its observation.
    for (auto& t : queue_) {
        if (t.key == key) {
            t.obs = obs;
            return;
        }
    }

    if (queue_.size() >= cfg_.max_queue_size) {
        // Backpressure: drop oldest low-priority task.
        queue_.pop_front();
    }

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    a.active_cancel = cancel;
    a.status = BrainStatus::Queued;

    int prio = (obs.hp_pct <= 25 || obs.mana_pct <= 15) ? 2 : 1;
    queue_.push_back(QueuedTask{key, prio, now, obs, std::move(cancel)});
    cv_.notify_one();
}

void CortexSupervisor::send_message(AgentMessage msg) {
    std::lock_guard<std::mutex> lock(mu_);
    if (msg.ttl == 0) return;
    msg.id = next_msg_id_.fetch_add(1);
    --msg.ttl;
    if (!msg.to_key.empty()) {
        auto it = agents_.find(msg.to_key);
        if (it != agents_.end()) {
            it->second.inbox.push_back(std::move(msg));
            if (it->second.inbox.size() > 16) it->second.inbox.pop_front();
        }
    } else {
        for (auto& [k, a] : agents_) {
            if (k != msg.from_key &&
                (msg.group_leader.empty() || a.group_leader == msg.group_leader)) {
                a.inbox.push_back(msg);
                if (a.inbox.size() > 16) a.inbox.pop_front();
            }
        }
    }
}

std::vector<AgentMessage> CortexSupervisor::drain_messages(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<AgentMessage> out;
    auto it = agents_.find(key);
    if (it == agents_.end()) return out;
    while (!it->second.inbox.empty()) {
        out.push_back(std::move(it->second.inbox.front()));
        it->second.inbox.pop_front();
    }
    return out;
}

std::string CortexSupervisor::build_system_prompt(const AgentState& agent) const {
    std::ostringstream ss;
    ss << "You are " << agent.identity.character_name << " (" << agent.identity.class_short
       << " L" << static_cast<int>(agent.identity.level) << "). "
       << "Objective: " << agent.current_objective << ". "
       << "Respond ONLY with JSON: {\"action\":\"<intention>\",\"reason\":\"<short_code>\"}. "
       << "Allowed actions: hold_pull, resume_pull, recover_resources, request_assistance, "
       << "regroup, follow_group, continue_current_task, report_problem, fight_to_location, clear_objective.";
    return ss.str();
}

std::string CortexSupervisor::build_user_prompt(const AgentState& agent,
                                                const CharacterObservation& obs) const {
    std::ostringstream ss;
    ss << to_prompt_context(obs);
    if (!agent.inbox.empty()) {
        ss << " inbox=" << agent.inbox.back().payload;
    }
    return ss.str();
}

void CortexSupervisor::worker_loop() {
    while (true) {
        QueuedTask task;
        std::string sys_prompt;
        std::string usr_prompt;
        std::shared_ptr<InferenceProvider> prov;
        std::string server_name;
        std::string char_name;

        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return !running_ || (!queue_.empty() && !emergency_stop_); });
            if (!running_) return;
            if (queue_.empty() || emergency_stop_) continue;

            // Pick highest priority task.
            auto best = std::max_element(queue_.begin(), queue_.end(),
                                         [](const QueuedTask& a, const QueuedTask& b) {
                                             return a.priority < b.priority;
                                         });
            task = std::move(*best);
            queue_.erase(best);

            auto it = agents_.find(task.key);
            if (it == agents_.end() || !it->second.enabled) continue;
            it->second.status = BrainStatus::Thinking;
            it->second.last_inference_ms = steady_now_ms();
            sys_prompt = build_system_prompt(it->second);
            usr_prompt = build_user_prompt(it->second, task.obs);
            server_name = it->second.identity.server_name;
            char_name = it->second.identity.character_name;
            prov = provider_;
        }

        if (!prov) continue;
        total_requests_.fetch_add(1);

        InferenceRequest req;
        req.character_key = task.key;
        req.system_prompt = std::move(sys_prompt);
        req.user_prompt = std::move(usr_prompt);
        // No max_tokens here: the endpoint's configured output budget is
        // what goes on the wire. The 64 this used to hardcode was all spent
        // on the reasoning model's thinking, leaving an empty reply
        // (bench-results/SCALE_TEST_REPORT.md finding #6).
        req.timeout_ms = static_cast<int>(cfg_.inference_timeout_ms);
        req.cancelled = task.cancelled;

        InferenceResponse resp = prov->complete(req);
        std::uint64_t now = steady_now_ms();

        std::lock_guard<std::mutex> lock(mu_);
        auto it = agents_.find(task.key);
        if (it == agents_.end()) continue;
        auto& a = it->second;

        if (emergency_stop_ || (task.cancelled && task.cancelled->load())) {
            a.status = a.enabled ? BrainStatus::Waiting : BrainStatus::Disabled;
            continue;
        }

        if (!resp.ok) {
            total_errors_.fetch_add(1);
            ++a.consecutive_errors;
            a.last_error = resp.error;
            a.status = BrainStatus::Error;
            ++cb_failures_;
            if (cb_failures_ >= cfg_.circuit_breaker_failures) {
                cb_open_until_ms_ = now + cfg_.circuit_breaker_cooldown_ms;
            }
            continue;
        }

        cb_failures_ = 0;
        a.consecutive_errors = 0;
        a.last_error.clear();

        Decision d = validate_decision_json(
            resp.content, task.enqueued_ms, now, resp.latency_ms, cfg_.validation);
        a.latest_decision = d;
        a.decision_history.push_front(d);
        if (a.decision_history.size() > 20) a.decision_history.pop_back();

        if (d.valid) {
            a.current_intention = d.intention;
            a.status = BrainStatus::DecisionReady;
            total_decisions_.fetch_add(1);
        } else {
            a.status = BrainStatus::Error;
            a.last_error = d.rejection_reason;
            total_errors_.fetch_add(1);
        }

        if (memory_ && memory_->is_open() && !char_name.empty()) {
            memory_->add_decision(server_name, char_name, d);
            if (d.valid) {
                // The bounded context the decision was made on, as the prompt saw it (one line),
                // written before the decision so the newest entry stays the decision itself.
                memory_->add_memory(server_name, char_name, "observation",
                                    to_prompt_context(task.obs), now);
                memory_->add_memory(server_name, char_name, "decision",
                                    d.action_raw + ":" + d.reason_code, now);
            }
        }
    }
}

// Runs on health_thread_ only. The provider's health_check() is a connect to
// the model endpoint and may block for seconds against one that never answers,
// so it runs here - off mu_ - and publishes its answer to health_cached_ for
// snapshot() to read. stop() joins this thread, so shutdown waits out at most
// the one probe in flight.
void CortexSupervisor::health_loop() {
    for (;;) {
        std::shared_ptr<InferenceProvider> provider;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!running_) return;
            provider = provider_;
        }

        const bool healthy = provider && provider->health_check();

        std::unique_lock<std::mutex> lock(mu_);
        if (!running_) return;
        health_cached_ = healthy;
        if (health_cv_.wait_for(lock, std::chrono::milliseconds(cfg_.health_refresh_ms),
                                [this] { return !running_; })) {
            return;
        }
    }
}

SupervisorSnapshot CortexSupervisor::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    SupervisorSnapshot snap;
    snap.emergency_stop = emergency_stop_;
    snap.mode = global_mode_;
    snap.pending_queue_size = queue_.size();
    snap.total_requests = total_requests_.load();
    snap.total_decisions = total_decisions_.load();
    snap.total_errors = total_errors_.load();
    if (provider_) {
        snap.model_name = provider_->model_name();
        snap.endpoint = provider_->endpoint();
        // The cached answer the refresh thread published. Probing here would
        // run a TCP connect while holding mu_, stalling every other caller.
        snap.inference_healthy = health_cached_;
    }
    for (const auto& [k, a] : agents_) {
        AgentSnapshot as;
        as.key = k;
        as.identity = a.identity;
        as.group_leader = a.group_leader;
        as.status = a.status;
        as.mode = a.mode;
        as.enabled = a.enabled;
        as.current_objective = a.current_objective;
        as.current_intention = a.current_intention;
        as.latest_decision = a.latest_decision;
        as.consecutive_errors = a.consecutive_errors;
        as.last_error = a.last_error;
        if (memory_ && memory_->is_open() && !a.identity.character_name.empty()) {
            as.memory_count = memory_->memory_count(a.identity.server_name, a.identity.character_name);
        }
        snap.agents.push_back(std::move(as));
    }
    std::sort(snap.agents.begin(), snap.agents.end(),
              [](const AgentSnapshot& x, const AgentSnapshot& y) { return x.key < y.key; });
    return snap;
}

std::vector<Decision> CortexSupervisor::recent_decisions(const std::string& key,
                                                         std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Decision> out;
    auto it = agents_.find(key);
    if (it == agents_.end()) return out;
    for (std::size_t i = 0; i < it->second.decision_history.size() && i < limit; ++i) {
        out.push_back(it->second.decision_history[i]);
    }
    return out;
}

std::vector<MemoryEntry> CortexSupervisor::recent_memories(const std::string& key,
                                                           std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = agents_.find(key);
    if (it == agents_.end() || !memory_ || !memory_->is_open()) return {};
    return memory_->recent_memories(
        it->second.identity.server_name, it->second.identity.character_name, limit);
}

} // namespace hailstone::cortex
