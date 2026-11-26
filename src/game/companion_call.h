#pragma once

// The item /summoncompanion looks for (app/client_app_commands.cpp): the horn that calls a character's companion.
// Kept apart from the command so the name matching and the carried-items search can be tested without a client.

#include <string_view>

#include "game/inventory.h"

namespace hailstone::game {

// EZ Server's reward item is "Call of the Companions"; older and custom item files use "Summon Companion". The
// match is a case-insensitive substring, so decorated names ("Call of the Companions +1") still count, and the
// companion wording is required so unrelated summon items (a summoned mount, a pet) cannot be clicked.
bool is_companion_call_name(std::string_view name);

// The carried item that calls a companion: Possessions, the top level first and then one bag level deep, in slot
// order. An invalid slot when the character carries none; a horn inside a bag inside a bag is not found (the
// inventory's carried bags are read one level, as the client's own item list is).
net::ItemSlot find_companion_call_item(const Inventory& inventory);

} // namespace hailstone::game
