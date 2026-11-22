#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace hailstone::cortex {

struct InferenceRequest {
    std::string character_key;
    std::string system_prompt;
    std::string user_prompt;
    int max_tokens = 96;
    int timeout_ms = 5000;
    std::shared_ptr<std::atomic<bool>> cancelled;
};

struct InferenceResponse {
    bool ok = false;
    std::string content;
    std::string error;
    std::uint32_t latency_ms = 0;
    std::uint32_t prompt_tokens = 0;
    std::uint32_t completion_tokens = 0;
};

class InferenceProvider {
public:
    virtual ~InferenceProvider() = default;
    virtual InferenceResponse complete(const InferenceRequest& req) = 0;
    virtual bool health_check() = 0;
    virtual std::string model_name() const = 0;
    virtual std::string endpoint() const = 0;
};

class MockInferenceProvider final : public InferenceProvider {
public:
    explicit MockInferenceProvider(std::string model = "mock-cortex-1.0");
    InferenceResponse complete(const InferenceRequest& req) override;
    bool health_check() override { return healthy_.load(); }
    std::string model_name() const override { return model_; }
    std::string endpoint() const override { return "mock://local"; }

    void set_healthy(bool healthy) { healthy_.store(healthy); }
    void set_forced_response(std::string json) { forced_response_ = std::move(json); }

private:
    std::string model_;
    std::atomic<bool> healthy_{true};
    std::string forced_response_;
};

class HttpInferenceProvider final : public InferenceProvider {
public:
    HttpInferenceProvider(std::string host = "127.0.0.1",
                          std::uint16_t port = 8100,
                          std::string model = "qwen3-0.6b");

    InferenceResponse complete(const InferenceRequest& req) override;
    bool health_check() override;
    std::string model_name() const override { return model_; }
    std::string endpoint() const override;

    void set_endpoint(std::string host, std::uint16_t port, std::string model);

private:
    std::string host_;
    std::uint16_t port_;
    std::string model_;
};

} // namespace hailstone::cortex
