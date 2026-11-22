#pragma once

#include <memory>
#include <vector>

#include "cortex/commander_view.h"
#include "cortex/inference_provider.h"
#include "cortex/runtime.h"
#include "cortex/supervisor.h"
#include "ipc/session_bus.h"

namespace hailstone::cortex {

// The application-owned owner of the one shared supervisor and the runtime
// that feeds it: one per client process, created by the client at startup,
// driven from its telemetry tick, and read by its status callback. The
// supervisor it holds is the same one every other Cortex user in the process
// shares (set_provider/set_mode reach it through supervisor()).
//
// It reports state and proposals and executes nothing: there is no method
// here that runs, confirms or dispatches an intention. A proposal leaves as
// data in view(), a human confirms it, and the client's own command path -
// which this class does not touch - is the only thing that can act.
class CortexService {
public:
    // `provider` is the inference backend behind the supervisor; `cfg` the
    // supervisor's queue, cooldown and circuit-breaker settings.
    explicit CortexService(std::shared_ptr<InferenceProvider> provider,
                           std::shared_ptr<MemoryStore> memory = nullptr,
                           SupervisorConfig cfg = {});
    // The same service built from an endpoint instead of a provider: the
    // provisioned assisted-mode server when nothing is configured, or the
    // host/port/model the caller names. The provider is HttpInferenceProvider
    // either way, so the view reports the endpoint actually in use.
    explicit CortexService(InferenceEndpointConfig endpoint,
                           std::shared_ptr<MemoryStore> memory = nullptr,
                           SupervisorConfig cfg = {});
    ~CortexService();

    CortexService(const CortexService&) = delete;
    CortexService& operator=(const CortexService&) = delete;

    void start();   // starts the supervisor's workers; Observe mode until told otherwise
    void stop();
    bool running() const noexcept { return running_; }

    // Observe every online roster entry, submit it and refresh the view.
    // Game thread, one tick at a time (the supervisor behind it is thread-safe).
    const CortexStatusView& tick(const std::vector<ipc::SessionSnapshot>& roster, std::uint64_t now_ms);
    // The same, taking the roster straight from the session bus's cache.
    const CortexStatusView& tick(const ipc::SessionBusClient& bus, std::uint64_t now_ms);

    // The last tick's view; before the first tick, the service's own state.
    const CortexStatusView& view() const noexcept { return view_; }

    // The shared supervisor itself, for the process's other Cortex users to
    // read snapshots and set the mode or the emergency stop. Not an execution
    // surface: CortexSupervisor has no method that runs an intention either.
    CortexSupervisor& supervisor() noexcept { return *supervisor_; }
    const CortexSupervisor& supervisor() const noexcept { return *supervisor_; }

private:
    std::shared_ptr<CortexSupervisor> supervisor_;
    CortexRuntime runtime_;
    bool running_ = false;
    CortexStatusView view_; // available from construction: the service is wired
};

} // namespace hailstone::cortex
