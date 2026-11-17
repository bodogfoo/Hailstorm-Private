// Hailstone 24 Multibox Switcher & Telemetry Manager.
// Connects to hailstone24_broker as a non-session controller (__manager__) via AF_UNIX IPC:
//   - --status / --switch / --cmd : fast scriptable CLI control
//   - --tui                        : interactive POSIX terminal dashboard (hotkeys + /bca,/bcaa,/rc prompt)
//   - --gui (default on desktop)   : lightweight SDL3 Renderer dashboard (zero Vulkan swapchain cost)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#if !defined(_WIN32)
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

#include "app/execution_state.h"
#include "ipc/session_bus.h"

using namespace hailstone;

namespace {

std::atomic<bool> g_quit{false};

void on_sig(int) {
    g_quit.store(true, std::memory_order_relaxed);
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto ca = static_cast<unsigned char>(a[i]);
        auto cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
    }
    return true;
}

std::string normalize_eq_cmd(std::string_view cmd) {
    cmd = trim(cmd);
    if (cmd.rfind("//", 0) == 0) return "/" + std::string(cmd.substr(2));
    return std::string(cmd);
}

bool dispatch_input_line(ipc::SessionBusClient& client, std::string_view line, std::string& status_msg) {
    line = trim(line);
    if (line.empty()) return true;

    const auto sp = line.find(' ');
    const std::string_view verb = sp == std::string_view::npos ? line : line.substr(0, sp);
    const std::string_view rest = sp == std::string_view::npos ? std::string_view{} : trim(line.substr(sp + 1));

    if (ieq(verb, "/switch") || ieq(verb, "switch")) {
        if (rest.empty()) {
            status_msg = "Usage: /switch <session_id|character>";
            return false;
        }
        client.request_switch_foreground(std::string(rest));
        status_msg = "Switched foreground -> " + std::string(rest);
        return true;
    }

    ipc::CommandPacket pkt;
    pkt.sender_char = "__manager__";
    if (ieq(verb, "/bcaa")) {
        pkt.scope = ipc::CommandScope::AllIncludingSelf;
        pkt.command = normalize_eq_cmd(rest);
    } else if (ieq(verb, "/bca")) {
        pkt.scope = ipc::CommandScope::AllExceptSelf;
        pkt.command = normalize_eq_cmd(rest);
    } else if (ieq(verb, "/bcaaz")) {
        pkt.scope = ipc::CommandScope::ZoneIncludingSelf;
        pkt.command = normalize_eq_cmd(rest);
    } else if (ieq(verb, "/bcz")) {
        pkt.scope = ipc::CommandScope::ZoneExceptSelf;
        pkt.command = normalize_eq_cmd(rest);
    } else if (ieq(verb, "/bcaag")) {
        pkt.scope = ipc::CommandScope::GroupIncludingSelf;
        pkt.command = normalize_eq_cmd(rest);
    } else if (ieq(verb, "/bcg")) {
        pkt.scope = ipc::CommandScope::GroupExceptSelf;
        pkt.command = normalize_eq_cmd(rest);
    } else if (ieq(verb, "/bct") || ieq(verb, "/rc") || ieq(verb, "/dex")) {
        const auto sp2 = rest.find(' ');
        if (sp2 == std::string_view::npos) {
            status_msg = "Usage: " + std::string(verb) + " <target> <command>";
            return false;
        }
        pkt.scope = ipc::CommandScope::TargetPeer;
        pkt.target = std::string(rest.substr(0, sp2));
        pkt.command = normalize_eq_cmd(rest.substr(sp2 + 1));
    } else if (ieq(verb, "/bc")) {
        const auto sp2 = rest.find(' ');
        if (sp2 == std::string_view::npos) {
            status_msg = "Usage: /bc <channel> <command>";
            return false;
        }
        pkt.scope = ipc::CommandScope::Channel;
        pkt.target = std::string(rest.substr(0, sp2));
        pkt.command = normalize_eq_cmd(rest.substr(sp2 + 1));
    } else {
        pkt.scope = ipc::CommandScope::AllIncludingSelf;
        pkt.command = normalize_eq_cmd(line);
    }

    if (pkt.command.empty()) {
        status_msg = "Empty command payload";
        return false;
    }
    const auto seq = client.send_command(std::move(pkt));
    status_msg = std::format("Dispatched seq={} cmd={}", seq, line);
    return true;
}

void sort_roster(std::vector<ipc::SessionSnapshot>& list) {
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        if (a.session_id != b.session_id) return a.session_id < b.session_id;
        return a.character_name < b.character_name;
    });
}

void print_status_table(std::vector<ipc::SessionSnapshot> roster) {
    sort_roster(roster);
    std::uint64_t rss = 0;
    std::uint64_t vram = 0;
    for (const auto& s : roster) {
        rss += s.rss_mib;
        vram += s.vram_mib;
    }
    std::printf("Hailstone 24 Sessions (%zu/24 connected) | Total RSS: %llu MiB | Est VRAM: %llu MiB\n",
                roster.size(), static_cast<unsigned long long>(rss), static_cast<unsigned long long>(vram));
    std::printf("%-10s %-14s %-7s %-18s %-12s %-11s %-10s %s\n",
                "SESSION", "CHARACTER", "STATE", "ZONE", "HP/MP/END", "RGMERCS", "RSS/VRAM", "SW(ms)");
    for (const auto& s : roster) {
        const char* st = (s.exec_state == app::ExecutionState::ForegroundFull) ? "FG"
                       : (s.exec_state == app::ExecutionState::Transitioning)  ? "TRANS"
                       : (s.exec_state == app::ExecutionState::BackgroundHeadless) ? "BG_HL"
                                                                                   : "DISC";
        std::printf("%-10s %-14s %-7s %-18s %3u/%3u/%3u  %-11s %4u/%4uM %u\n",
                    s.session_id.c_str(),
                    s.character_name.empty() ? "-" : s.character_name.c_str(),
                    st,
                    s.zone_short.empty() ? "-" : s.zone_short.c_str(),
                    s.hp_pct, s.mana_pct, s.end_pct,
                    s.rgmercs_state.empty() ? "-" : s.rgmercs_state.c_str(),
                    s.rss_mib, s.vram_mib, s.last_switch_ms);
    }
}

#if !defined(_WIN32)
struct TermRawGuard {
    termios orig{};
    bool active = false;
    TermRawGuard() {
        if (::isatty(STDIN_FILENO) && ::tcgetattr(STDIN_FILENO, &orig) == 0) {
            termios raw = orig;
            raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON));
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) active = true;
        }
    }
    ~TermRawGuard() {
        if (active) ::tcsetattr(STDIN_FILENO, TCSANOW, &orig);
        std::printf("\033[?25h\033[?1049l");
        std::fflush(stdout);
    }
};

int run_tui(ipc::SessionBusClient& client) {
    TermRawGuard guard;
    std::printf("\033[?1049h\033[?25l");
    std::size_t selected = 0;
    bool typing_cmd = false;
    std::string cmd_buf;
    std::string status = "Ready. Keys: 1-9 switch slot | j/k or Up/Down select | Enter switch | / command | r refresh | q quit";
    auto last_draw = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    auto last_poll_req = std::chrono::steady_clock::now();

    while (!g_quit.load(std::memory_order_relaxed)) {
        client.poll();
        const auto now = std::chrono::steady_clock::now();
        if (now - last_poll_req >= std::chrono::milliseconds(500)) {
            client.request_roster();
            last_poll_req = now;
        }

        auto roster = client.cached_roster();
        sort_roster(roster);
        if (!roster.empty() && selected >= roster.size()) selected = roster.size() - 1;

        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        if (::poll(&pfd, 1, 30) > 0 && (pfd.revents & POLLIN)) {
            char buf[64]{};
            const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            for (ssize_t i = 0; i < n; ++i) {
                const char ch = buf[i];
                if (typing_cmd) {
                    if (ch == '\r' || ch == '\n') {
                        dispatch_input_line(client, cmd_buf, status);
                        client.poll();
                        cmd_buf.clear();
                        typing_cmd = false;
                    } else if (ch == 27) {
                        cmd_buf.clear();
                        typing_cmd = false;
                        status = "Cancelled command.";
                    } else if (ch == 127 || ch == '\b') {
                        if (!cmd_buf.empty()) cmd_buf.pop_back();
                    } else if (ch >= 32 && ch < 127) {
                        cmd_buf.push_back(ch);
                    }
                    last_draw = std::chrono::steady_clock::time_point{};
                    continue;
                }

                if (ch == 'q' || ch == 'Q') {
                    g_quit.store(true, std::memory_order_relaxed);
                    break;
                } else if (ch == 'r' || ch == 'R') {
                    client.request_roster();
                    status = "Requested roster refresh.";
                } else if (ch == '/' || ch == ':') {
                    typing_cmd = true;
                    cmd_buf = (ch == '/') ? "/" : "";
                } else if (ch == 'j' || ch == 'J') {
                    if (!roster.empty()) selected = (selected + 1) % roster.size();
                } else if (ch == 'k' || ch == 'K') {
                    if (!roster.empty()) selected = (selected + roster.size() - 1) % roster.size();
                } else if (ch == '\r' || ch == '\n') {
                    if (selected < roster.size()) {
                        client.request_switch_foreground(roster[selected].session_id);
                        status = "Switched foreground -> " + roster[selected].session_id;
                    }
                } else if (ch >= '1' && ch <= '9') {
                    const std::size_t idx = static_cast<std::size_t>(ch - '1');
                    if (idx < roster.size()) {
                        selected = idx;
                        client.request_switch_foreground(roster[idx].session_id);
                        status = "Switched foreground -> " + roster[idx].session_id;
                    }
                } else if (ch == 27 && i + 2 < n && buf[i + 1] == '[') {
                    if (buf[i + 2] == 'A' && !roster.empty()) selected = (selected + roster.size() - 1) % roster.size();
                    else if (buf[i + 2] == 'B' && !roster.empty()) selected = (selected + 1) % roster.size();
                    i += 2;
                }
                last_draw = std::chrono::steady_clock::time_point{};
            }
        }

        if (now - last_draw >= std::chrono::milliseconds(150)) {
            last_draw = now;
            std::uint64_t rss = 0, vram = 0;
            std::string fg = "none";
            for (const auto& s : roster) {
                rss += s.rss_mib;
                vram += s.vram_mib;
                if (s.exec_state == app::ExecutionState::ForegroundFull) {
                    fg = s.character_name.empty() ? s.session_id : s.character_name;
                }
            }
            std::printf("\033[H\033[2J");
            std::printf("\033[1;36mHAILSTONE 24 MANAGER\033[0m | Bus: %s | Sessions: %zu/24 | FG: \033[1;32m%s\033[0m | RSS: %llu MiB | VRAM: %llu MiB\n",
                        client.connected() ? "CONNECTED" : "RECONNECTING",
                        roster.size(), fg.c_str(),
                        static_cast<unsigned long long>(rss),
                        static_cast<unsigned long long>(vram));
            std::printf("---------------------------------------------------------------------------------------------------------\n");
            std::printf("%-3s %-10s %-14s %-6s %-16s %-14s %-11s %-10s %-9s %s\n",
                        "ID", "SESSION", "CHARACTER", "STATE", "ZONE", "TARGET", "HP/MP/END", "RGMERCS", "RSS/VRAM", "SW(ms)");
            for (std::size_t i = 0; i < roster.size(); ++i) {
                const auto& s = roster[i];
                const bool is_sel = (i == selected);
                const bool is_fg = (s.exec_state == app::ExecutionState::ForegroundFull);
                const char* st = is_fg ? "FG"
                               : (s.exec_state == app::ExecutionState::Transitioning) ? "TRANS"
                               : (s.exec_state == app::ExecutionState::BackgroundHeadless) ? "BG_HL" : "DISC";
                if (is_sel) std::printf("\033[7m");
                else if (is_fg) std::printf("\033[1;32m");
                std::printf("%2zu  %-10s %-14s %-6s %-16s %-14s %3u/%3u/%3u %-10s %4u/%4u %4u\033[0m\n",
                            i + 1,
                            s.session_id.c_str(),
                            s.character_name.empty() ? "-" : s.character_name.c_str(),
                            st,
                            s.zone_short.empty() ? "-" : s.zone_short.c_str(),
                            s.target_name.empty() ? "-" : s.target_name.substr(0, 14).c_str(),
                            s.hp_pct, s.mana_pct, s.end_pct,
                            s.rgmercs_state.empty() ? "-" : s.rgmercs_state.c_str(),
                            s.rss_mib, s.vram_mib, s.last_switch_ms);
            }
            if (roster.empty()) {
                std::printf("  (Waiting for Hailstone 24 sessions on IPC bus...)\n");
            }
            std::printf("---------------------------------------------------------------------------------------------------------\n");
            if (typing_cmd) {
                std::printf("CMD> %s_\n", cmd_buf.c_str());
            } else {
                std::printf("%s\n", status.c_str());
            }
            std::fflush(stdout);
        }
    }
    return 0;
}
#endif

int run_sdl_gui(ipc::SessionBusClient& client) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "[manager] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    if (!SDL_CreateWindowAndRenderer("Hailstone 24 Control Deck", 1120, 740, SDL_WINDOW_RESIZABLE, &win, &ren)) {
        std::fprintf(stderr, "[manager] SDL_CreateWindowAndRenderer failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_StartTextInput(win);

    std::string cmd_buf = "/bca /rc start";
    std::string status_line = "Click any slot card to switch FOREGROUND_FULL (<250ms). Type /bca, /bcaa, /rc or /switch below.";
    auto last_req = std::chrono::steady_clock::now();

    bool running = true;
    while (running && !g_quit.load(std::memory_order_relaxed)) {
        client.poll();
        const auto now = std::chrono::steady_clock::now();
        if (now - last_req >= std::chrono::milliseconds(400)) {
            client.request_roster();
            last_req = now;
        }

        auto roster = client.cached_roster();
        sort_roster(roster);

        int win_w = 1120, win_h = 740;
        SDL_GetWindowSize(win, &win_w, &win_h);
        const float pad = 12.0f;
        const float top_h = 56.0f;
        const float bar_h = 68.0f;
        const int cols = 4;
        const int rows = 6;
        const float cell_w = std::max(140.0f, (static_cast<float>(win_w) - pad * (cols + 1)) / cols);
        const float cell_h = std::max(64.0f, (static_cast<float>(win_h) - top_h - bar_h - pad * (rows + 1)) / rows);

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (ev.type == SDL_EVENT_TEXT_INPUT) {
                cmd_buf += ev.text.text;
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.key == SDLK_ESCAPE) {
                    running = false;
                } else if (ev.key.key == SDLK_BACKSPACE) {
                    if (!cmd_buf.empty()) cmd_buf.pop_back();
                } else if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER) {
                    dispatch_input_line(client, cmd_buf, status_line);
                    cmd_buf.clear();
                } else if (ev.key.key == SDLK_TAB && !roster.empty()) {
                    std::size_t cur = 0;
                    for (std::size_t i = 0; i < roster.size(); ++i) {
                        if (roster[i].exec_state == app::ExecutionState::ForegroundFull) {
                            cur = i;
                            break;
                        }
                    }
                    const std::size_t next = (cur + 1) % roster.size();
                    client.request_switch_foreground(roster[next].session_id);
                    status_line = "Tab-switched foreground -> " + roster[next].session_id;
                }
            } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
                const float mx = ev.button.x;
                const float my = ev.button.y;
                if (my >= top_h && my < static_cast<float>(win_h) - bar_h) {
                    const int col = static_cast<int>((mx - pad) / (cell_w + pad));
                    const int row = static_cast<int>((my - top_h) / (cell_h + pad));
                    if (col >= 0 && col < cols && row >= 0 && row < rows) {
                        const std::size_t idx = static_cast<std::size_t>(row * cols + col);
                        if (idx < roster.size()) {
                            client.request_switch_foreground(roster[idx].session_id);
                            status_line = "Switched foreground -> " + roster[idx].session_id;
                        }
                    }
                }
            }
        }

        std::uint64_t rss = 0, vram = 0;
        std::string fg = "none";
        for (const auto& s : roster) {
            rss += s.rss_mib;
            vram += s.vram_mib;
            if (s.exec_state == app::ExecutionState::ForegroundFull) {
                fg = s.character_name.empty() ? s.session_id : s.character_name;
            }
        }

        SDL_SetRenderDrawColor(ren, 14, 17, 23, 255);
        SDL_RenderClear(ren);

        SDL_FRect hdr{0.0f, 0.0f, static_cast<float>(win_w), top_h - 6.0f};
        SDL_SetRenderDrawColor(ren, 22, 27, 36, 255);
        SDL_RenderFillRect(ren, &hdr);
        SDL_SetRenderDrawColor(ren, 45, 212, 191, 255);
        SDL_RenderDebugText(ren, 14.0f, 12.0f,
                            std::format("HAILSTONE 24 CONTROL DECK | BUS: {} | SESSIONS: {}/24 | FOREGROUND: {}",
                                        client.connected() ? "LIVE" : "WAIT", roster.size(), fg).c_str());
        SDL_SetRenderDrawColor(ren, 170, 185, 204, 255);
        SDL_RenderDebugText(ren, 14.0f, 30.0f,
                            std::format("Total RSS: {} MiB / 49152 MiB | Est VRAM: {} MiB / 32768 MiB (Intel Arc Pro B70)",
                                        rss, vram).c_str());

        for (std::size_t idx = 0; idx < 24; ++idx) {
            const auto r = static_cast<float>(idx / static_cast<std::size_t>(cols));
            const auto c = static_cast<float>(idx % static_cast<std::size_t>(cols));
            SDL_FRect box{pad + c * (cell_w + pad), top_h + r * (cell_h + pad), cell_w, cell_h};
            if (idx < roster.size()) {
                const auto& s = roster[idx];
                const bool is_fg = (s.exec_state == app::ExecutionState::ForegroundFull);
                if (is_fg) SDL_SetRenderDrawColor(ren, 20, 48, 46, 255);
                else SDL_SetRenderDrawColor(ren, 24, 29, 39, 255);
                SDL_RenderFillRect(ren, &box);
                SDL_SetRenderDrawColor(ren, is_fg ? 45 : 58, is_fg ? 212 : 70, is_fg ? 191 : 90, 255);
                SDL_RenderRect(ren, &box);

                const char* st = is_fg ? "FG_FULL" : "BG_HEADLESS";
                SDL_SetRenderDrawColor(ren, 235, 242, 250, 255);
                SDL_RenderDebugText(ren, box.x + 8.0f, box.y + 8.0f,
                                    std::format("[{:02}] {}", idx + 1,
                                                s.character_name.empty() ? s.session_id : s.character_name).c_str());
                SDL_SetRenderDrawColor(ren, is_fg ? 94 : 148, is_fg ? 234 : 163, is_fg ? 212 : 184, 255);
                SDL_RenderDebugText(ren, box.x + 8.0f, box.y + 22.0f,
                                    std::format("{} | {} | {}m/{}m", st,
                                                s.zone_short.empty() ? "login" : s.zone_short,
                                                s.rss_mib, s.vram_mib).c_str());
                SDL_SetRenderDrawColor(ren, 180, 196, 214, 255);
                SDL_RenderDebugText(ren, box.x + 8.0f, box.y + 36.0f,
                                    std::format("RG:{} SW:{}ms",
                                                s.rgmercs_state.empty() ? "-" : s.rgmercs_state,
                                                s.last_switch_ms).c_str());

                const float bar_w = box.w - 16.0f;
                SDL_FRect hp_bg{box.x + 8.0f, box.y + box.h - 14.0f, bar_w, 4.0f};
                SDL_FRect hp_fg{box.x + 8.0f, box.y + box.h - 14.0f,
                                bar_w * (static_cast<float>(std::min<std::uint8_t>(s.hp_pct, 100)) / 100.0f), 4.0f};
                SDL_SetRenderDrawColor(ren, 45, 22, 26, 255);
                SDL_RenderFillRect(ren, &hp_bg);
                SDL_SetRenderDrawColor(ren, 34, 197, 94, 255);
                SDL_RenderFillRect(ren, &hp_fg);

                SDL_FRect mp_bg{box.x + 8.0f, box.y + box.h - 8.0f, bar_w, 4.0f};
                SDL_FRect mp_fg{box.x + 8.0f, box.y + box.h - 8.0f,
                                bar_w * (static_cast<float>(std::min<std::uint8_t>(s.mana_pct, 100)) / 100.0f), 4.0f};
                SDL_SetRenderDrawColor(ren, 20, 32, 56, 255);
                SDL_RenderFillRect(ren, &mp_bg);
                SDL_SetRenderDrawColor(ren, 59, 130, 246, 255);
                SDL_RenderFillRect(ren, &mp_fg);
            } else {
                SDL_SetRenderDrawColor(ren, 18, 22, 29, 255);
                SDL_RenderFillRect(ren, &box);
                SDL_SetRenderDrawColor(ren, 40, 48, 62, 255);
                SDL_RenderRect(ren, &box);
                SDL_SetRenderDrawColor(ren, 82, 96, 115, 255);
                SDL_RenderDebugText(ren, box.x + 8.0f, box.y + 8.0f,
                                    std::format("[{:02}] Offline Slot", idx + 1).c_str());
            }
        }

        SDL_FRect bottom{0.0f, static_cast<float>(win_h) - bar_h, static_cast<float>(win_w), bar_h};
        SDL_SetRenderDrawColor(ren, 20, 24, 33, 255);
        SDL_RenderFillRect(ren, &bottom);
        SDL_SetRenderDrawColor(ren, 148, 163, 184, 255);
        SDL_RenderDebugText(ren, 14.0f, bottom.y + 10.0f, status_line.c_str());
        SDL_FRect input_box{14.0f, bottom.y + 28.0f, static_cast<float>(win_w) - 28.0f, 28.0f};
        SDL_SetRenderDrawColor(ren, 12, 15, 20, 255);
        SDL_RenderFillRect(ren, &input_box);
        SDL_SetRenderDrawColor(ren, 45, 212, 191, 255);
        SDL_RenderRect(ren, &input_box);
        SDL_SetRenderDrawColor(ren, 240, 246, 252, 255);
        SDL_RenderDebugText(ren, input_box.x + 8.0f, input_box.y + 10.0f, ("> " + cmd_buf + "_").c_str());

        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    SDL_StopTextInput(win);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

void usage() {
    std::printf(
        "usage: hailstone24_manager [options]\n"
        "  --socket PATH      local AF_UNIX broker socket\n"
        "  --status           print 24-session status table and exit\n"
        "  --switch TARGET    switch FOREGROUND_FULL to session_id or character name and exit\n"
        "  --cmd LINE         dispatch /bca, /bcaa, /bcz, /bcg, /bc, /rc, or /switch and exit\n"
        "  --tui              run interactive terminal dashboard (default if no display server)\n"
        "  --gui              run lightweight SDL3 control window (default on desktop)\n"
        "  -h, --help         show this help\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string socket_path = ipc::default_bus_socket_path();
    bool do_status = false;
    bool force_tui = false;
    bool force_gui = false;
    std::string switch_target;
    std::string one_shot_cmd;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto need = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for %s\n", argv[i]);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--socket") socket_path = need();
        else if (a == "--status") do_status = true;
        else if (a == "--tui") force_tui = true;
        else if (a == "--gui") force_gui = true;
        else if (a == "--switch") switch_target = need();
        else if (a == "--cmd") one_shot_cmd = need();
        else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);

    ipc::SessionBusClient client;
    if (!client.connect(socket_path, "__manager__")) {
        if (do_status || !switch_target.empty() || !one_shot_cmd.empty()) {
            std::fprintf(stderr, "[manager] Unable to connect to broker socket: %s\n", socket_path.c_str());
            return 1;
        }
    }

    if (do_status || !switch_target.empty() || !one_shot_cmd.empty()) {
        client.request_roster();
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (std::chrono::steady_clock::now() < until) {
            client.poll();
            if (!client.cached_roster().empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (!switch_target.empty()) {
            client.request_switch_foreground(switch_target);
            client.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            client.poll();
            std::printf("[manager] Requested foreground switch -> %s\n", switch_target.c_str());
            return 0;
        }
        if (!one_shot_cmd.empty()) {
            std::string msg;
            const bool ok = dispatch_input_line(client, one_shot_cmd, msg);
            client.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            client.poll();
            std::printf("[manager] %s\n", msg.c_str());
            return ok ? 0 : 1;
        }
        print_status_table(client.cached_roster());
        return 0;
    }

#if !defined(_WIN32)
    const bool has_display = (std::getenv("DISPLAY") && *std::getenv("DISPLAY")) ||
                             (std::getenv("WAYLAND_DISPLAY") && *std::getenv("WAYLAND_DISPLAY"));
    if (force_tui || (!force_gui && !has_display)) {
        return run_tui(client);
    }
#endif
    return run_sdl_gui(client);
}
