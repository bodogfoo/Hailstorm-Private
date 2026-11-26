#include "game/companion_call.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace hailstone::game {

bool is_companion_call_name(std::string_view name) {
    std::string low(name);
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return low.find("call of the companions") != std::string::npos ||
           low.find("summon companion") != std::string::npos;
}

net::ItemSlot find_companion_call_item(const Inventory& inventory) {
    for (const auto& [key, item] : inventory.items()) {
        if (key.first != net::kInvPossessions) continue;
        if (is_companion_call_name(item.name)) return net::ItemSlot::possessions(key.second);
        for (const auto& bag : item.contents) {
            if (is_companion_call_name(bag.item.name)) {
                return net::ItemSlot::possessions(key.second, static_cast<std::int16_t>(bag.index));
            }
        }
    }
    return net::ItemSlot::invalid();
}

} // namespace hailstone::game
