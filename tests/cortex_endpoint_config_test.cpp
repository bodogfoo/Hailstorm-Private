// The assisted-mode inference endpoint this project provisions: the local
// llama-server at 127.0.0.1:8100 serving the Qwen3-1.7B-Q4_K_M GGUF under
// its `cortex-mini` alias (bench-results/summary.json, the
// cortex-inference-server.sh defaults). The client's Cortex service points
// there unless it is configured otherwise. Production's 8095 server is a
// different thing and is never this default.

#include <doctest/doctest.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cortex/inference_provider.h"
#include "cortex/service.h"

using namespace hailstone::cortex;

namespace {

// The tests' own environment variables (the environment is this process's
// alone); set and restored the way tests/process_test.cpp does it.
void set_test_env(const std::string& name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name.c_str(), value);
#else
    setenv(name.c_str(), value, 1);
#endif
}

void clear_test_env(const std::string& name) {
#if defined(_WIN32)
    _putenv_s(name.c_str(), "");
#else
    unsetenv(name.c_str());
#endif
}

// Remembers what the variable held and puts it back when the case ends, so
// the provisioned-default case below is not reading a variable this file set.
struct ScopedEnv {
    explicit ScopedEnv(std::string name) : name_(std::move(name)) {
        if (const char* v = std::getenv(name_.c_str())) {
            had_ = true;
            prior_ = v;
        }
    }
    ~ScopedEnv() {
        if (had_) set_test_env(name_, prior_.c_str());
        else clear_test_env(name_);
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

    void set(const char* value) const { set_test_env(name_, value); }
    void clear() const { clear_test_env(name_); }

private:
    std::string name_;
    std::string prior_;
    bool had_ = false;
};

// Both spellings: core::env() reads HAILSTONE_ and falls back to NMSC_, so a
// stale NMSC_ value must not stand in for a cleared HAILSTONE_ one, and a
// stale HAILSTONE_ one must not survive into the default case.
void clear_cortex_env(const std::vector<ScopedEnv*>& vars) {
    for (const auto* v : vars) v->clear();
}

// A one-shot llama-server stand-in: accepts one connection, records the
// request body it was sent and answers with a completion that carries
// content, so the settings the endpoint puts on the wire can be read back
// instead of inferred from the code that writes them.
class OneShotCompletionServer {
public:
    ~OneShotCompletionServer() { stop(); }

    // False when no socket can be made: the case reports that rather than
    // letting the provider wait on a server that is not there.
    bool start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // the OS picks a free port
        if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (::listen(listen_fd_, 1) != 0) return false;
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
        return true;
    }

    std::uint16_t port() const { return port_; }

    std::string request_body() const {
        std::lock_guard<std::mutex> lock(mu_);
        return body_;
    }

private:
    void serve() {
        // Deadlines throughout: a test whose provider never connects must
        // fail, not hang.
        pollfd listening{listen_fd_, POLLIN, 0};
        if (::poll(&listening, 1, 5000) <= 0) return;
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) return;

        std::string request;
        std::size_t want = std::string::npos;
        char buf[4096];
        while (true) {
            pollfd readable{fd, POLLIN, 0};
            if (::poll(&readable, 1, 5000) <= 0) break;
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            request.append(buf, static_cast<std::size_t>(n));
            if (want == std::string::npos) {
                const std::size_t head = request.find("\r\n\r\n");
                if (head != std::string::npos) {
                    want = head + 4;
                    const std::size_t length = request.find("Content-Length:");
                    if (length != std::string::npos)
                        want += static_cast<std::size_t>(
                            std::strtoul(request.c_str() + length + 15, nullptr, 10));
                }
            }
            if (want != std::string::npos && request.size() >= want) break;
        }

        const std::size_t head = request.find("\r\n\r\n");
        if (head != std::string::npos) {
            std::lock_guard<std::mutex> lock(mu_);
            body_ = request.substr(head + 4, want == std::string::npos ? std::string::npos : want - head - 4);
        }

        static const char kJson[] =
            "{\"choices\":[{\"message\":{\"role\":\"assistant\","
            "\"content\":\"{\\\"action\\\":\\\"continue_current_task\\\","
            "\\\"reason\\\":\\\"nominal\\\"}\"}}]}";
        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(sizeof(kJson) - 1) + "\r\nConnection: close\r\n\r\n" + kJson;
        for (std::size_t sent = 0; sent < response.size();) {
            const ssize_t n = ::send(fd, response.data() + sent, response.size() - sent, 0);
            if (n <= 0) break;
            sent += static_cast<std::size_t>(n);
        }
        ::close(fd);
    }

    void stop() {
        if (thread_.joinable()) thread_.join();
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    mutable std::mutex mu_;
    std::string body_;
    std::thread thread_;
};

} // namespace

TEST_CASE("Cortex endpoint: the provisioned assisted-mode server is the default") {
    HttpInferenceProvider provider;
    CHECK(provider.endpoint() == "http://127.0.0.1:8100");
    CHECK(provider.model_name() == "cortex-mini");
    CHECK(provider.endpoint() != "http://127.0.0.1:8095");
}

TEST_CASE("Cortex endpoint: explicit config replaces host, port and model") {
    InferenceEndpointConfig cfg;
    cfg.port = 8123;
    cfg.model = "other-model";

    HttpInferenceProvider provider(cfg);
    CHECK(provider.endpoint() == "http://127.0.0.1:8123");
    CHECK(provider.model_name() == "other-model");

    // The service takes the same config and reports it through the
    // supervisor's snapshot, which is where the view reads it from.
    CortexService svc(cfg, nullptr, SupervisorConfig{});
    const SupervisorSnapshot snap = svc.supervisor().snapshot();
    CHECK(snap.endpoint == "http://127.0.0.1:8123");
    CHECK(snap.model_name == "other-model");
}

TEST_CASE("Cortex endpoint: the service builds the provisioned server from a config") {
    CortexService svc(InferenceEndpointConfig{});
    const SupervisorSnapshot snap = svc.supervisor().snapshot();
    CHECK(snap.endpoint == "http://127.0.0.1:8100");
    CHECK(snap.model_name == "cortex-mini");
    CHECK(snap.endpoint != "http://127.0.0.1:8095");
}

TEST_CASE("Cortex endpoint: HAILSTONE_CORTEX_* overrides, unset keeps the provisioned default") {
    ScopedEnv host("HAILSTONE_CORTEX_HOST");
    ScopedEnv port("HAILSTONE_CORTEX_PORT");
    ScopedEnv model("HAILSTONE_CORTEX_MODEL");
    ScopedEnv legacy_host("NMSC_CORTEX_HOST");
    ScopedEnv legacy_port("NMSC_CORTEX_PORT");
    ScopedEnv legacy_model("NMSC_CORTEX_MODEL");
    const std::vector<ScopedEnv*> all{&host, &port, &model, &legacy_host, &legacy_port, &legacy_model};
    clear_cortex_env(all);

    // Nothing set: the provisioned endpoint, whichever spelling is absent.
    const InferenceEndpointConfig plain = InferenceEndpointConfig::from_env();
    CHECK(plain.host == "127.0.0.1");
    CHECK(plain.port == 8100);
    CHECK(plain.model == "cortex-mini");

    // Explicitly configured: every field the caller names is used.
    host.set("127.0.0.1");
    port.set("8123");
    model.set("env-model");
    const InferenceEndpointConfig configured = InferenceEndpointConfig::from_env();
    CHECK(configured.host == "127.0.0.1");
    CHECK(configured.port == 8123);
    CHECK(configured.model == "env-model");
    CHECK(HttpInferenceProvider(configured).endpoint() == "http://127.0.0.1:8123");

    // A port that is not a port keeps the provisioned one instead of
    // producing an endpoint nothing listens on.
    port.set("not-a-port");
    CHECK(InferenceEndpointConfig::from_env().port == 8100);

    // Cleared again: back to the provisioned default.
    clear_cortex_env(all);
    const InferenceEndpointConfig restored = InferenceEndpointConfig::from_env();
    CHECK(restored.host == "127.0.0.1");
    CHECK(restored.port == 8100);
    CHECK(restored.model == "cortex-mini");
}

// The output budget is configuration, not a constant. The scale run showed
// 64 is not enough for the reasoning model to reach JSON, and a different
// model alias may want a different number, so the endpoint reads it from
// the environment exactly like its host, port and model.
TEST_CASE("Cortex endpoint: the output budget comes from configuration, not a hardcoded 64") {
    ScopedEnv budget("HAILSTONE_CORTEX_MAX_TOKENS");
    ScopedEnv legacy_budget("NMSC_CORTEX_MAX_TOKENS");
    const std::vector<ScopedEnv*> both{&budget, &legacy_budget};
    clear_cortex_env(both);

    // Nothing set: the budget the scale run got valid JSON from.
    CHECK(InferenceEndpointConfig::from_env().output_tokens == 512);

    // Explicitly configured: used as asked.
    budget.set("1024");
    CHECK(InferenceEndpointConfig::from_env().output_tokens == 1024);

    // Not a number, or not a positive token count: the default instead of
    // a request that asks for nothing.
    budget.set("not-a-budget");
    CHECK(InferenceEndpointConfig::from_env().output_tokens == 512);
    budget.set("0");
    CHECK(InferenceEndpointConfig::from_env().output_tokens == 512);

    clear_cortex_env(both);
    CHECK(InferenceEndpointConfig::from_env().output_tokens == 512);
}

// Configurable does not mean arbitrarily small: below the floor the model's
// thinking can eat the whole budget again and the reply arrives with no
// content (SCALE_TEST_REPORT.md finding #6).
TEST_CASE("Cortex endpoint: the provider raises a budget below the JSON floor") {
    InferenceEndpointConfig small;
    small.output_tokens = 8;
    CHECK(HttpInferenceProvider(small).output_tokens() >= 256);

    InferenceEndpointConfig raised;
    raised.output_tokens = 2048;
    CHECK(HttpInferenceProvider(raised).output_tokens() == 2048);
}

// The scale report's finding #6: against the live Qwen3 reasoning model a
// default HTTP request came back with `content = ""` - the 64-token budget
// the worker asked for was all spent on `reasoning_content` and the reply
// stopped on `finish_reason = "length"`. What the endpoint puts on the wire
// has to survive the model's thinking; changing the URL or the model alias
// does not address it.
TEST_CASE("Cortex endpoint: the wire request carries a budget the reasoning model cannot exhaust") {
    OneShotCompletionServer server;
    REQUIRE(server.start());

    HttpInferenceProvider provider("127.0.0.1", server.port());
    InferenceRequest req;
    req.system_prompt = "You are a decision service.";
    req.user_prompt = "nominal";
    req.max_tokens = 64; // the number the supervisor used to hardcode

    const InferenceResponse resp = provider.complete(req);
    INFO(resp.error);
    REQUIRE(resp.ok);

    const std::string body = server.request_body();
    REQUIRE_FALSE(body.empty());

    const std::size_t at = body.find("\"max_tokens\":");
    REQUIRE(at != std::string::npos);
    const long sent = std::strtol(body.c_str() + at + 13, nullptr, 10);

    // 256 tokens was the first budget the Python client got valid JSON at
    // (SCALE_TEST_REPORT.md #6), and the provisioned context gives each of
    // the 6 slots ~1365 tokens beside a ~400-token observation prompt.
    CHECK(sent >= 256);
    CHECK(sent > req.max_tokens);
    // Still the provisioned model and loopback endpoint: the budget fix does
    // not repoint the URL.
    CHECK(body.find("\"model\":\"cortex-mini\"") != std::string::npos);
    CHECK(provider.endpoint() == "http://127.0.0.1:" + std::to_string(server.port()));
}
