// The native combat slash command's name, as the chat input resolves it.
//
// `/combat` cannot be the native command: the interpreter's own test
// (game/slash_commands.cpp, word_matches) accepts a typed word that is a *prefix* of a
// table name, and the stock table has `/combatmusic` (Cmd_combatmusic, the combat-music
// toggle) with no `/combat` entry. So `/combat status` resolved to `Cmd_combatmusic`
// ("status" is a word, so it switched combat music on) and the native hook in
// GameConnection::send_command's invalid-command branch never ran. The native command is
// `/hcombat`; `/combat` stays the stock music command.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "game/slash_commands.h"
#include "mock_server/harness.h"

using namespace hailstone;

TEST_CASE("native combat alias: /combat is the stock combat-music command and /hcombat is free") {
    // The typed word is a prefix of the table name, so "/combat" is the combat-music
    // command rather than an invalid line: that is why the native hook could not see it.
    const auto music = game::find_slash_command("/combat", false, nullptr);
    REQUIRE(music.has_value());
    CHECK(std::string(game::slash_commands()[*music].name) == "/combatmusic");
    CHECK(std::string(game::slash_commands()[*music].handler) == "Cmd_combatmusic");

    // A native name must not be a prefix of any stock command, or the table wins again.
    for (const char* typed : {"/hcombat", "/hcombat status"}) {
        CAPTURE(typed);
        const std::string line(typed);
        const std::string word = line.substr(0, line.find(' '));
        CHECK_FALSE(game::find_slash_command(word, false, nullptr).has_value());
    }
}

TEST_CASE("native combat alias: /hcombat reaches the native handler, /combat stays combat music") {
    // The user's chat line, handled the way ClientApp handles it: the chat window's submit
    // hands the text to GameConnection::send_command (app/client_app_ui.cpp:407). The
    // connection is in a zone, so the command interpreter runs for real.
    test::Rig rig;
    REQUIRE(rig.to_zone());

    struct Call {
        std::string handler;
        std::string args;
    };
    std::vector<Call> calls;
    rig.conn.set_client_commands([&](std::string_view handler, const std::string& args) {
        calls.push_back({std::string(handler), args});
        return true;
    });

    rig.conn.send_command("/hcombat status");
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].handler == "Hailstone_combat"); // app/client_app_commands.cpp:244 -> run_slash_combat
    CHECK(calls[0].args == "status");

    calls.clear();
    rig.conn.send_command("/combat status");
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].handler == "Cmd_combatmusic"); // the stock music setting, not the native engine
    CHECK(calls[0].args == "status");
    for (const auto& c : calls) CHECK(c.handler != "Hailstone_combat");

    // A bare /hcombat opens the combat window through the same handler; it is not a stock
    // command and must not fall through to "not a valid command".
    calls.clear();
    rig.conn.send_command("/hcombat");
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].handler == "Hailstone_combat");
    CHECK(calls[0].args.empty());
}
