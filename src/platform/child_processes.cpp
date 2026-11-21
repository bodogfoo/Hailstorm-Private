#include "platform/child_processes.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_process.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

namespace hailstone::platform {

namespace {

bool tag_equals_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

} // namespace

ChildProcesses::~ChildProcesses() {
    for (const Entry& e : children_) {
        if (e.proc) SDL_DestroyProcess(e.proc); // the SDL object only, not the program
    }
}

bool ChildProcesses::start(const std::vector<std::string>& args, std::string& error) {
    return start_tagged({}, args, error);
}

bool ChildProcesses::start_tagged(std::string tag, const std::vector<std::string>& args, std::string& error) {
    if (args.empty()) {
        error = "empty argument list";
        return false;
    }
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_Process* p = SDL_CreateProcess(argv.data(), false);
    if (!p) {
        error = SDL_GetError();
        return false;
    }
    children_.push_back(Entry{std::move(tag), p});
    return true;
}

void ChildProcesses::poll() {
    std::erase_if(children_, [](const Entry& e) {
        if (!e.proc) return true;
        if (!SDL_WaitProcess(e.proc, false, nullptr)) return false;
        SDL_DestroyProcess(e.proc);
        return true;
    });
}

bool ChildProcesses::is_tag_running(std::string_view tag) const {
    if (tag.empty()) return false;
    for (const Entry& e : children_) {
        if (!e.proc) continue;
        if (tag_equals_ci(e.tag, tag)) return true;
    }
    return false;
}

void ChildProcesses::stop_all() {
    poll();
    for (const Entry& e : children_) {
        if (e.proc) (void)SDL_KillProcess(e.proc, false);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    while (!children_.empty() && std::chrono::steady_clock::now() < deadline) {
        poll();
        if (children_.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    for (const Entry& e : children_) {
        if (e.proc) {
            (void)SDL_KillProcess(e.proc, true);
            (void)SDL_WaitProcess(e.proc, true, nullptr);
            SDL_DestroyProcess(e.proc);
        }
    }
    children_.clear();
}

} // namespace hailstone::platform
