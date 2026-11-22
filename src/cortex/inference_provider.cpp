#include "cortex/inference_provider.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <sstream>

namespace hailstone::cortex {

namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

// Minimal JSON string field extractor for OpenAI chat/completions response.
std::string extract_json_string_field(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    ++pos;
    std::string out;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '\\' && pos < json.size()) {
            char next = json[pos++];
            switch (next) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            default: out += next; break;
            }
        } else if (c == '"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace

MockInferenceProvider::MockInferenceProvider(std::string model)
    : model_(std::move(model)) {}

InferenceResponse MockInferenceProvider::complete(const InferenceRequest& req) {
    InferenceResponse resp;
    if (!healthy_.load()) {
        resp.ok = false;
        resp.error = "mock provider unhealthy";
        return resp;
    }
    if (req.cancelled && req.cancelled->load()) {
        resp.ok = false;
        resp.error = "cancelled";
        return resp;
    }
    if (!forced_response_.empty()) {
        resp.ok = true;
        resp.content = forced_response_;
        resp.latency_ms = 1;
        return resp;
    }
    // Deterministic tactical heuristic for tests.
    if (req.user_prompt.find("mana_pct=1") != std::string::npos ||
        req.user_prompt.find("mana_pct=0") != std::string::npos ||
        req.user_prompt.find("healer_mana_low") != std::string::npos) {
        resp.content = R"({"action":"hold_pull","reason":"healer_mana_low"})";
    } else if (req.user_prompt.find("hp_pct=1") != std::string::npos ||
               req.user_prompt.find("hp_pct=2") != std::string::npos) {
        resp.content = R"({"action":"recover_resources","reason":"low_hp"})";
    } else {
        resp.content = R"({"action":"continue_current_task","reason":"nominal"})";
    }
    resp.ok = true;
    resp.latency_ms = 1;
    resp.prompt_tokens = 64;
    resp.completion_tokens = 14;
    return resp;
}

HttpInferenceProvider::HttpInferenceProvider(std::string host,
                                             std::uint16_t port,
                                             std::string model)
    : host_(std::move(host)), port_(port), model_(std::move(model)) {}

std::string HttpInferenceProvider::endpoint() const {
    return "http://" + host_ + ":" + std::to_string(port_);
}

void HttpInferenceProvider::set_endpoint(std::string host, std::uint16_t port, std::string model) {
    host_ = std::move(host);
    port_ = port;
    model_ = std::move(model);
}

bool HttpInferenceProvider::health_check() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        ::close(fd);
        return false;
    }
    bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    ::close(fd);
    return ok;
}

InferenceResponse HttpInferenceProvider::complete(const InferenceRequest& req) {
    InferenceResponse resp;
    auto t0 = std::chrono::steady_clock::now();

    if (req.cancelled && req.cancelled->load()) {
        resp.error = "cancelled";
        return resp;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        resp.error = "socket() failed";
        return resp;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        ::close(fd);
        resp.error = "invalid host";
        return resp;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        resp.error = "connect failed";
        return resp;
    }

    std::ostringstream body;
    body << "{\"model\":\"" << json_escape(model_) << "\","
         << "\"max_tokens\":" << req.max_tokens << ","
         << "\"temperature\":0.1,"
         << "\"messages\":["
         << "{\"role\":\"system\",\"content\":\"" << json_escape(req.system_prompt) << "\"},"
         << "{\"role\":\"user\",\"content\":\"" << json_escape(req.user_prompt) << "\"}"
         << "]}";
    std::string body_str = body.str();

    std::ostringstream http;
    http << "POST /v1/chat/completions HTTP/1.1\r\n"
         << "Host: " << host_ << ":" << port_ << "\r\n"
         << "Content-Type: application/json\r\n"
         << "Content-Length: " << body_str.size() << "\r\n"
         << "Connection: close\r\n\r\n"
         << body_str;
    std::string req_str = http.str();

    std::size_t sent = 0;
    while (sent < req_str.size()) {
        ssize_t n = ::send(fd, req_str.data() + sent, req_str.size() - sent, 0);
        if (n <= 0) {
            ::close(fd);
            resp.error = "send failed";
            return resp;
        }
        sent += static_cast<std::size_t>(n);
    }

    std::string raw;
    char buf[4096];
    while (true) {
        if (req.cancelled && req.cancelled->load()) {
            ::close(fd);
            resp.error = "cancelled";
            return resp;
        }
        pollfd pfd{fd, POLLIN, 0};
        int pr = ::poll(&pfd, 1, req.timeout_ms);
        if (pr == 0) {
            ::close(fd);
            resp.error = "timeout";
            return resp;
        }
        if (pr < 0) {
            ::close(fd);
            resp.error = "poll error";
            return resp;
        }
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);

    auto t1 = std::chrono::steady_clock::now();
    resp.latency_ms = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

    auto hdr_end = raw.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
        resp.error = "malformed HTTP response";
        return resp;
    }
    std::string json_body = raw.substr(hdr_end + 4);
    resp.content = extract_json_string_field(json_body, "content");
    if (resp.content.empty()) {
        resp.error = "empty model content";
        return resp;
    }
    resp.ok = true;
    return resp;
}

} // namespace hailstone::cortex
