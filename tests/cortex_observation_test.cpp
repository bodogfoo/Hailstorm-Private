// src/cortex/observation.h: what one character's agent may observe, and nothing
// it may do. Every field here comes from the client's own published telemetry.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cortex/observation.h"
#include "ipc/session_bus.h"

using namespace hailstone;
using cortex::CharacterObservation;
using cortex::observe;
using cortex::PeerObservation;

namespace {

ipc::SessionSnapshot peer(std::string session_id, std::string character, std::string leader,
                          std::string server = "EZ Server", std::string zone = "qrg") {
    ipc::SessionSnapshot s;
    s.session_id = std::move(session_id);
    s.character_name = std::move(character);
    s.server_name = std::move(server);
    s.zone_short = std::move(zone);
    s.group_leader = std::move(leader);
    s.class_short = "CLR";
    s.level = 85;
    s.hp_pct = 90;
    s.mana_pct = 40;
    s.end_pct = 60;
    s.combat_state = "Combat";
    s.script_state = "running";
    s.online = true;
    return s;
}

bool has_member(const CharacterObservation& o, const std::string& name) {
    for (const auto& m : o.group)
        if (m.character_name == name) return true;
    return false;
}

const PeerObservation* member(const CharacterObservation& o, const std::string& name) {
    for (const auto& m : o.group)
        if (m.character_name == name) return &m;
    return nullptr;
}

} // namespace

TEST_CASE("cortex observation: identity, state and execution state come from the snapshot") {
    ipc::SessionSnapshot self;
    self.session_id = "mq-group1-1";
    self.character_name = "Stabbinyah";
    self.server_name = "EZ Server";
    self.class_short = "ROG";
    self.level = 85;
    self.online = true;
    self.zone_short = "qrg";
    self.hp_pct = 71;
    self.mana_pct = 12;
    self.end_pct = 88;
    self.target_name = "a gnoll";
    self.target_id = 4242;
    self.combat_state = "Combat";
    self.connection_state = "in_game";
    self.follow_target = "Yavaha";
    self.active_profile = "EZ Six Box";
    self.script_name = "rgmercs";
    self.script_state = "running";
    self.script_health = "OK";
    self.exec_state = app::ExecutionState::BackgroundHeadless;
    self.ping_ms = 31;
    self.rss_mib = 1500;
    self.vram_mib = 0;

    const auto o = observe(self, {self}, 4242);

    CHECK(o.observed_ms == 4242);
    CHECK(o.session_id == "mq-group1-1");
    CHECK(o.character_name == "Stabbinyah");
    CHECK(o.server_name == "EZ Server");
    CHECK(o.class_short == "ROG");
    CHECK(o.level == 85);
    CHECK(o.online);
    CHECK(o.zone_short == "qrg");
    CHECK(o.hp_pct == 71);
    CHECK(o.mana_pct == 12);
    CHECK(o.end_pct == 88);
    CHECK(o.target_name == "a gnoll");
    CHECK(o.target_id == 4242);
    CHECK(o.combat_state == "Combat");
    CHECK(o.connection_state == "in_game");
    CHECK(o.follow_target == "Yavaha");
    CHECK(o.active_profile == "EZ Six Box");
    CHECK(o.script_name == "rgmercs");
    CHECK(o.script_state == "running");
    CHECK(o.script_health == "OK");
    CHECK(o.execution_state == "BACKGROUND_HEADLESS");
    CHECK(o.ping_ms == 31);
    CHECK(o.rss_mib == 1500);
    CHECK(o.group_leader.empty());
    CHECK_FALSE(o.self_is_leader);
    CHECK(o.group.empty());
}

TEST_CASE("cortex observation: a follower sees its leader and the peers naming the same leader") {
    ipc::SessionSnapshot self = peer("mq-group1-3", "Fastlikebruce", "Yavaha");
    self.target_id = 7;

    std::vector<ipc::SessionSnapshot> roster;
    roster.push_back(self);                                           // self, excluded
    roster.push_back(peer("mq-group1-1", "Yavaha", ""));               // the leader, names no leader itself
    roster.push_back(peer("mq-group1-4", "Gatupy", "Yavaha"));         // same leader
    roster.push_back(peer("mq-group1-5", "Other", "Yavaha", "Other Server")); // other server, excluded
    ipc::SessionSnapshot offline = peer("mq-group1-6", "Kucodion", "Yavaha");
    offline.online = false;
    roster.push_back(offline);                                        // offline, excluded
    roster.push_back(peer("mq-group2-1", "Stranger", "Someoneelse"));  // other group, excluded

    const auto o = observe(self, roster, 1000);

    CHECK(o.group_leader == "Yavaha");
    CHECK_FALSE(o.self_is_leader);
    REQUIRE(o.group.size() == 2);
    // Sorted by character name, so the order does not depend on the bus.
    CHECK(o.group[0].character_name == "Gatupy");
    CHECK(o.group[1].character_name == "Yavaha");
    CHECK_FALSE(has_member(o, "Fastlikebruce"));
    CHECK_FALSE(has_member(o, "Other"));
    CHECK_FALSE(has_member(o, "Kucodion"));
    CHECK_FALSE(has_member(o, "Stranger"));

    const PeerObservation* leader = member(o, "Yavaha");
    REQUIRE(leader != nullptr);
    CHECK(leader->is_leader);
    CHECK(leader->zone_short == "qrg");
    CHECK(leader->hp_pct == 90);
    CHECK(leader->combat_state == "Combat");

    const PeerObservation* gatupy = member(o, "Gatupy");
    REQUIRE(gatupy != nullptr);
    CHECK_FALSE(gatupy->is_leader);
    CHECK(gatupy->group_leader == "Yavaha");
}

TEST_CASE("cortex observation: a leader sees the peers that name it") {
    ipc::SessionSnapshot self = peer("mq-group1-1", "Yavaha", "Yavaha");

    std::vector<ipc::SessionSnapshot> roster;
    roster.push_back(self);
    roster.push_back(peer("mq-group1-2", "Kucodion", "Yavaha"));
    roster.push_back(peer("mq-group1-3", "Beeboppaloopa", "")); // no leader published: not a member

    const auto o = observe(self, roster, 1);

    CHECK(o.self_is_leader);
    REQUIRE(o.group.size() == 1);
    CHECK(o.group[0].character_name == "Kucodion");
    CHECK_FALSE(o.group[0].is_leader);
}

TEST_CASE("cortex observation: no named leader means no group, whatever the roster says") {
    ipc::SessionSnapshot self = peer("solo-1", "Solo", "");
    std::vector<ipc::SessionSnapshot> roster;
    roster.push_back(peer("mq-group1-1", "Yavaha", "Yavaha"));
    roster.push_back(peer("mq-group1-2", "Kucodion", "Yavaha"));

    const auto o = observe(self, roster, 1);

    CHECK(o.group_leader.empty());
    CHECK_FALSE(o.self_is_leader);
    CHECK(o.group.empty());
}

TEST_CASE("cortex observation: two observations of the same roster are the same bytes") {
    ipc::SessionSnapshot self = peer("mq-group1-3", "Fastlikebruce", "Yavaha");
    std::vector<ipc::SessionSnapshot> roster;
    roster.push_back(peer("mq-group1-1", "Yavaha", ""));
    roster.push_back(peer("mq-group1-4", "Gatupy", "Yavaha"));
    std::vector<ipc::SessionSnapshot> reversed(roster.rbegin(), roster.rend());

    const std::string a = cortex::to_json(observe(self, roster, 9));
    const std::string b = cortex::to_json(observe(self, reversed, 9));

    CHECK(a == b);
    CHECK(a.find("\"observed_ms\":9") != std::string::npos);
    CHECK(a.find("\"character\":\"Fastlikebruce\"") != std::string::npos);
    CHECK(a.find("\"leader\":\"Yavaha\"") != std::string::npos);
    CHECK(a.find("\"members\":[") != std::string::npos);
    // Members are sorted in the JSON too.
    CHECK(a.find("\"character\":\"Gatupy\"") < a.find("\"character\":\"Yavaha\""));
}

TEST_CASE("cortex observation: JSON escapes what a character name can hold and stays on one line") {
    ipc::SessionSnapshot self = peer("s1", "a\"b\\c\nd", "");
    self.script_health = "ERR: <bad>\tthing";

    const std::string json = cortex::to_json(observe(self, {self}, 3));

    CHECK(json.find("\"character\":\"a\\\"b\\\\c\\nd\"") != std::string::npos);
    CHECK(json.find("\"script_health\":\"ERR: <bad>\\tthing\"") != std::string::npos);
    CHECK(json.find('\n') == std::string::npos);
    CHECK(json.front() == '{');
    CHECK(json.back() == '}');
    CHECK(json.find("\"group\":{\"leader\":\"\",\"self_is_leader\":false,\"members\":[]}") != std::string::npos);
}

TEST_CASE("cortex prompt context: one line naming the character, its zone, target and group") {
    ipc::SessionSnapshot self = peer("mq-group1-3", "Fastlikebruce", "Yavaha");
    self.class_short = "ROG";
    self.hp_pct = 64;
    self.target_name = "a gnoll";
    self.target_id = 99;
    self.exec_state = app::ExecutionState::ForegroundFull;

    std::vector<ipc::SessionSnapshot> roster;
    roster.push_back(peer("mq-group1-1", "Yavaha", ""));
    roster.push_back(peer("mq-group1-4", "Gatupy", "Yavaha"));

    const auto o = observe(self, roster, 1);
    const std::string text = cortex::to_prompt_context(o);

    CHECK(text.find("Fastlikebruce ROG 85 on EZ Server in qrg") == 0);
    CHECK(text.find("HP 64%") != std::string::npos);
    CHECK(text.find("target \"a gnoll\"") != std::string::npos);
    CHECK(text.find("Group (leader Yavaha, 2 peers)") != std::string::npos);
    CHECK(text.find("Yavaha CLR 85 (leader)") != std::string::npos);
    CHECK(text.find("Gatupy CLR 85") != std::string::npos);
    CHECK(text.find('\n') == std::string::npos);
}

TEST_CASE("cortex prompt context: no target, no group, and a character outside the world") {
    ipc::SessionSnapshot self = peer("solo-1", "Solo", "");
    self.target_id = 0;
    self.target_name = "";

    const std::string text = cortex::to_prompt_context(observe(self, {self}, 1));
    CHECK(text.find("no target") != std::string::npos);
    CHECK(text.find("Group: none.") != std::string::npos);

    ipc::SessionSnapshot offline = self;
    offline.online = false;
    offline.connection_state = "char_select";
    const std::string offline_text = cortex::to_prompt_context(observe(offline, {offline}, 1));
    CHECK(offline_text.find("is not in the world (char_select)") != std::string::npos);
    CHECK(offline_text.find("HP ") == std::string::npos);
}
