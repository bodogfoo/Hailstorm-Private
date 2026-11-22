// Health-check slice: a supervisor snapshot must answer from a cached health
// state, never by running the inference provider's probe on the caller's
// thread. The provider's health_check() is a TCP connect to the model
// endpoint: on the game/telemetry thread that is a 25 ms stall per probe and
// an unbounded stall (~127 s of SYN retries) when the endpoint black-holes.
#include "doctest.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "cortex/inference_provider.h"
#include "cortex/supervisor.h"

using namespace hailstone::cortex;
using namespace std::chrono_literals;

namespace {

// A game/telemetry tick's worth of patience for a snapshot: reading state
// costs microseconds, so anything near this budget means a probe ran inline.
constexpr auto kSnapshotBudget = 250ms;
constexpr auto kWaitLimit = 5s;

// Waits for `pred` so timing assertions state a deadline instead of leaning on
// scheduler luck.
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds limit = kWaitLimit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

// A provider whose health probe parks until the test releases it (or its own
// cap expires): the shape of a connect to an endpoint that never answers.
// probe_cap_ms = 0 makes the same probe return at once while still counting,
// which is what the probe-count assertions want.
class BlockingHealthProvider final : public InferenceProvider {
public:
    explicit BlockingHealthProvider(long probe_cap_ms) : probe_cap_ms_(probe_cap_ms) {}

    InferenceResponse complete(const InferenceRequest&) override { return InferenceResponse{}; }

    bool health_check() override {
        probes_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(probe_mu_);
            probing_ = true;
        }
        probe_cv_.notify_all();

        bool healthy = true;
        {
            std::unique_lock<std::mutex> lock(probe_mu_);
            probe_cv_.wait_for(lock, std::chrono::milliseconds(probe_cap_ms_),
                               [this] { return released_; });
            probing_ = false;
            healthy = healthy_;
        }
        probe_cv_.notify_all();
        return healthy;
    }

    std::string model_name() const override { return "blocking-probe"; }
    std::string endpoint() const override { return "tcp://blackhole:8100"; }

    void release() {
        {
            std::lock_guard<std::mutex> lock(probe_mu_);
            released_ = true;
        }
        probe_cv_.notify_all();
    }

    void set_healthy(bool healthy) {
        std::lock_guard<std::mutex> lock(probe_mu_);
        healthy_ = healthy;
    }

    // True while a probe is parked inside health_check().
    bool wait_until_probing(std::chrono::milliseconds limit = kWaitLimit) {
        std::unique_lock<std::mutex> lock(probe_mu_);
        return probe_cv_.wait_for(lock, limit, [this] { return probing_; });
    }

    // True once the parked probe has returned.
    bool wait_until_idle(std::chrono::milliseconds limit = kWaitLimit) {
        std::unique_lock<std::mutex> lock(probe_mu_);
        return probe_cv_.wait_for(lock, limit, [this] { return !probing_; });
    }

    int probes() const { return probes_.load(); }

private:
    const long probe_cap_ms_;
    mutable std::mutex probe_mu_;
    std::condition_variable probe_cv_;
    bool probing_ = false;
    bool released_ = false;
    bool healthy_ = true;
    std::atomic<int> probes_{0};
};

} // namespace

TEST_CASE("Cortex supervisor: snapshot() never runs the provider's health probe inline") {
    // The probe parks for 1.5 s (a connect that never answers). If snapshot()
    // runs it while holding mu_, the caller eats the whole stall.
    auto probe = std::make_shared<BlockingHealthProvider>(1500);
    SupervisorConfig cfg;
    cfg.health_refresh_ms = 60;
    CortexSupervisor sup(probe, nullptr, cfg);
    sup.start();

    // The background refresh must be the one inside the probe; if nothing is
    // probing, the budget check below fails anyway, which is the point.
    const bool background_probing = probe->wait_until_probing(2s);

    const auto began = std::chrono::steady_clock::now();
    const auto snap = sup.snapshot();
    const auto elapsed = std::chrono::steady_clock::now() - began;

    CHECK(elapsed < kSnapshotBudget);     // RED: waits out the 1.5 s probe
    CHECK(background_probing);            // RED: nothing refreshed in the background
    CHECK_FALSE(snap.inference_healthy);  // the probe has not answered yet

    probe->release();
    REQUIRE(probe->wait_until_idle());
    CHECK(probe->probes() >= 1);
    // The cache catches up on the background path, without the caller probing.
    CHECK(wait_until([&] { return sup.snapshot().inference_healthy; }));
}

TEST_CASE("Cortex supervisor: snapshots read the cache instead of scheduling probes") {
    auto provider = std::make_shared<BlockingHealthProvider>(0);
    SupervisorConfig cfg;
    cfg.health_refresh_ms = 60'000;  // one refresh now, then silence for a minute
    CortexSupervisor sup(provider, nullptr, cfg);
    sup.start();
    REQUIRE(wait_until([&] { return sup.snapshot().inference_healthy; }));

    const int probes_after_refresh = provider->probes();
    for (int i = 0; i < 100; ++i) {
        CHECK(sup.snapshot().inference_healthy);
    }
    // RED: every snapshot ran its own probe.
    CHECK(provider->probes() == probes_after_refresh);
}

TEST_CASE("Cortex supervisor: the cached health tracks the provider") {
    auto provider = std::make_shared<BlockingHealthProvider>(0);
    SupervisorConfig cfg;
    cfg.health_refresh_ms = 20;
    CortexSupervisor sup(provider, nullptr, cfg);

    // Before the first refresh there is no answer to report yet, which is what
    // a cache that nobody has filled looks like.
    CHECK_FALSE(sup.snapshot().inference_healthy);
    sup.start();
    REQUIRE(wait_until([&] { return sup.snapshot().inference_healthy; }));

    provider->set_healthy(false);
    REQUIRE(wait_until([&] { return !sup.snapshot().inference_healthy; }));
    provider->set_healthy(true);
    REQUIRE(wait_until([&] { return sup.snapshot().inference_healthy; }));
}

TEST_CASE("Cortex supervisor: stop() waits only for the in-flight probe") {
    // stop() joins the refresh thread, so the probe it may be parked in is the
    // one thing shutdown waits for: bounded by that probe's own cap.
    auto probe = std::make_shared<BlockingHealthProvider>(300);
    SupervisorConfig cfg;
    cfg.health_refresh_ms = 60'000;  // exactly one probe, then silence
    CortexSupervisor sup(probe, nullptr, cfg);
    sup.start();
    REQUIRE(probe->wait_until_probing(2s));

    const auto began = std::chrono::steady_clock::now();
    sup.stop();
    const auto elapsed = std::chrono::steady_clock::now() - began;

    CHECK(elapsed < 1s);  // the probe's cap, never the refresh schedule
    CHECK(probe->probes() == 1);
}
