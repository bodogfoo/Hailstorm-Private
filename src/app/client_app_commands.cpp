// ClientApp: the original's slash commands that act on the client rather than the connection (the rules
// are game/option_commands.h): the windows (/filter, /note, /map, /loadskin), the
// chat windows (/chatfontsize, /clearchat, /clearallchat, /usercolor), hot buttons (/hotbutton), and the display,
// sound and mouse settings, which go to hailstone.ini as the Options window's do (the original's eqclient.ini
// [Defaults] / [Options]). GameConnection::run_slash_command hands them here (set_client_commands).
//

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>
#include <limits>
#include <sstream>

#include <imgui.h>

#include "app/chat_tabs_command.h"
#include "app/client_app_internal.h"
#include "app/lua_ui.h"
#include "app/lua_ui_windows.h"
#include "core/group_profile.h"
#include "core/msvc_atoi.h"
#include "game/chat_windows.h"
#include "game/hotbuttons.h"
#include "game/option_commands.h"
#include "game/slash_commands.h"
#include "game/xtarget.h"
#include "mq/rgmercs_bridge.h"
#include "platform/paths.h"

namespace hailstone::app {

namespace client_internal {

// Cmd_xtarget @0x4e4780 (the usage: Cmd_xtarget_PrintUsage @0x4e3530). Up to three
// words (GetNextCmdArg). A first word starting with a digit targets that slot (1-based); "show" (any prefix of it)
// shows the window with "on", hides it with "off", else toggles it; "target <n>" targets slot n; "add" puts the
// target (a player 2, an NPC 3, by name) in the first auto hater slot; "remove" turns the last specific slot back
// into an auto hater, "remove target" every specific slot holding the target; "set <n> <type or name>" sets slot n
// to a type (XTarget_ParseTargetType; CurrentTarget takes the target) or to an existing player or NPC by that
// name; "auto" (any prefix) sets the auto add flag ("on", "off", else toggled), which the window stores.
void ClientApp::xtarget_command(const std::string& args) {
    net::GameConnection& c = *conn_;
    const std::uint32_t expansions = c.profile() ? c.profile()->expansions : 0;
    if (!(expansions & 0x8000)) {
        c.show_chat("In order to gain access to this feature, you must purchase the EverQuest expansion: Underfoot.");
        return;
    }
    auto usage = [&] {
        static constexpr std::pair<const char*, int> kUsage[] = {
            {"/xtarget USAGE:", 15},
            {"/xtarget show <on/off>", 15},
            {" /xtarget show is used to toggle the visiblity of the window", 15},
            {"\t---example: /xtarget show on", 15},
            {"", -1},
            {"/xtarget set <slot> <role>", 15},
            {"OR: /xtarget set <slot> <player_name>", 15},
            {" /xtarget set is used to assign a role, player, or NPC to a specific slot", 15},
            {"\t---example: /xtarget set 3 grouptank ", 15},
            {"\t---example: /xtarget set 3 Lorudan ", 15},
            {"", -1},
            {"/xtarget target <slot>", 15},
            {" /xtarget target is used to select a target at a specific slot", 15},
            {"\t---example: /xtarget target 7", 15},
            {"\t---shortcut example: /xtarget 7", 15},
            {"", -1},
            {"/xtarget auto <on/off>", 15},
            {" /xtarget auto is used to toggle auto-hater-targeting", 15},
            {"\t---example: /xtarget auto on", 15},
            {"", -1},
            {"/xtarget add", 15},
            {" /xtarget add will add current target to your next available slot", 15},
            {"", -1},
            {"/xtarget remove", 15},
            {" /xtarget remove will remove the last added target", 15},
            {"\t---example: /xtarget remove", 15},
            {" /xtarget remove target will remove targeted player from slots not using rolls (specific PC/NPC slots)", 15},
            {"\t---example: /xtarget remove target", 15},
        };
        for (auto [text, color] : kUsage) c.show_chat(text, color < 0 ? 0u : static_cast<std::uint32_t>(color));
    };
    if (args.empty()) {
        c.show_chat("You must enter a valid Extended Target command.", 0xd);
        usage();
        return;
    }
    std::string word[3];
    std::string_view rest = args;
    for (auto& w : word) {
        w = game::next_cmd_arg(rest).substr(0, 0x7f);
        if (w.empty()) break;
    }
    auto prefix = [](const std::string& w, std::string_view of, std::size_t n) { // strnicmp(w, of, n) == 0
        for (std::size_t i = 0; i < n; ++i) {
            char a = i < w.size() ? w[i] : 0, b = i < of.size() ? of[i] : 0;
            if (std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(b))) return false;
            if (!a) break;
        }
        return true;
    };
    auto digit = [](const std::string& w) { return !w.empty() && w[0] >= '0' && w[0] <= '9'; }; // Utf8IsDigit
    const game::XTargetState& list = c.xtargets();
    const int count = static_cast<int>(list.slots.size());
    auto range = [&] {
        c.show_chat(std::format("Slot_number must be between 1 and {} (your current maximum number of slots).", count));
    };
    auto target_slot = [&](const std::string& w) {
        // In 64 bits: "2147483648" is INT_MIN, and INT_MIN - 1 overflowed.
        const std::int64_t n = std::int64_t{core::msvc_atoi(w)} - 1;
        if (n < 0 || n >= count) return range();
        c.target_xtarget(static_cast<int>(n));
    };
    auto incorrect = [&] { c.show_chat("Incorrect Usage. Type /xtarget for correct usage.", 0xd); };
    const game::Spawn* target = nullptr;
    if (auto it = c.world().spawns.find(c.world().target_id); it != c.world().spawns.end()) target = &it->second;
    // A player 2, an NPC 3; corpses none.
    auto specific = [](const game::Spawn& s) -> std::optional<std::uint32_t> {
        if (s.is_corpse) return std::nullopt;
        return s.is_npc ? game::kXTargetSpecificNpc : game::kXTargetSpecificPc;
    };
    if (digit(word[0])) return target_slot(word[0]);
    if (prefix(word[0], "show", word[0].size())) {
        if (!ui_api_) return;
        if (prefix(word[1], "off", 3)) ui_api_->show("ExtendedTargetWnd", false);
        else if (prefix(word[1], "on", 2)) ui_api_->show("ExtendedTargetWnd", true);
        else ui_api_->toggle("ExtendedTargetWnd");
        return;
    }
    if (prefix(word[0], "target", 6)) {
        if (!digit(word[1])) return incorrect();
        return target_slot(word[1]);
    }
    if (prefix(word[0], "add", 3)) {
        if (!target || target->name.empty()) return;
        int slot = game::xtarget_first_auto_hater_slot(list, [&c](int i) { return c.xtarget_slot_hidden(i); });
        auto type = specific(*target);
        if (slot < 0 || !type) return;
        c.xtarget_request(slot, *type, target->name);
        return;
    }
    if (prefix(word[0], "remove", 6)) {
        if (prefix(word[1], "target", 6)) c.remove_target_xtargets();
        else c.remove_last_xtarget();
        return;
    }
    if (prefix(word[0], "set", 3)) {
        if (!digit(word[1])) return incorrect();
        const std::int64_t wide = std::int64_t{core::msvc_atoi(word[1])} - 1;
        if (wide < 0) return incorrect();
        const int slot = static_cast<int>(wide);
        std::uint32_t type = 0;
        std::string name;
        if (auto parsed = game::parse_xtarget_type(word[2])) {
            type = *parsed;
            if (type == game::kXTargetSpecificPc) { // CurrentTarget
                if (!target || target->name.empty()) return;
                auto t = specific(*target);
                if (!t) return;
                type = *t;
                name = target->name;
            }
        } else {
            // PlayerManagerClient::GetSpawnByName: the name as typed is sent.
            const game::Spawn* who = nullptr;
            for (const auto& [id, s] : c.world().spawns)
                if (game::same_no_case(s.name, word[2])) {
                    who = &s;
                    break;
                }
            if (!who) {
                c.show_chat("That is neither a valid slot type nor an existing player or NPC name.", 0xd);
                return incorrect();
            }
            auto t = specific(*who);
            if (!t) return;
            type = *t;
            name = word[2];
        }
        if (slot >= count) return range();
        c.xtarget_request(slot, type, name);
        return;
    }
    if (prefix(word[0], "auto", word[0].size())) {
        bool on = prefix(word[1], "off", 3) ? false : prefix(word[1], "on", 2) ? true : !list.auto_haters;
        c.set_xtarget_auto_haters(on); // the window stores its settings (StoreIniInfo) when it sees the change
        return;
    }
    incorrect();
}

bool ClientApp::client_command(std::string_view h, const std::string& args) {
    if (!conn_) return false;
    net::GameConnection& c = *conn_;
    auto eqstr = [&](std::uint32_t id) { return c.strings().format(id, {}); };
    auto save = [&] { config_.save(config_path_); };
    const std::string off = eqstr(12946), on = eqstr(12947);

    // ---- windows
    if (h == "Hailstone_abilitybook") {
        // The Ability Book (game/ability_book.h): no argument shows or hides it; "auto" toggles autoskill, "auto on"
        // / "auto off" set it.
        if (!ui_api_) return true;
        std::string_view rest = args;
        std::string word = game::next_cmd_arg(rest);
        for (char& ch : word) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (word.empty()) {
            ui_api_->show("AbilityBookWnd", !ui_api_->shown("AbilityBookWnd"));
        } else if (word == "auto") {
            std::string to = game::next_cmd_arg(rest);
            for (char& ch : to) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            ui_api_->click_child("AbilityBookWnd", to == "on" ? "AutoskillOn" : to == "off" ? "AutoskillOff" : "AutoskillToggle");
        } else {
            c.show_chat("Usage: /abilitybook [auto [on|off]]", 15);
        }
        return true;
    }
    if (h == "Hailstone_camera") {
        camera_command(args);
        return true;
    }
    if (h == "Hailstone_combat") {
        run_slash_combat(args);
        return true;
    }
    if (h == "Hailstone_estop") {
        std::string msg;
        handle_combat_command("estop", {}, msg);
        c.show_chat(msg.empty() ? "Emergency stop activated." : msg, 0xd);
        return true;
    }
    if (h == "Hailstone_finditem") {
        // The Advanced Inventory (/finditem, /advi): no argument shows or hides it; text opens it searching for that
        // (its filters cleared, so the search covers everything the character has).
        if (!ui_api_) return true;
        std::string text = args;
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
        if (text.empty()) {
            ui_api_->toggle("InventoryManagerWnd");
        } else {
            ui_host_.item_search = text;
            ui_api_->show("InventoryManagerWnd", true);
        }
        return true;
    }
    if (h == "Cmd_xtarget") {
        xtarget_command(args);
        return true;
    }
    if (h == "Cmd_filter") {
        // Cmd_filter @0x4e83a0: the options window shown on its chat filter page (4).
        if (ui_api_) {
            ui_api_->show("OptionsWindow", true);
            ui_api_->set_tab_page("OptionsWindow", "OPTW_OptionsSubwindows", 4);
        }
        return true;
    }
    if (h == "Cmd_note") {
        // Cmd_note @0x4f5e50: no text opens the notes window; text is added to it (13224 when that fails).
        if (!ui_api_) return true;
        if (args.empty()) ui_api_->show("PlayerNotesWindow", true);
        else if (!ui_api_->add_player_note(args)) c.show_eqstr(13224);
        return true;
    }
    if (h == "Cmd_map") {
        if (ui_api_)
            for (std::uint32_t id : ui_api_->map_command(args)) c.show_eqstr(id);
        return true;
    }
    if (h == "Cmd_loadskin") {
        // Cmd_loadskin @0x4f31e0: the skin is this client's user interface (ui/<pack>). No argument: the load-skin
        // window (LoadskinWnd, app/lua_ui_layout.cpp: the user interfaces). Not while the give, trade or send-money window is
        // open (3192, colour 0xf). "<skin> [flag]" writes the skin (the server's UI choice, as the Options window
        // does), says 3125 and loads it before the next frame; the flag (reload the window settings) is not used: the
        // window places are the character's and stay. (Its zone check, zone type 0xbc: 6028, has no such zone here.)
        if (!ui_api_) return true;
        if (args.empty()) {
            ui_api_->show("LoadskinWnd", true);
            return true;
        }
        if (ui_api_->shown("GiveWnd") || ui_api_->shown("TradeWnd") || c.trade().has_value()) {
            c.show_eqstr(3192, {}, 0xf);
            return true;
        }
        std::string_view rest = args;
        std::string skin = game::next_cmd_arg(rest);
        if (skin.empty()) return c.show_eqstr(3124), true;
        c.show_eqstr(3125, {skin});
        for (std::size_t i = 0; i < ui_packs_.size(); ++i)
            if (game::same_no_case(ui_packs_[i].id, skin) || game::same_no_case(ui_packs_[i].name, skin)) {
                select_ui_pack(ui_packs_[i].id);
                return true;
            }
        log::warn("/loadskin: no user interface named {}", skin);
        return true;
    }

    if (h == "Cmd_copylayout") {
        // Cmd_copylayout @0x4f33d0: no argument opens the layout copy window
        // (LayoutCopyWindow, app/lua_ui_layout.cpp); not while the give, trade or send-money window is open (3192,
        // colour 0xf); "usage" / "help" print 6335; "restore" is the screen's size as "%dx%d", else the first two
        // words are the size and the ini file (none: the current one; neither: 6335). It prints 6336 and copies
        // (UiHost::copy_layout, the boxes off). The ini file is UI_<name>_<server>.ini, default(s).ini or a
        // character's name here; the posted message's own reading of it (WndProc 0x4646) is not decoded
        // (docs/unknowns.md).
        if (!ui_api_) return true;
        if (args.empty()) {
            ui_api_->show("LayoutCopyWindow", true);
            return true;
        }
        if (ui_api_->shown("GiveWnd") || ui_api_->shown("TradeWnd") || c.trade().has_value()) {
            c.show_eqstr(3192, {}, 0xf);
            return true;
        }
        if (game::same_no_case(args, "usage") || game::same_no_case(args, "help")) return c.show_eqstr(6335), true;
        std::string resolution, ini;
        if (game::same_no_case(args, "restore")) {
            resolution = std::format("{}x{}", static_cast<int>(ImGui::GetIO().DisplaySize.x), static_cast<int>(ImGui::GetIO().DisplaySize.y));
        } else {
            std::string_view rest = args;
            resolution = game::next_cmd_arg(rest);
            ini = game::next_cmd_arg(rest);
        }
        if (resolution.empty()) return c.show_eqstr(6335), true;
        std::vector<UiHost::LayoutSource> sources = ui_host_.layout_sources ? ui_host_.layout_sources() : std::vector<UiHost::LayoutSource>{};
        auto names = [&](const UiHost::LayoutSource& s) {
            if (ini.empty()) return s.file == ui_api_->layout_file();
            if (s.label == "Defaults.ini") return game::same_no_case(ini, "defaults.ini") || game::same_no_case(ini, "default.ini");
            return game::same_no_case(ini, s.label) || game::same_no_case(ini, "UI_" + s.label + "_" + char_view_.server_name + ".ini");
        };
        UiHost::LayoutSource from;
        from.mode = game::same_no_case(resolution, "windowed") ? "Windowed" : resolution;
        from.label = ini.empty() ? "UI.ini" : ini;
        for (const auto& s : sources)
            if (names(s)) {
                from.file = s.file;
                from.label = s.label;
                break;
            }
        // An ini no source names is looked for beside the current layout file, by its bare name only: a path (a
        // folder, "..", a drive) is no layout, and copy_layout gives 6338 for the empty file as for a missing one.
        if (from.file.empty()) {
            if (ini.empty()) from.file = ui_api_->layout_file();
            else if (game::bare_file_name(ini) && !ui_api_->layout_file().empty())
                from.file = ui_api_->layout_file().parent_path() / ini;
        }
        c.show_eqstr(6336, {from.label, resolution});
        if (ui_host_.copy_layout)
            if (auto refused = ui_host_.copy_layout(from, {})) c.show_eqstr(refused->first, refused->second);
        return true;
    }

    // ---- chat windows
    if (h == "Hailstone_chattabs") {
        run_chat_tabs_command(c, args);
        return true;
    }
    if (h == "Cmd_chatfontsize") {
        // Cmd_chatfontsize @0x4e9a10: atoi 0..10 for the active chat window (a word that is not a number is 0);
        // otherwise 13270 (colour 0x111, to the active window in the original).
        if (!args.empty()) {
            auto size = static_cast<std::uint32_t>(game::eq_atoi(args));
            if (size <= 10) {
                set_active_chat_font(static_cast<int>(size));
                return true;
            }
        }
        c.show_eqstr(13270, {}, 0x111);
        return true;
    }
    if (h == "Cmd_clearchat" || h == "Cmd_clearallchat") {
        clear_chat_windows(h == "Cmd_clearallchat");
        return true;
    }
    if (h == "Cmd_usercolor") {
        // Cmd_usercolor @0x4e9b60 (game::usercolor_command): a help page, or the colour set (SetUserDefinedColor with
        // index - 1, which ignores anything outside 0..98) and kept (User_<n>_Red / _Green / _Blue in chat.ini).
        game::UserColor u = game::usercolor_command(args);
        if (u.page) {
            for (const char* line : game::usercolor_help_lines(*u.page)) c.show_chat(line, 0x111);
            return true;
        }
        // Hailstone's extended colours (indices 99..152) are not /usercolor's: the original's 100 is a no-op still.
        if (u.index - 1 >= game::kChatUserColorCountRoF2) return true;
        std::uint32_t rgb = u.reset ? default_user_color_rgb(u.index - 1)
                                    : static_cast<std::uint32_t>(u.red << 16 | u.green << 8 | u.blue);
        set_user_color(ui_host_, u.index - 1, rgb);
        return true;
    }
    if (h == "Cmd_hotbutton") {
        // Cmd_hotbutton @0x4f79b0: a social made from the line (game::hotbutton_command), put on the cursor (the
        // original's 0x4ea9f0 with start -1); 13282 made, 13283 not; no name the usage.
        game::HotButtonCommand hb = game::hotbutton_command(args);
        if (hb.usage) {
            for (std::uint32_t id = game::kStrHotButtonUsage; id <= game::kStrHotButtonUsage + 3; ++id) c.show_eqstr(id);
            return true;
        }
        int index = actions_ui_ ? actions_ui_->store_social(hb.name, hb.line, hb.color) : -1;
        if (index >= 0) {
            game::HotButton b;
            b.type = game::HotButtonType::Social;
            b.slot = index;
            ui_host_.cursor_button = b;
            ++ui_host_.cursor_serial;
        }
        c.show_eqstr(index >= 0 ? game::kStrHotButtonMade : game::kStrHotButtonFailed);
        return true;
    }

    // ---- display
    if (h == "Cmd_shownames") {
        // Cmd_shownames @0x4eb0f0: the line first, then the names (CDisplay::ToggleCharacterNameSprites) and the
        // level (__ShowNames, the name sprite's parts: draw_nameplates); ShowNamesLevel and PCNames kept.
        game::ShowNames n = game::shownames_command(args, off);
        c.show_eqstr(n.eqstr, {}, 0x111);
        if (n.usage) return true;
        pc_names_ = !n.off;
        if (!n.off) show_names_ = n.level;
        config_.set_int("display.show_names", n.off ? 0 : n.level);
        config_.set_bool("display.pc_names", pc_names_);
        save();
        return true;
    }
    if (h == "Cmd_showspelleffects") {
        // Cmd_showspelleffects @0x4e5780: "off" turns the spell particles off, any other word on; no word the usage
        // (13168, 13169); then the state (13170 / 13171). ShowSpellEffects kept.
        if (!args.empty()) {
            spell_effects_ = !game::same_no_case(args, off);
            config_.set_bool("display.spell_effects", spell_effects_);
            save();
        } else {
            c.show_eqstr(13168);
            c.show_eqstr(13169);
        }
        c.show_eqstr(spell_effects_ ? 13170 : 13171);
        return true;
    }
    if (h == "Cmd_dynamiclights") {
        // Cmd_dynamiclights @0x4e98f0: off / on (3 / 2 characters) / toggle; ShowDynamicLights kept; 13268 / 13269
        // (colour 0x111) say it takes effect at the next zone.
        bool lights = config_.get_bool("display.dynamic_lights", true);
        switch (game::off_on_toggle(args, off, on)) {
        case game::Switch::Off: lights = false; break;
        case game::Switch::On: lights = true; break;
        case game::Switch::Toggle: lights = !lights; break;
        }
        config_.set_bool("display.dynamic_lights", lights);
        save();
        c.show_eqstr(lights ? 13268 : 13269, {}, 0x111);
        return true;
    }
    if (h == "Cmd_showgrass") {
        // Cmd_showgrass @0x4e6da0: on / off when it is not so already (13179 / 13182; 13181 / 13183 when it is);
        // anything else the usage (13184) and the state. ShowGrass kept.
        bool grass = grass_;
        if (!args.empty() && (game::same_no_case(args, on) || game::same_no_case(args, off))) {
            bool want = game::same_no_case(args, on);
            if (grass == want) {
                c.show_eqstr(want ? 13181 : 13183);
            } else {
                grass_ = want; // the scene's virtual +0xb8: the flora drawn or not
                config_.set_bool("display.grass", want);
                save();
                c.show_eqstr(want ? 13179 : 13182);
            }
            return true;
        }
        c.show_eqstr(13184);
        c.show_eqstr(grass ? 13179 : 13182);
        return true;
    }
    if (h == "Cmd_shadows") {
        // Cmd_shadows @0x4de880: "on" turns shadows on (7002; here the Options window's shadows at Medium when they
        // were off) and a number after it is the shadow clip distance (0..100, ShadowClipPlane); "off" (7003);
        // anything else 7001.
        std::string_view rest = args;
        std::string first = game::next_cmd_arg(rest).substr(0, 0x27), second = game::next_cmd_arg(rest).substr(0, 0x27);
        if (game::same_no_case(first, on)) {
            if (display_option("shadows").value_or(0) == 0) set_display_option("shadows", 2);
            c.show_eqstr(7002);
            if (game::is_numeric_string(second)) {
                shadow_clip_plane_ = std::clamp(game::eq_atoi(second), 0, 100);
                config_.set_int("display.shadow_clip_plane", shadow_clip_plane_);
                save();
            }
        } else if (game::same_no_case(first, off)) {
            set_display_option("shadows", 0);
            c.show_eqstr(7003);
        } else {
            c.show_eqstr(7001);
        }
        return true;
    }
    if (h == "Cmd_particledensity") {
        // Cmd_particledensity @0x4ea740: 0..3 or OFF / LOW / MED / HIG(H); the line repeats the argument.
        // Cmd_particledensity_Set: the zone's particle clouds' density (EnvironmentParticleDensity).
        auto level = game::particle_density(args);
        if (!level) return c.show_chat(game::kParticleDensityUsage), true;
        set_particle_density("environment", game::particle_density_value(*level));
        save();
        c.show_chat(game::particle_density_line(args));
        return true;
    }
    if (h == "Cmd_viewport") {
        // Cmd_viewport @0x4eb380 (game::viewport_command): set, reset, or else the current one and the usage
        // (colour 0x111). The 3D view is drawn in it from the next frame (CDisplay::UpdateCameraFov; update_view_area),
        // kept as display.viewport (the original: the UI ini's ViewPort section per resolution). The screen is the
        // window, in its own coordinates (the UI's).
        ImVec2 screen = ImGui::GetIO().DisplaySize;
        auto sw = static_cast<std::int32_t>(screen.x), sh = static_cast<std::int32_t>(screen.y);
        if (auto v = game::viewport_command(args, sw, sh)) {
            viewport_ = *v;
            config_.set("display.viewport", game::viewport_text(*v));
            save();
            return true;
        }
        // The current one: the globals as the frame drew them (the whole screen without one).
        game::Viewport now;
        now.v = {static_cast<std::int32_t>(view_area_.x), static_cast<std::int32_t>(view_area_.y),
                 static_cast<std::int32_t>(view_area_.x + view_area_.w), static_cast<std::int32_t>(view_area_.y + view_area_.h),
                 0, 0, 100, 100};
        if (viewport_) {
            for (int k = 4; k < 8; ++k) now.v[k] = viewport_->v[k];
            if (!viewport_->scaled()) now.v[6] = now.v[7] = 0;
        }
        c.show_chat(game::viewport_line(now), 0x111);
        c.show_eqstr(game::kStrViewportUsage, {}, 0x111);
        return true;
    }
    if (h == "Cmd_indicator") {
        // Cmd_indicator @0x5c5530: the target indicator off / on (__stricmp) for this session (its flag, which
        // Show3dTargetIndicator sets at the start); "on" reads TargetIndicator.ini again.
        if (args.empty()) return true;
        if (game::same_no_case(args, off)) {
            c.set_client_option_for_session(game::ClientOption::ShowTargetRing, 0);
        } else if (game::same_no_case(args, on)) {
            c.set_client_option_for_session(game::ClientOption::ShowTargetRing, 1);
            target_indicator_.reset();
        }
        return true;
    }
    if (h == "Cmd_fullscreen") {
        // Cmd_fullscreen @0x478140: "on" when windowed, "off" when full screen, "toggle" either way.
        bool full = video_option("window_mode").value_or(0) != 0;
        bool want = full;
        if (game::same_no_case(args, "on")) want = true;
        else if (game::same_no_case(args, "off")) want = false;
        else if (game::same_no_case(args, "toggle")) want = !full;
        if (want != full) set_video_option("window_mode", want ? 2 : 0);
        return true;
    }
    if (h == "Cmd_fontface") {
        // Cmd_fontface @0x4de560: no face 3378 (colour 0xf); else Fonts / Font.<language>.0 (kept as ui.font_face).
        if (args.empty()) return c.show_eqstr(3378, {}, 0xf), true;
        config_.set("ui.font_face", args);
        save();
        apply_font_face(); // the font manager's +0x90: every UI font in the face from the next frame
        return true;
    }

    // ---- sound and mouse
    if (h == "Cmd_combatmusic") {
        // Cmd_combatmusic @0x4e5880: "off" off, any other word on (CombatMusic); no word 7523; then 7524 / 7525.
        if (!args.empty()) set_option_value("combat_music", game::same_no_case(args, off) ? 0 : 1);
        else c.show_eqstr(7523);
        c.show_eqstr(option_value("combat_music").value_or(1) ? 7524 : 7525);
        return true;
    }
    if (h == "Cmd_reversesound") {
        // Cmd_reversesound @0x4e81e0: "normal" (4 characters) / "reverse" (3) swap the stereo channels back or over;
        // no word 13214 and 13215; then the state (13217 / 13218); ReverseSound kept either way.
        bool reverse = option_value("reverse_sound").value_or(0) != 0;
        if (args.empty()) {
            c.show_eqstr(13214);
            c.show_eqstr(13215);
        } else if (game::starts_no_case(args, eqstr(13120), 4)) {
            reverse = false;
        } else if (game::starts_no_case(args, eqstr(13216), 3)) {
            reverse = true;
        }
        c.show_eqstr(reverse ? 13217 : 13218);
        set_option_value("reverse_sound", reverse ? 1 : 0);
        return true;
    }
    if (h == "Cmd_mixahead") {
        // Cmd_mixahead @0x4f5230: 1..100 kept (SoundMixAhead); else 7528; then 7529 with the value.
        // EqSoundManager::SetMixAhead: the output's buffer (audio::mix_ahead_ms).
        audio::AudioSettings settings = audio_ ? audio_->settings() : audio::load_audio_settings(config_);
        int value = settings.mix_ahead;
        if (auto v = game::mixahead_value(args)) {
            value = settings.mix_ahead = *v;
            if (audio_) audio_->set_settings(settings);
            audio::store_audio_settings(settings, config_);
            save();
        } else {
            c.show_eqstr(game::kStrMixAheadUsage);
        }
        c.show_eqstr(game::kStrMixAhead, {std::to_string(value)});
        return true;
    }
    if (h == "Cmd_mousespeed") {
        // Cmd_mousespeed @0x4f5130: a number kept as the pointer speed (MousePointerSpeedMod, the argument as typed);
        // else 13165, 13166; then 13167 with the value. A deliberate change: the number is held to 0..10, the range
        // eqclient.ini's is read in (game::held_pointer_speed), and that is what is applied, saved and shown.
        int value = window_.pointer_speed();
        if (game::is_numeric_string(args)) {
            value = game::held_pointer_speed(game::eq_atoi(args));
            set_pointer_speed(value);
            config_.set_int("mouse.pointer_speed", value);
            save();
        } else {
            c.show_eqstr(game::kStrMouseSpeedUsage);
            c.show_eqstr(game::kStrMouseSpeedUsage + 1);
        }
        c.show_eqstr(game::kStrMouseSpeed, {std::to_string(value)});
        return true;
    }
    return client_info_command(h, args);
}

void ClientApp::load_combat_profile_for_zone(const std::filesystem::path& char_dir,
                                             std::string_view class_short) {
    if (!combat_engine_) return;
    combat_char_dir_ = char_dir;
    const std::string group_id = active_group_profile_ ? active_group_profile_->id : opts_.profile;
    combat::CombatProfile prof;
    std::string err;
    if (!combat::resolve_character_combat_profile(platform::data_dir(), char_dir, group_id,
                                                class_short, prof, err)) {
        log::warn("combat: profile load note: {}", err);
        prof = combat::default_combat_profile_for_class(class_short);
    }
    if (!char_dir.empty()) {
        combat::migrate_imported_combat_settings(platform::data_dir(), char_dir / "hailstone.ini", prof);
        if (prof.profile_path.empty()) {
            prof.profile_path = (char_dir / "combat_profile.ini").string();
        }
    }
    combat_engine_->set_profile(std::move(prof));
    HailstoneCombatAdapter actions(conn_.get(), nav_.get(), &ui_host_);
    const std::string zone = conn_ ? conn_->world().zone_short_name : std::string();
    combat_engine_->on_zone_change(&actions, zone);
}

void ClientApp::tick_combat_controller() {
    if (!combat_engine_ || !conn_ || !player_ready_ || screen_ != Screen::InGame) return;
    const std::uint64_t now_ms = static_cast<std::uint64_t>(net::GameConnection::now_ms());
    const game::mechanics::Character* ch = stats_ui_ ? stats_ui_->character() : nullptr;
    last_combat_snapshot_ = build_combat_snapshot(*conn_, nav_.get(), &bus_, ch,
                                                  combat_engine_->profile(),
                                                  last_player_moved_, last_manual_backward_,
                                                  server_profile_.name, now_ms);
    HailstoneCombatAdapter actions(conn_.get(), nav_.get(), &ui_host_);
    combat_engine_->tick(last_combat_snapshot_, actions);
}

bool ClientApp::handle_combat_command(std::string_view action, std::string_view arg,
                                      std::string& out_msg) {
    if (!combat_engine_) {
        out_msg = "Combat controller not initialized";
        return false;
    }
    auto lower = [](std::string_view v) {
        std::string out(v);
        for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return out;
    };
    auto parse_bool = [&](std::string_view v, bool cur) {
        const std::string lv = lower(v);
        if (lv == "toggle") return !cur;
        if (lv == "1" || lv == "true" || lv == "on" || lv == "yes") return true;
        if (lv == "0" || lv == "false" || lv == "off" || lv == "no") return false;
        return !cur;
    };
    const std::string act = lower(action);
    HailstoneCombatAdapter actions(conn_.get(), nav_.get(), &ui_host_);

    if (act == "start" || act == "on") {
        combat_engine_->start("Started by player");
        out_msg = "Combat controller started";
        return true;
    }
    if (act == "stop" || act == "off") {
        combat_engine_->stop(&actions, "Stopped by player");
        out_msg = "Combat controller stopped";
        return true;
    }
    if (act == "pause") {
        combat_engine_->pause(&actions, "Paused by player");
        out_msg = "Combat controller paused";
        return true;
    }
    if (act == "resume") {
        combat_engine_->resume("Resumed by player");
        out_msg = "Combat controller resumed";
        return true;
    }
    if (act == "estop" || act == "emergency_stop") {
        combat_engine_->emergency_stop(actions, arg.empty() ? std::string_view("Emergency stop activated") : arg);
        out_msg = "EMERGENCY STOP activated - movement, casting, and attack halted";
        return true;
    }
    if (act == "reset_estop" || act == "reset") {
        const bool ok = combat_engine_->reset_emergency_stop("Emergency stop cleared by player");
        out_msg = ok ? "Emergency stop reset (controller remains Idle)" : "Not in EmergencyStop state";
        return ok;
    }
    if (act == "status") {
        const auto st = combat_engine_->status_snapshot(last_combat_snapshot_.in_zone ? &last_combat_snapshot_ : nullptr);
        out_msg = std::format("Combat: {} | Move: {} | Target: {} | Reason: {}{}",
                              combat::combat_state_name(st.state),
                              combat::movement_mode_name(st.movement_mode),
                              st.target_name.empty() ? "none" : st.target_name,
                              st.idle_reason.empty() ? st.transition_reason : st.idle_reason,
                              st.dry_run ? " [DRY-RUN]" : "");
        return true;
    }
    if (act == "reload") {
        std::string cls;
        if (conn_) {
            if (const game::Spawn* me = conn_->world().player()) {
                cls = std::string(game::class_short_name(me->class_id));
            }
        }
        load_combat_profile_for_zone(combat_char_dir_, cls);
        out_msg = "Combat profile reloaded";
        return true;
    }
    if (act == "save") {
        std::filesystem::path target = combat_char_dir_.empty()
            ? std::filesystem::path(combat_engine_->profile().profile_path)
            : (combat_char_dir_ / "combat_profile.ini");
        if (target.empty() || !combat::is_path_within_root(platform::data_dir(), target)) {
            out_msg = "Refused: combat profile path outside Hailstone data root";
            return false;
        }
        combat_engine_->mutable_profile().profile_path = target.string();
        const bool ok = combat::save_combat_profile(combat_engine_->profile(), target, out_msg);
        if (ok) out_msg = std::format("Saved {}", target.filename().string());
        return ok;
    }
    if (act == "dry_run") {
        const bool next = parse_bool(arg, combat_engine_->profile().dry_run);
        combat_engine_->set_dry_run(next);
        out_msg = next ? "Dry-run enabled" : "Dry-run disabled";
        return true;
    }
    if (act == "auto_loadout") {
        const bool next = parse_bool(arg, combat_engine_->profile().auto_loadout);
        combat_engine_->set_auto_loadout(next);
        out_msg = next ? "Automatic gem loadout overwrite ENABLED" : "Automatic gem loadout disabled (manual gems protected)";
        return true;
    }
    if (act == "rememorize_missing") {
        combat_engine_->mutable_profile().rememorize_missing =
            parse_bool(arg, combat_engine_->profile().rememorize_missing);
        out_msg = "Updated rememorize_missing";
        return true;
    }
    if (act == "rememorize_now") {
        combat_engine_->request_rememorize();
        out_msg = "Requested safe missing-gem memorization";
        return true;
    }
    if (act == "allow_melee_attack") {
        const bool next = parse_bool(arg, combat_engine_->profile().allow_melee_attack);
        combat_engine_->mutable_profile().allow_melee_attack = next;
        if (!next) actions.set_auto_attack(false);
        out_msg = next ? "Melee auto-attack enabled" : "Melee auto-attack disabled";
        return true;
    }
    if (act == "allow_pet_attack") {
        const bool next = parse_bool(arg, combat_engine_->profile().allow_pet_attack);
        combat_engine_->mutable_profile().allow_pet_attack = next;
        if (!next) actions.pet_back();
        out_msg = next ? "Pet attack enabled" : "Pet attack disabled";
        return true;
    }
    if (act == "use_abilities") {
        combat_engine_->mutable_profile().use_abilities =
            parse_bool(arg, combat_engine_->profile().use_abilities);
        out_msg = "Updated use_abilities";
        return true;
    }
    if (act == "melee_behind") {
        combat_engine_->mutable_profile().melee_behind =
            parse_bool(arg, combat_engine_->profile().melee_behind);
        out_msg = "Updated melee_behind";
        return true;
    }
    if (act == "pause_on_manual_move") {
        combat_engine_->mutable_profile().pause_on_manual_move =
            parse_bool(arg, combat_engine_->profile().pause_on_manual_move);
        out_msg = "Updated pause_on_manual_move";
        return true;
    }
    if (act == "movement_mode") {
        const auto m = combat::parse_movement_mode(arg, combat_engine_->profile().movement_mode);
        combat_engine_->set_movement_mode(m);
        out_msg = std::format("Movement mode: {}", combat::movement_mode_name(m));
        return true;
    }
    if (act == "assist_target") {
        std::string target(arg);
        if (lower(target) == "none") target.clear();
        combat_engine_->set_assist_target(target);
        out_msg = target.empty() ? "Assist target cleared" : std::format("Assist target: {}", target);
        return true;
    }
    if (act == "follow_target") {
        std::string target(arg);
        if (lower(target) == "none") target.clear();
        combat_engine_->set_follow_target(target);
        out_msg = target.empty() ? "Follow target cleared" : std::format("Follow target: {}", target);
        return true;
    }
    if (act == "retreat_hp_pct" || act == "engage_at_hp_pct") {
        try {
            const int v = std::clamp(std::stoi(std::string(arg)), 0, 100);
            if (act == "retreat_hp_pct") combat_engine_->mutable_profile().retreat_hp_pct = v;
            else combat_engine_->mutable_profile().engage_at_hp_pct = v;
            out_msg = std::format("{} = {}%", act, v);
            return true;
        } catch (...) {
            out_msg = "Invalid percentage";
            return false;
        }
    }
    if (act == "assist_range" || act == "melee_range" || act == "ranged_min_range" ||
        act == "ranged_max_range" || act == "disengage_distance" || act == "follow_distance") {
        try {
            const float v = std::clamp(std::stof(std::string(arg)), 0.0f, 500.0f);
            auto& p = combat_engine_->mutable_profile();
            if (act == "assist_range") p.assist_range = v;
            else if (act == "melee_range") p.melee_range = v;
            else if (act == "ranged_min_range") p.ranged_min_range = v;
            else if (act == "ranged_max_range") p.ranged_max_range = v;
            else if (act == "disengage_distance") p.disengage_distance = v;
            else p.follow_distance = v;
            out_msg = std::format("{} = {:.1f}", act, v);
            return true;
        } catch (...) {
            out_msg = "Invalid distance";
            return false;
        }
    }
    if (act == "toggle_rule") {
        auto& rules = combat_engine_->mutable_profile().rules;
        try {
            const std::size_t idx = static_cast<std::size_t>(std::stoul(std::string(arg)));
            if (idx >= 1 && idx <= rules.size()) {
                rules[idx - 1].enabled = !rules[idx - 1].enabled;
                out_msg = std::format("Rule {} {}", rules[idx - 1].name,
                                      rules[idx - 1].enabled ? "enabled" : "disabled");
                return true;
            }
        } catch (...) {}
        for (auto& r : rules) {
            if (lower(r.name) == lower(arg)) {
                r.enabled = !r.enabled;
                out_msg = std::format("Rule {} {}", r.name, r.enabled ? "enabled" : "disabled");
                return true;
            }
        }
        out_msg = "Unknown rule";
        return false;
    }
    if (act == "clear_logs") {
        combat_engine_->clear_logs();
        out_msg = "Combat logs cleared";
        return true;
    }
    if (act == "ui" || act == "toggle") {
        if (ui_api_) ui_api_->toggle("CombatWnd");
        out_msg = "Toggled Combat window";
        return true;
    }
    if (act == "show") {
        if (ui_api_) ui_api_->show("CombatWnd", true);
        out_msg = "Shown Combat window";
        return true;
    }
    if (act == "hide") {
        if (ui_api_) ui_api_->show("CombatWnd", false);
        out_msg = "Hidden Combat window";
        return true;
    }

    out_msg = std::format("Unknown combat action: {}", action);
    return false;
}

void ClientApp::run_slash_combat(const std::string& args) {
    std::string_view rest = args;
    std::string word = game::next_cmd_arg(rest);
    if (word.empty()) {
        if (ui_api_) ui_api_->toggle("CombatWnd");
        return;
    }
    std::string msg;
    std::string rem(rest);
    while (!rem.empty() && (rem.front() == ' ' || rem.front() == '\t')) rem.erase(rem.begin());
    while (!rem.empty() && (rem.back() == ' ' || rem.back() == '\t')) rem.pop_back();
    const bool ok = handle_combat_command(word, rem, msg);
    if (conn_ && !msg.empty()) {
        conn_->show_chat(msg, ok ? 15 : 0xd);
    }
    if (!ok && conn_) {
        conn_->show_chat("Usage: /combat [start|stop|pause|resume|estop|reset|reload|save|dry_run on|off|status|ui]", 15);
    }
}

ipc::CommandAckPacket ClientApp::handle_bus_command(const ipc::CommandPacket& cmd) {
    ipc::CommandAckPacket ack;
    ack.seq = cmd.seq;

    auto trim = [](std::string_view sv) {
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n'))
            sv.remove_prefix(1);
        while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n'))
            sv.remove_suffix(1);
        return sv;
    };
    auto lower_str = [](std::string_view sv) {
        std::string out(sv);
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    };

    const std::string_view full = trim(cmd.command);
    if (full.empty()) {
        ack.ok = false;
        ack.message = "empty command";
        return ack;
    }

    std::string_view rest = full;
    const std::string verb = lower_str(game::next_cmd_arg(rest));
    rest = trim(rest);
    const bool in_game = conn_ && screen_ == Screen::InGame && player_ready_;

    if (verb == "/estop") {
        std::string msg;
        ack.ok = handle_combat_command("estop", rest, msg);
        if (conn_) conn_->stop_follow(false);
        ack.message = msg.empty() ? "emergency stop activated" : msg;
        return ack;
    }

    if (verb == "/combat") {
        run_slash_combat(std::string(rest));
        ack.ok = true;
        ack.message = "combat command dispatched";
        return ack;
    }

    if (verb == "/combatstick") {
        if (!in_game || !conn_ || !nav_) {
            ack.ok = false;
            ack.message = "cannot stick while not in game";
            return ack;
        }
        const std::string mode = lower_str(rest);
        if (mode == "off" || mode == "stop") {
            nav_->unstick("Combat stick stopped");
            ack.ok = true;
            ack.message = "combat stick stopped";
            return ack;
        }
        const std::uint32_t target_id = conn_->world().target_id;
        const auto it = conn_->world().spawns.find(target_id);
        const game::Spawn* target = it != conn_->world().spawns.end() ? &it->second : nullptr;
        if (target_id == 0 || !target || target_id == conn_->world().player_id || !target->is_npc) {
            ack.ok = false;
            ack.message = "target an NPC first";
            return ack;
        }
        nav::StickOptions opt;
        opt.distance = combat_engine_ ? combat_engine_->profile().melee_range : 12.0f;
        if (!rest.empty()) {
            try { opt.distance = std::max(1.0f, std::stof(std::string(rest))); }
            catch (...) {
                ack.ok = false;
                ack.message = "usage: /combatstick [distance] | off";
                return ack;
            }
        }
        const std::string err = nav_->stick(target_id, opt);
        if (!err.empty()) {
            ack.ok = false;
            ack.message = err;
            return ack;
        }
        if (combat_engine_) {
            combat_engine_->mutable_profile().movement_mode = combat::MovementMode::MeleeRange;
            combat_engine_->mutable_profile().allow_melee_attack = true;
            combat_engine_->start("Combat stick enabled");
        }
        ack.ok = true;
        ack.message = std::format("combat stick enabled at {:.1f}", opt.distance);
        return ack;
    }

    if (verb == "/stopfollow" || (verb == "/follow" && (rest.empty() || lower_str(rest) == "off" || lower_str(rest) == "stop"))) {
        if (conn_) conn_->stop_follow(false);
        if (combat_engine_) {
            combat_engine_->set_follow_target("");
            if (combat_engine_->profile().movement_mode == combat::MovementMode::Follow)
                combat_engine_->set_movement_mode(combat::MovementMode::None);
        }
        ack.ok = true;
        ack.message = "stopped following";
        return ack;
    }

    if (verb == "/follow") {
        if (!in_game) {
            ack.ok = false;
            ack.message = "cannot follow while not in game";
            return ack;
        }
        const std::string target_name(rest);
        std::uint32_t match_id = 0;
        for (const auto& [sid, sp] : conn_->world().spawns) {
            if (iequals(game::display_name(sp.name), target_name) || iequals(sp.name, target_name)) {
                match_id = sid;
                break;
            }
        }
        if (match_id == 0) {
            ack.ok = false;
            ack.message = std::format("follow target '{}' not found in zone", target_name);
            return ack;
        }
        conn_->set_target(match_id);
        conn_->send_command("/follow");
        if (combat_engine_) {
            combat_engine_->set_follow_target(target_name);
            combat_engine_->set_movement_mode(combat::MovementMode::Follow);
        }
        ack.ok = true;
        ack.message = std::format("following {}", target_name);
        return ack;
    }

    if (verb == "/assist") {
        if (!in_game) {
            ack.ok = false;
            ack.message = "cannot assist while not in game";
            return ack;
        }
        if (rest.empty()) {
            ack.ok = false;
            ack.message = "assist target is required";
            return ack;
        }
        const std::string target_name(rest);
        if (combat_engine_) combat_engine_->set_assist_target(target_name);
        conn_->send_command("/assist " + target_name);
        ack.ok = true;
        ack.message = std::format("assisting {}", target_name);
        return ack;
    }

    if (verb == "/attack") {
        if (!in_game) {
            ack.ok = false;
            ack.message = "cannot toggle attack while not in game";
            return ack;
        }
        const std::string mode = lower_str(rest);
        const bool turn_on = (mode.empty() || mode == "on" || mode == "1" || mode == "true");
        conn_->set_auto_attack(turn_on);
        std::string msg;
        if (combat_engine_) {
            if (turn_on) handle_combat_command("start", {}, msg);
            else handle_combat_command("stop", {}, msg);
        }
        ack.ok = true;
        ack.message = turn_on ? "attack enabled" : "attack stopped";
        return ack;
    }

    if (verb == "/script") {
        if (!automation_) {
            ack.ok = false;
            ack.message = "script host unavailable";
            return ack;
        }
        std::string_view srest = rest;
        const std::string sub = lower_str(game::next_cmd_arg(srest));
        srest = trim(srest);
        std::string script_name = std::string(srest);
        if (script_name.empty()) {
            const auto running = automation_->scripts().list();
            script_name = running.empty() ? std::string("rgmercs") : running.front().name;
        }
        std::string why;
        if (sub == "pause") {
            bool is_running = false;
            for (const auto& s : automation_->scripts().list()) {
                if (s.name == script_name && s.status == "RUNNING") is_running = true;
            }
            if (!is_running) {
                ack.ok = false;
                ack.message = std::format("script '{}' is not running", script_name);
                return ack;
            }
            ack.ok = automation_->scripts().pause(script_name);
            ack.message = ack.ok ? std::format("paused {}", script_name) : std::format("failed to pause {}", script_name);
            return ack;
        }
        if (sub == "resume") {
            bool is_paused = false;
            for (const auto& s : automation_->scripts().list()) {
                if (s.name == script_name && s.status == "PAUSED") is_paused = true;
            }
            if (!is_paused) {
                ack.ok = false;
                ack.message = std::format("script '{}' is not paused", script_name);
                return ack;
            }
            ack.ok = automation_->scripts().pause(script_name);
            ack.message = ack.ok ? std::format("resumed {}", script_name) : std::format("failed to resume {}", script_name);
            return ack;
        }
        if (sub == "restart" || sub == "reload") {
            ack.ok = automation_->scripts().reload(script_name, {}, &why);
            ack.message = ack.ok ? std::format("reloaded {}", script_name)
                                 : (why.empty() ? std::format("failed to reload {}", script_name) : why);
            return ack;
        }
        if (sub == "start" || sub == "run") {
            ack.ok = automation_->scripts().run(script_name, {}, &why);
            ack.message = ack.ok ? std::format("started {}", script_name)
                                 : (why.empty() ? std::format("failed to start {}", script_name) : why);
            return ack;
        }
        if (sub == "stop") {
            ack.ok = automation_->scripts().stop(script_name);
            ack.message = ack.ok ? std::format("stopped {}", script_name) : std::format("script '{}' is not running", script_name);
            return ack;
        }
        ack.ok = false;
        ack.message = std::format("unknown /script action '{}'", sub);
        return ack;
    }

    if (verb == "/reloadprofile") {
        std::string msg;
        if (!rest.empty()) {
            const std::string pname(rest);
            active_commander_profile_ = pname;
            const auto gp = core::make_named_group_profile_preset(pname, server_profile_.name);
            if (combat_engine_ && !gp.slots.empty()) {
                const std::string my_name = in_game ? conn_->character_name() : opts_.auto_character;
                for (const auto& slot : gp.slots) {
                    if (iequals(slot.character, my_name) || gp.slots.size() == 1) {
                        if (!slot.follow_target.empty()) combat_engine_->set_follow_target(slot.follow_target);
                        if (!slot.class_role.empty()) combat_engine_->mutable_profile().role = slot.class_role;
                        if (!slot.combat_profile.empty()) combat_engine_->mutable_profile().profile_name = slot.combat_profile;
                        break;
                    }
                }
            }
            ack.ok = true;
            ack.message = std::format("profile '{}' applied", pname);
            return ack;
        }
        ack.ok = handle_combat_command("reload", {}, msg);
        ack.message = msg;
        return ack;
    }

    if (verb == "/reloadspells") {
        if (!in_game) {
            ack.ok = false;
            ack.message = "cannot reload spells while not in game";
            return ack;
        }
        if (conn_->in_combat() || (combat_engine_ && combat_engine_->state() == combat::CombatState::Combat)) {
            ack.ok = false;
            ack.message = "cannot memorize spells during combat";
            return ack;
        }
        if (!combat_engine_) {
            ack.ok = false;
            ack.message = "combat engine unavailable";
            return ack;
        }
        combat_engine_->request_rememorize();
        const auto st = combat_engine_->status_snapshot(last_combat_snapshot_.in_zone ? &last_combat_snapshot_ : nullptr);
        if (!st.loadout_audit.level_restricted_spells.empty() || !st.loadout_audit.missing_spell_data.empty() ||
            !st.loadout_audit.unknown_spells.empty()) {
            ack.ok = false;
            ack.message = std::format("spell loadout issues: {} unavailable, {} level-restricted, {} missing spell data",
                                      st.loadout_audit.unknown_spells.size(),
                                      st.loadout_audit.level_restricted_spells.size(),
                                      st.loadout_audit.missing_spell_data.size());
            return ack;
        }
        ack.ok = true;
        ack.message = "safe spell loadout reload requested";
        return ack;
    }

    if (verb == "/camp") {
        if (!in_game) {
            ack.ok = false;
            ack.message = "not in game to camp";
            return ack;
        }
        conn_->send_command("/camp");
        ack.ok = true;
        ack.message = "camp initiated";
        return ack;
    }

    if (verb == "/bind" || verb == "/returntobind") {
        if (!in_game) {
            ack.ok = false;
            ack.message = "cannot return to bind while not in game";
            return ack;
        }
        const std::string safe_bind_cmd = config_.get_string("commander.safe_bind_cmd", "");
        if (!safe_bind_cmd.empty() && safe_bind_cmd.front() == '/') {
            conn_->send_command(safe_bind_cmd);
            ack.ok = true;
            ack.message = std::format("dispatched server-authorized bind command '{}'", safe_bind_cmd);
            return ack;
        }
        // Check if character has a memorized Gate spell (safe standard game cast).
        if (last_combat_snapshot_.in_zone) {
            for (const auto& g : last_combat_snapshot_.gems) {
                if (g.spell_id != 0 && iequals(g.spell_name, "Gate")) {
                    conn_->send_command(std::format("/cast {}", g.gem_index + 1));
                    ack.ok = true;
                    ack.message = "casting memorized Gate";
                    return ack;
                }
            }
        }
        ack.ok = false;
        ack.message = "no server-authorized return-to-bind command or memorized Gate spell available";
        return ack;
    }

    if (verb == "/tptome" || verb == "/tphere") {
        if (!in_game) {
            ack.ok = false;
            ack.message = "cannot teleport while not in game";
            return ack;
        }
        const std::string safe_tp_cmd = config_.get_string("commander.safe_teleport_cmd", "");
        if (safe_tp_cmd.empty() || safe_tp_cmd.front() != '/') {
            ack.ok = false;
            ack.message = "teleport refused: no server-authorized safe teleport command configured (commander.safe_teleport_cmd)";
            return ack;
        }
        const std::string full_tp = rest.empty() ? safe_tp_cmd : (safe_tp_cmd + " " + std::string(rest));
        conn_->send_command(full_tp);
        ack.ok = true;
        ack.message = std::format("dispatched server-authorized '{}'", full_tp);
        return ack;
    }

    if (verb == "/porter" || verb == "/porterall") {
        if (!in_game) {
            ack.ok = false;
            ack.message = "cannot use porter while not in game";
            return ack;
        }
        std::string_view prest = rest;
        std::string zone = lower_str(game::next_cmd_arg(prest));
        if (zone.empty()) {
            ack.ok = false;
            ack.message = "porter destination zone is required";
            return ack;
        }
        std::string driver;
        std::string mode = "auto";
        std::string porter_npc = config_.get_string("commander.porter_npc", "Porter");
        while (!prest.empty()) {
            std::string tok = game::next_cmd_arg(prest);
            if (tok.empty()) break;
            const std::string low = lower_str(tok);
            if (low.starts_with("driver=")) {
                driver = tok.substr(7);
            } else if (low.starts_with("mode=")) {
                mode = lower_str(tok.substr(5));
            } else if (low.starts_with("npc=")) {
                porter_npc = tok.substr(4);
            }
        }
        if (porter_npc.empty()) porter_npc = "Porter";

        // Freeze active chase/follow before interacting with Porter (matches MQ Commander 5 porter_go flow).
        conn_->stop_follow(false);
        if (combat_engine_) {
            if (combat_engine_->profile().movement_mode == combat::MovementMode::Follow)
                combat_engine_->set_movement_mode(combat::MovementMode::None);
            if (!driver.empty()) combat_engine_->set_follow_target(driver);
        }

        // 1) Delegate to scripts/porter_go.lua when installed (unless explicitly forced to native mode).
        if (mode != "native" && automation_) {
            std::error_code ec;
            const auto scripts_root = platform::scripts_dir();
            const bool has_porter_script =
                std::filesystem::is_regular_file(scripts_root / "porter_go.lua", ec) ||
                std::filesystem::is_regular_file(scripts_root / "porter_go" / "init.lua", ec);
            if (has_porter_script) {
                std::vector<std::string> script_args;
                script_args.push_back(zone);
                if (!driver.empty()) script_args.push_back("driver=" + driver);
                std::string why;
                if (automation_->scripts().run("porter_go", script_args, &why)) {
                    ack.ok = true;
                    ack.message = driver.empty()
                                      ? std::format("PORTER_START: porter_go {}", zone)
                                      : std::format("PORTER_START: porter_go {} driver={}", zone, driver);
                    return ack;
                }
                if (mode == "script") {
                    ack.ok = false;
                    ack.message = why.empty() ? "failed to start porter_go" : why;
                    return ack;
                }
            } else if (mode == "script") {
                ack.ok = false;
                ack.message = "porter_go.lua not found in scripts/ directory";
                return ack;
            }
        }

        // 2) Native Hailstone Porter execution: locate nearest Porter NPC, target it, and /say <zone>.
        const game::Spawn* me = conn_->world().player();
        const std::string want_low = lower_str(porter_npc);
        std::uint32_t best_exact_id = 0;
        std::string best_exact_name;
        float best_exact_dist = std::numeric_limits<float>::max();
        std::uint32_t best_partial_id = 0;
        std::string best_partial_name;
        float best_partial_dist = std::numeric_limits<float>::max();

        for (const auto& [sid, sp] : conn_->world().spawns) {
            if (!sp.is_npc || sp.is_corpse) continue;
            const std::string disp = game::display_name(sp.name);
            const std::string disp_low = lower_str(disp);
            const std::string raw_low = lower_str(sp.name);
            float dist = 0.0f;
            if (me) {
                const float dx = sp.position.x - me->position.x;
                const float dy = sp.position.y - me->position.y;
                const float dz = sp.position.z - me->position.z;
                dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            if (disp_low == want_low || raw_low == want_low) {
                if (dist < best_exact_dist) {
                    best_exact_dist = dist;
                    best_exact_id = sid;
                    best_exact_name = disp;
                }
            } else if (disp_low.find(want_low) != std::string::npos ||
                       raw_low.find(want_low) != std::string::npos ||
                       disp_low.find("teleporter") != std::string::npos) {
                if (dist < best_partial_dist) {
                    best_partial_dist = dist;
                    best_partial_id = sid;
                    best_partial_name = disp;
                }
            }
        }

        const std::uint32_t chosen_id = best_exact_id != 0 ? best_exact_id : best_partial_id;
        const std::string& chosen_name = best_exact_id != 0 ? best_exact_name : best_partial_name;
        const float chosen_dist = best_exact_id != 0 ? best_exact_dist : best_partial_dist;
        if (chosen_id != 0) {
            conn_->set_target(chosen_id);
            conn_->send_command("/say " + zone);
            ack.ok = true;
            ack.message = std::format("PORTER_SAID: targeted {} ({:.0f}u) -> /say {}",
                                      chosen_name, chosen_dist, zone);
            return ack;
        }

        const std::string summon_cmd = config_.get_string("commander.summon_porter_cmd", "");
        if (!summon_cmd.empty() && summon_cmd.front() == '/') {
            conn_->send_command(summon_cmd);
            ack.ok = true;
            ack.message = std::format("PORTER_SUMMON: dispatched '{}'; retry /porter {} once Porter spawns",
                                      summon_cmd, zone);
            return ack;
        }

        ack.ok = false;
        ack.message = std::format("PORTER_FAIL: no '{}' NPC found in zone (stand near Porter or install scripts/porter_go.lua)",
                                  porter_npc);
        return ack;
    }

    if (automation_) {
        automation_->run_command(cmd.command);
        ack.ok = true;
        ack.message = "dispatched";
    } else {
        ack.ok = false;
        ack.message = "client command router unavailable";
    }
    return ack;
}

UiHost::CommanderStatusSnapshot ClientApp::commander_status() {
    UiHost::CommanderStatusSnapshot out;
    out.session_id = session_id_;
    out.character_name = (conn_ && screen_ == Screen::InGame && player_ready_)
                             ? conn_->character_name()
                             : (opts_.auto_character.empty() ? opts_.account : opts_.auto_character);
    out.execution_state = execution_state_name(execution_.state());
    out.bus_connected = bus_.connected();
    out.auto_headless_others = auto_headless_others_;
    out.active_profile_id = active_commander_profile_.empty() && combat_engine_
                                ? combat_engine_->profile().profile_name
                                : active_commander_profile_;
    out.available_profiles = core::default_group_profile_preset_names();
    for (const auto& p : core::list_group_profiles(platform::data_dir() / "profiles")) {
        if (std::find(out.available_profiles.begin(), out.available_profiles.end(), p.name) ==
            out.available_profiles.end()) {
            out.available_profiles.push_back(p.name);
        }
    }

    if (bus_.connected()) {
        out.peers = bus_.cached_roster();
        out.tracked_commands = bus_.tracked_commands();
    }
    const bool has_self = std::any_of(out.peers.begin(), out.peers.end(), [&](const ipc::SessionSnapshot& p) {
        return p.session_id == session_id_;
    });
    if (!has_self) {
        ipc::SessionSnapshot self;
        self.session_id = session_id_;
        self.pid = static_cast<std::uint32_t>(platform::process_id());
        self.character_name = out.character_name;
        self.server_name = server_profile_.name.empty() ? char_view_.server_name : server_profile_.name;
        self.exec_state = execution_.state();
        self.online = conn_ && screen_ == Screen::InGame && player_ready_;
        self.connection_state = self.online ? "in_game" : "login";
        if (self.online) {
            self.zone_short = conn_->world().zone_short_name;
            if (const game::Spawn* me = conn_->world().player()) {
                self.class_short = game::class_short_name(me->class_id);
                self.level = me->level;
            }
        }
        self.combat_state = combat_engine_
                                ? std::string(combat::combat_state_name(combat_engine_->state()))
                                : "Idle";
        if (automation_) {
            const auto running = automation_->scripts().list();
            if (!running.empty()) {
                self.script_name = running.front().name;
                self.script_state = running.front().status;
            } else {
                const auto rg = automation_->rgmercs().snapshot();
                self.script_state = rg.script_status == "error" ? "error" : "stopped";
            }
        }
        self.active_profile = out.active_profile_id;
        if (combat_engine_) self.follow_target = combat_engine_->profile().follow_target;
        out.peers.insert(out.peers.begin(), std::move(self));
    }
    return out;
}

bool ClientApp::handle_commander_command(std::string_view action, std::string_view target,
                                         std::string_view arg, std::string& out_msg) {
    auto lower_str = [](std::string_view sv) {
        std::string out(sv);
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    };
    const std::string act = lower_str(action);
    const std::string local_char = (conn_ && screen_ == Screen::InGame && player_ready_)
                                       ? conn_->character_name()
                                       : (opts_.auto_character.empty() ? opts_.account : opts_.auto_character);
    const bool targets_self = target.empty() || target == session_id_ ||
                              (!local_char.empty() && iequals(target, local_char));

    if (act == "summon_companion" || act == "summoncompanion") {
        if (!targets_self) {
            out_msg = "summon companion must run on the character using the horn";
            return false;
        }
        if (!conn_ || screen_ != Screen::InGame || !player_ready_) {
            out_msg = "cannot summon companions while not in game";
            return false;
        }
        if (ui_host_.confirm) {
            out_msg = "another confirmation is already pending";
            return false;
        }

        net::ItemSlot horn = net::ItemSlot::invalid();
        auto is_companion_call = [](std::string_view name) {
            std::string low(name);
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            // EZ Server's actual reward item is "Call of the Companions".  Keep
            // the generic form for older/custom item naming, but require the
            // companion wording so unrelated summon items cannot be clicked.
            return low.find("call of the companions") != std::string::npos ||
                   low.find("summon companion") != std::string::npos;
        };
        for (const auto& [key, item] : conn_->inventory().items()) {
            if (key.first != net::kInvPossessions) continue;
            if (is_companion_call(item.name)) {
                horn = net::ItemSlot::possessions(key.second);
                break;
            }
            for (const auto& bag : item.contents) {
                if (is_companion_call(bag.item.name)) {
                    horn = net::ItemSlot::possessions(key.second, static_cast<std::int16_t>(bag.index));
                    break;
                }
            }
            if (horn.valid()) break;
        }
        if (!horn.valid()) {
            out_msg = "Summon Companion was not found in carried inventory";
            return false;
        }

        const std::string prompt = arg.empty()
                                       ? "Group members are out of range or stuck. Summon them now?"
                                       : std::format("{} is out of range or stuck. Summon them now?", std::string(arg));
        ui_host_.confirm = UiHost::Confirm{
            prompt,
            3000,
            [this, horn](bool yes) {
                if (yes && conn_ && screen_ == Screen::InGame && player_ready_) {
                    conn_->use_item(horn);
                    conn_->show_chat("Summon Companion activated", 15);
                }
            },
            true};
        out_msg = "Summon Companion confirmation shown (timeout: Yes)";
        return true;
    }

    if (act == "clear_acks") {
        bus_.clear_tracked_commands();
        out_msg = "Cleared command acknowledgements";
        return true;
    }

    if (act == "set_auto_headless") {
        const std::string v = lower_str(arg);
        auto_headless_others_ = (v == "1" || v == "true" || v == "on" || v == "yes");
        config_.set_bool("multibox.auto_headless_others", auto_headless_others_);
        out_msg = auto_headless_others_
                      ? "Auto-headless background clients on tab switch: ON"
                      : "Auto-headless background clients on tab switch: OFF";
        return true;
    }

    if (act == "switch_tab") {
        if (targets_self) {
            apply_execution_state(ExecutionState::ForegroundFull);
            if (auto_headless_others_ && bus_.connected()) {
                bus_.request_state_control(session_id_, ExecutionState::ForegroundFull, true);
            }
            out_msg = std::format("Switched live view to {}", local_char.empty() ? session_id_ : local_char);
            return true;
        }
        if (!bus_.connected()) {
            out_msg = "Session bus disconnected; cannot switch remote client tab";
            return false;
        }
        bus_.request_state_control(std::string(target), ExecutionState::ForegroundFull, auto_headless_others_);
        out_msg = std::format("Activated client tab '{}'", target);
        return true;
    }

    if (act == "make_live") {
        if (targets_self) {
            apply_execution_state(ExecutionState::ForegroundFull);
            out_msg = "Client set to Live (Foreground)";
            return true;
        }
        if (!bus_.connected()) {
            out_msg = "Session bus disconnected";
            return false;
        }
        bus_.request_state_control(std::string(target), ExecutionState::ForegroundFull, false);
        out_msg = std::format("Requested Live mode for '{}'", target);
        return true;
    }

    if (act == "make_headless") {
        if (targets_self) {
            apply_execution_state(ExecutionState::BackgroundHeadless);
            out_msg = "Client set to Headless (Background)";
            return true;
        }
        if (!bus_.connected()) {
            out_msg = "Session bus disconnected";
            return false;
        }
        bus_.request_state_control(std::string(target), ExecutionState::BackgroundHeadless, false);
        out_msg = std::format("Requested Headless mode for '{}'", target);
        return true;
    }

    // Build canonical slash command for local/bus dispatch
    std::string slash_cmd;
    if (act == "follow") {
        const std::string who = arg.empty() ? local_char : std::string(arg);
        slash_cmd = who.empty() ? "/follow" : ("/follow " + who);
    } else if (act == "stop_follow") {
        slash_cmd = "/follow off";
    } else if (act == "stop") {
        slash_cmd = "/estop";
    } else if (act == "assist") {
        const std::string who = arg.empty() ? local_char : std::string(arg);
        slash_cmd = who.empty() ? "/assist" : ("/assist " + who);
    } else if (act == "attack_on") {
        slash_cmd = "/attack on";
    } else if (act == "attack_off") {
        slash_cmd = "/attack off";
    } else if (act == "pause_script") {
        slash_cmd = arg.empty() ? "/script pause" : ("/script pause " + std::string(arg));
    } else if (act == "resume_script") {
        slash_cmd = arg.empty() ? "/script resume" : ("/script resume " + std::string(arg));
    } else if (act == "restart_script") {
        slash_cmd = arg.empty() ? "/script restart" : ("/script restart " + std::string(arg));
    } else if (act == "reload_profile" || act == "apply_profile") {
        slash_cmd = arg.empty() ? "/reloadprofile" : ("/reloadprofile " + std::string(arg));
    } else if (act == "reload_spells") {
        slash_cmd = "/reloadspells";
    } else if (act == "camp") {
        slash_cmd = "/camp";
    } else if (act == "return_to_bind") {
        slash_cmd = "/returntobind";
    } else if (act == "tp_to_me") {
        const std::string who = arg.empty() ? local_char : std::string(arg);
        slash_cmd = who.empty() ? "/tptome" : ("/tptome " + who);
    } else if (act == "tp_here") {
        slash_cmd = arg.empty() ? "/tphere" : ("/tphere " + std::string(arg));
    } else if (act == "porter_all" || act == "porter") {
        std::string_view parg = arg;
        while (!parg.empty() && (parg.front() == ' ' || parg.front() == '\t')) parg.remove_prefix(1);
        while (!parg.empty() && (parg.back() == ' ' || parg.back() == '\t')) parg.remove_suffix(1);
        if (parg.empty()) {
            out_msg = "Porter destination zone is required";
            return false;
        }
        slash_cmd = "/porter " + std::string(parg);
    } else {
        out_msg = std::format("Unknown commander action '{}'", action);
        return false;
    }

    const std::string scope =
        (act == "porter_all") ? std::string("all")
                              : (target.empty() ? std::string("all") : std::string(target));
    bool local_ok = true;
    std::string local_msg;

    if (scope == "all" || targets_self) {
        ipc::CommandPacket local_pkt;
        local_pkt.target = session_id_;
        local_pkt.command = slash_cmd;
        const auto ack = handle_bus_command(local_pkt);
        local_ok = ack.ok;
        local_msg = ack.message;
    }

    if (scope == "all") {
        if (bus_.connected()) {
            ipc::CommandPacket pkt;
            pkt.scope = ipc::CommandScope::AllExceptSelf;
            pkt.command = slash_cmd;
            const auto seq = bus_.send_command(std::move(pkt));
            out_msg = std::format("Sent '{}' to group (local: {}; bus seq #{})", slash_cmd, local_msg, seq);
            return local_ok;
        }
        out_msg = local_msg.empty() ? std::format("Executed '{}' locally", slash_cmd) : local_msg;
        return local_ok;
    }

    if (targets_self) {
        out_msg = local_msg;
        return local_ok;
    }

    if (!bus_.connected()) {
        out_msg = "Session bus disconnected; remote command not sent";
        return false;
    }
    ipc::CommandPacket pkt;
    pkt.scope = ipc::CommandScope::TargetPeer;
    pkt.target = scope;
    pkt.command = slash_cmd;
    const auto seq = bus_.send_command(std::move(pkt));
    out_msg = std::format("Dispatched '{}' to '{}' (seq #{})", slash_cmd, scope, seq);
    return seq != 0;
}

} // namespace client_internal

} // namespace hailstone::app
