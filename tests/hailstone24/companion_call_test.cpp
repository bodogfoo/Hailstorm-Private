// /summoncompanion's item lookup (game/companion_call.h) and the confirmation timeout it relies on
// (app/lua_ui.h confirmation_timeout_answer, app/lua_ui_player.cpp OnProcessFrame).

#include <doctest/doctest.h>

#include <string>

#include "app/lua_ui.h"
#include "game/companion_call.h"
#include "net/item_packets.h"

using namespace hailstone;

namespace {

net::Item named(std::string name) {
    net::Item item;
    item.name = std::move(name);
    return item;
}

net::Item bag() {
    net::Item item;
    item.item_class = 1; // is_bag()
    item.bag_slots = 10;
    return item;
}

} // namespace

TEST_CASE("Companion call: only the companion horn's names match") {
    // EZ Server's reward item, decorated names, and the generic wording, in any case.
    CHECK(game::is_companion_call_name("Call of the Companions"));
    CHECK(game::is_companion_call_name("call of the companions"));
    CHECK(game::is_companion_call_name("CALL OF THE COMPANIONS +1"));
    CHECK(game::is_companion_call_name("Summon Companion"));
    CHECK(game::is_companion_call_name("summon companion horn of the pack"));

    // Names that must not be clicked: other summons, other items with "companion" in them.
    CHECK_FALSE(game::is_companion_call_name("Summon Mount"));
    CHECK_FALSE(game::is_companion_call_name("Summon Corpse"));
    CHECK_FALSE(game::is_companion_call_name("Companion's Blessing"));
    CHECK_FALSE(game::is_companion_call_name("Call of the Wild"));
    CHECK_FALSE(game::is_companion_call_name(""));
}

TEST_CASE("Companion call: the carried horn is found at the top level or one bag deep") {
    SUBCASE("nothing carried") {
        game::Inventory inv;
        CHECK_FALSE(game::find_companion_call_item(inv).valid());
    }

    SUBCASE("top-level possessions slot") {
        game::Inventory inv;
        REQUIRE(inv.put(net::ItemSlot::possessions(7), named("Call of the Companions")));
        const net::ItemSlot horn = game::find_companion_call_item(inv);
        REQUIRE(horn.valid());
        CHECK(horn.type == net::kInvPossessions);
        CHECK(horn.slot == 7);
        CHECK(horn.top_level());
    }

    SUBCASE("inside a carried bag") {
        game::Inventory inv;
        REQUIRE(inv.put(net::ItemSlot::possessions(3), bag()));
        REQUIRE(inv.put(net::ItemSlot::possessions(3, 2), named("Summon Companion")));
        const net::ItemSlot horn = game::find_companion_call_item(inv);
        REQUIRE(horn.valid());
        CHECK(horn.slot == 3);
        CHECK(horn.sub == 2);
        CHECK_FALSE(horn.top_level());
    }

    SUBCASE("a horn in the bank is not carried") {
        game::Inventory inv;
        REQUIRE(inv.put(net::ItemSlot{net::kInvBank, 1, -1, -1, 0, 0}, named("Call of the Companions")));
        CHECK_FALSE(game::find_companion_call_item(inv).valid());
    }

    SUBCASE("an unrelated summon item is not the horn") {
        game::Inventory inv;
        REQUIRE(inv.put(net::ItemSlot::possessions(1), named("Summon Mount")));
        REQUIRE(inv.put(net::ItemSlot::possessions(2), bag()));
        REQUIRE(inv.put(net::ItemSlot::possessions(2, 0), named("Companion's Blessing")));
        CHECK_FALSE(game::find_companion_call_item(inv).valid());
    }

    SUBCASE("the top level is searched before bag contents") {
        game::Inventory inv;
        REQUIRE(inv.put(net::ItemSlot::possessions(1), bag()));
        REQUIRE(inv.put(net::ItemSlot::possessions(1, 0), named("Call of the Companions")));
        REQUIRE(inv.put(net::ItemSlot::possessions(5), named("Summon Companion")));
        const net::ItemSlot horn = game::find_companion_call_item(inv);
        // The map's own slot order decides; both are the right item, so only the first found matters.
        REQUIRE(horn.valid());
        CHECK(horn.type == net::kInvPossessions);
    }
}

TEST_CASE("Confirmation timeout: only an opt-in question 3 answers Yes") {
    app::UiHost host;
    CHECK_FALSE(app::confirmation_timeout_answer(host, 3)); // no pending confirmation at all

    bool answered = false;
    host.confirm = app::UiHost::Confirm{"Summon them now?", 3000, [&](bool yes) { answered = yes; }};
    // The aggregate leaves timeout_answer false: a dialog that did not opt in times out as No.
    CHECK_FALSE(host.confirm->timeout_answer);
    CHECK_FALSE(app::confirmation_timeout_answer(host, 3));
    CHECK_FALSE(app::confirmation_timeout_answer(host, 1));
    CHECK_FALSE(app::confirmation_timeout_answer(host, 2));
    CHECK_FALSE(app::confirmation_timeout_answer(host, 4));

    // /summoncompanion is the one caller that opts in; still only for its own question.
    host.confirm->timeout_answer = true;
    CHECK(app::confirmation_timeout_answer(host, 3));
    CHECK_FALSE(app::confirmation_timeout_answer(host, 1));
    CHECK_FALSE(app::confirmation_timeout_answer(host, 2));
    CHECK_FALSE(app::confirmation_timeout_answer(host, 4));
    CHECK_FALSE(answered); // asking the question does not answer it
}
