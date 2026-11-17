#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "app/login_launch.h"
#include "core/server_profile.h"
#include "net/login.h"
#include "ui/dev_screens.h"

using namespace hailstone;

TEST_CASE("Login auto-select: resolve_world_target supports exact, key-part, and unique keyword matching") {
    std::vector<net::ServerInfo> servers(3);
    servers[0].id = 10;
    servers[0].name = "Project 1999 (Green)";
    servers[1].id = 25;
    servers[1].name = "EZ (Linux) [RoF2]";
    servers[2].id = 30;
    servers[2].name = "Project Lazarus [RoF2]";

    // Exact numeric id & exact case-insensitive name (same as launcher_world).
    CHECK(app::resolve_world_target(servers, "25") == &servers[1]);
    CHECK(app::resolve_world_target(servers, "ez (linux) [rof2]") == &servers[1]);

    // Whitespace-trimmed / saved_character_key_part exact match.
    const std::string key = saved_character_key_part(servers[1].name);
    CHECK(app::resolve_world_target(servers, key) == &servers[1]);

    // Multi-token keyword match uniquely selects EZ (Linux) [RoF2].
    CHECK(app::resolve_world_target(servers, "ez linux") == &servers[1]);
    CHECK(app::resolve_world_target(servers, "EZ rof2") == &servers[1]);

    // Ambiguous keyword across multiple worlds refuses to guess.
    CHECK(app::resolve_world_target(servers, "rof2") == nullptr);
    CHECK(app::resolve_world_target(servers, "project") == nullptr);

    // Non-matching or empty selector.
    CHECK(app::resolve_world_target(servers, "nonexistent") == nullptr);
    CHECK(app::resolve_world_target(servers, "   ") == nullptr);

    // Exact launcher_world remains strict so upstream semantics never drift.
    CHECK(app::launcher_world(servers, "ez linux") == nullptr);
    CHECK(app::resolve_world_target(servers, "ez linux", false) == nullptr);
}

TEST_CASE("Server select: keyword filter matches all whitespace tokens case-insensitively") {
    ui::ServerEntry ez{"EZ (Linux) x2/x4 [RoF2]", "Up", true, true};
    ui::ServerEntry p99{"Project 1999 Green", "Up", true, false};

    CHECK(ui::server_matches_filter(ez, ""));
    CHECK(ui::server_matches_filter(ez, "   \t "));
    CHECK(ui::server_matches_filter(ez, "ez"));
    CHECK(ui::server_matches_filter(ez, "rof2 ez linux"));
    CHECK(ui::server_matches_filter(ez, "  EZ   x2  "));
    CHECK_FALSE(ui::server_matches_filter(ez, "ez p99"));
    CHECK_FALSE(ui::server_matches_filter(p99, "ez"));
}

TEST_CASE("Server select: favorite worlds trim whitespace and dedupe case-insensitively while pinning top") {
    const auto parsed = ui::parse_favorite_worlds("  EZ Server | Project 1999 || ez server |  Laz  ");
    CHECK(parsed == std::vector<std::string>{"EZ Server", "Project 1999", "Laz"});

    const std::vector<std::string> names{
        "Alpha World",
        "Beta [RoF2]",
        "EZ Server",
        "Gamma",
        "Zeta",
    };
    const auto order = ui::server_list_order(names, {"  ez server  ", "zeta"});
    std::vector<std::string> sorted;
    sorted.reserve(order.size());
    for (std::size_t idx : order) sorted.push_back(names[idx]);
    CHECK(sorted == std::vector<std::string>{"EZ Server", "Zeta", "Alpha World", "Beta [RoF2]", "Gamma"});
}
