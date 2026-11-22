#include "cortex/service.h"

#include <utility>

namespace hailstone::cortex {

CortexService::CortexService(std::shared_ptr<InferenceProvider> provider,
                             std::shared_ptr<MemoryStore> memory,
                             SupervisorConfig cfg)
    : supervisor_(std::make_shared<CortexSupervisor>(std::move(provider), std::move(memory), std::move(cfg))),
      runtime_(supervisor_) {
    view_.available = true;
}

CortexService::~CortexService() { stop(); }

void CortexService::start() {
    if (running_) return;
    supervisor_->start();
    running_ = true;
    view_.running = true;
}

void CortexService::stop() {
    if (!running_) return;
    supervisor_->stop();
    running_ = false;
    view_.running = false;
}

const CortexStatusView& CortexService::tick(const std::vector<ipc::SessionSnapshot>& roster,
                                            std::uint64_t now_ms) {
    view_ = to_commander_view(runtime_.tick(roster, now_ms), running_);
    return view_;
}

const CortexStatusView& CortexService::tick(const ipc::SessionBusClient& bus, std::uint64_t now_ms) {
    return tick(bus.cached_roster(), now_ms);
}

} // namespace hailstone::cortex
