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

// The assisted-mode inference endpoint this project provisions: the local
// llama-server at 127.0.0.1:8100 serving the Qwen3-1.7B-Q4_K_M GGUF under
// the `cortex-mini` alias (bench-results/summary.json, the
// cortex-inference-server.sh defaults). Production's 8095 server is a
// different thing and is never one of these defaults.
struct InferenceEndpointConfig {
    static constexpr char kDefaultHost[] = "127.0.0.1";
    static constexpr std::uint16_t kDefaultPort = 8100;
    static constexpr char kDefaultModel[] = "cortex-mini";

    // The output budget the endpoint asks for, in tokens. The provisioned
    // model is a Qwen3 reasoning model: at the 64 tokens the supervisor
    // used to hardcode it spent the whole budget on its thinking and the
    // reply stopped with no content at all (SCALE_TEST_REPORT.md finding
    // #6), so the default is the 512 the scale run got valid JSON from -
    // still small beside the 8192-token context the six slots share.
    static constexpr std::uint32_t kDefaultOutputTokens = 512;
    // The floor HttpInferenceProvider enforces: a configured budget below
    // this would let the thinking eat the reply again, so it is raised.
    static constexpr std::uint32_t kMinOutputTokens = 256;
    // The ceiling: the provisioned context is 8192 tokens in total.
    static constexpr std::uint32_t kMaxOutputTokens = 8192;

    std::string host = kDefaultHost;
    std::uint16_t port = kDefaultPort;
    std::string model = kDefaultModel;
    std::uint32_t output_tokens = kDefaultOutputTokens;

    // The same four fields read from HAILSTONE_CORTEX_HOST / _PORT / _MODEL
    // / _MAX_TOKENS (NMSC_ under 0.6, core/env.h). An unset or malformed
    // value keeps the provisioned default, so a plain environment uses this
    // endpoint as-is.
    static InferenceEndpointConfig from_env();
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
    // The defaults are the provisioned endpoint, one source of truth with
    // InferenceEndpointConfig: a caller that names nothing gets the
    // provisioned assisted-mode server, and a caller that names everything
    // replaces it wholesale.
    HttpInferenceProvider(std::string host = InferenceEndpointConfig::kDefaultHost,
                          std::uint16_t port = InferenceEndpointConfig::kDefaultPort,
                          std::string model = InferenceEndpointConfig::kDefaultModel);
    explicit HttpInferenceProvider(InferenceEndpointConfig cfg);

    InferenceResponse complete(const InferenceRequest& req) override;
    bool health_check() override;
    std::string model_name() const override { return model_; }
    std::string endpoint() const override;

    // The output budget this provider actually sends: the configured value
    // raised to InferenceEndpointConfig::kMinOutputTokens and held to
    // kMaxOutputTokens, so a small config cannot starve the reply.
    std::uint32_t output_tokens() const noexcept { return output_tokens_; }

    // The JSON body complete() puts on the wire, exposed so the settings
    // the endpoint sends can be read back instead of inferred from the
    // code that writes them.
    std::string request_body(const InferenceRequest& req) const;

    void set_endpoint(std::string host, std::uint16_t port, std::string model);

private:
    std::string host_;
    std::uint16_t port_;
    std::string model_;
    std::uint32_t output_tokens_ = InferenceEndpointConfig::kDefaultOutputTokens;
};

} // namespace hailstone::cortex
