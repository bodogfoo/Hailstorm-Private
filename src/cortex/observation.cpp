// Hailstone Cortex: the observation layer (src/cortex/observation.h).

#include "cortex/observation.h"

#include <algorithm>
#include <cstdio>
#include <string_view>

#include "app/execution_state.h"

namespace hailstone::cortex {
namespace {

// A JSON string literal. The same escape set as core/group_profile.cpp's
// json_escape, kept here because that one is private to its translation unit.
std::string quote(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned int>(c));
                out += buf;
            } else {
                out.push_back(ch);
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

std::string number(std::uint64_t value) { return std::to_string(value); }

// `"key":value` joined into an object body; the caller adds the braces.
struct ObjectWriter {
    std::string body;

    void add(std::string_view key, const std::string& json_value) {
        if (!body.empty()) body += ',';
        body += quote(key);
        body += ':';
        body += json_value;
    }
    void add_number(std::string_view key, std::uint64_t value) { add(key, number(value)); }
    void add_bool(std::string_view key, bool value) { add(key, value ? "true" : "false"); }
    void add_string(std::string_view key, std::string_view value) { add(key, quote(value)); }

    std::string str() const { return "{" + body + "}"; }
};

ObjectWriter peer_object(const PeerObservation& peer) {
    ObjectWriter o;
    o.add_string("session_id", peer.session_id);
    o.add_string("character", peer.character_name);
    o.add_string("class", peer.class_short);
    o.add_number("level", peer.level);
    o.add_string("zone", peer.zone_short);
    o.add_string("target", peer.target_name);
    o.add_number("hp_pct", peer.hp_pct);
    o.add_number("mana_pct", peer.mana_pct);
    o.add_number("end_pct", peer.end_pct);
    o.add_string("combat_state", peer.combat_state);
    o.add_string("script_state", peer.script_state);
    o.add_string("group_leader", peer.group_leader);
    o.add_bool("is_leader", peer.is_leader);
    return o;
}

// "Name CLR 85" style identity, or just the parts that are known.
std::string who(const std::string& character, const std::string& class_short, std::uint8_t level) {
    std::string out = character.empty() ? "<unnamed>" : character;
    if (!class_short.empty()) out += " " + class_short;
    if (level != 0) out += " " + std::to_string(level);
    return out;
}

std::string vitals(std::uint8_t hp, std::uint8_t mana, std::uint8_t endurance) {
    return "HP " + std::to_string(hp) + "% MP " + std::to_string(mana) + "% EN " + std::to_string(endurance) + "%";
}

void append(std::string& out, std::string_view part) {
    if (part.empty()) return;
    if (!out.empty()) out += ' ';
    out += part;
}

} // namespace

CharacterObservation observe(const ipc::SessionSnapshot& self,
                             const std::vector<ipc::SessionSnapshot>& roster,
                             std::uint64_t now_ms) {
    CharacterObservation out;
    out.observed_ms = now_ms;

    out.session_id = self.session_id;
    out.character_name = self.character_name;
    out.server_name = self.server_name;
    out.class_short = self.class_short;
    out.level = self.level;
    out.online = self.online;
    out.zone_short = self.zone_short;
    out.hp_pct = self.hp_pct;
    out.mana_pct = self.mana_pct;
    out.end_pct = self.end_pct;
    out.target_name = self.target_name;
    out.target_id = self.target_id;
    out.combat_state = self.combat_state;
    out.connection_state = self.connection_state;
    out.follow_target = self.follow_target;
    out.active_profile = self.active_profile;
    out.script_name = self.script_name;
    out.script_state = self.script_state;
    out.script_health = self.script_health;
    out.execution_state = app::execution_state_name(self.exec_state);
    out.ping_ms = self.ping_ms;
    out.rss_mib = self.rss_mib;
    out.vram_mib = self.vram_mib;
    out.group_leader = self.group_leader;
    out.self_is_leader = !out.group_leader.empty() && out.group_leader == out.character_name;

    if (out.group_leader.empty()) return out;

    for (const auto& peer : roster) {
        if (!peer.online) continue;
        if (peer.session_id == self.session_id) continue;
        if (peer.character_name.empty() || peer.character_name == self.character_name) continue;
        if (peer.server_name != self.server_name) continue;

        // The leader itself, a peer naming the same leader, or (when this character
        // leads) a peer naming this character. A leader's own snapshot may carry an
        // empty group_leader, hence the first case.
        const bool is_leader = peer.character_name == out.group_leader;
        const bool same_leader = !peer.group_leader.empty() && peer.group_leader == out.group_leader;
        const bool names_self = out.self_is_leader && peer.group_leader == self.character_name;
        if (!is_leader && !same_leader && !names_self) continue;

        PeerObservation member;
        member.session_id = peer.session_id;
        member.character_name = peer.character_name;
        member.class_short = peer.class_short;
        member.level = peer.level;
        member.zone_short = peer.zone_short;
        member.target_name = peer.target_name;
        member.hp_pct = peer.hp_pct;
        member.mana_pct = peer.mana_pct;
        member.end_pct = peer.end_pct;
        member.combat_state = peer.combat_state;
        member.script_state = peer.script_state;
        member.group_leader = peer.group_leader;
        member.is_leader = is_leader;
        out.group.push_back(std::move(member));
    }

    // A stable order, so two observations of the same roster are the same bytes
    // whatever order the bus delivered the peers in.
    std::sort(out.group.begin(), out.group.end(), [](const PeerObservation& a, const PeerObservation& b) {
        if (a.character_name != b.character_name) return a.character_name < b.character_name;
        return a.session_id < b.session_id;
    });
    return out;
}

std::string to_json(const CharacterObservation& o) {
    ObjectWriter identity;
    identity.add_string("session_id", o.session_id);
    identity.add_string("character", o.character_name);
    identity.add_string("server", o.server_name);
    identity.add_string("class", o.class_short);
    identity.add_number("level", o.level);
    identity.add_bool("online", o.online);

    ObjectWriter state;
    state.add_string("zone", o.zone_short);
    state.add_number("hp_pct", o.hp_pct);
    state.add_number("mana_pct", o.mana_pct);
    state.add_number("end_pct", o.end_pct);
    state.add_string("target", o.target_name);
    state.add_number("target_id", o.target_id);
    state.add_string("combat_state", o.combat_state);
    state.add_string("connection_state", o.connection_state);
    state.add_string("follow_target", o.follow_target);
    state.add_string("active_profile", o.active_profile);
    state.add_string("script_name", o.script_name);
    state.add_string("script_state", o.script_state);
    state.add_string("script_health", o.script_health);
    state.add_string("execution_state", o.execution_state);
    state.add_number("ping_ms", o.ping_ms);
    state.add_number("rss_mib", o.rss_mib);
    state.add_number("vram_mib", o.vram_mib);

    std::string members;
    for (const auto& peer : o.group) {
        if (!members.empty()) members += ',';
        members += peer_object(peer).str();
    }

    ObjectWriter group;
    group.add_string("leader", o.group_leader);
    group.add_bool("self_is_leader", o.self_is_leader);
    group.add("members", "[" + members + "]");

    ObjectWriter root;
    root.add_number("observed_ms", o.observed_ms);
    root.add("identity", identity.str());
    root.add("state", state.str());
    root.add("group", group.str());
    return root.str();
}

std::string to_prompt_context(const CharacterObservation& o) {
    std::string head = who(o.character_name, o.class_short, o.level);
    if (!o.server_name.empty()) head += " on " + o.server_name;
    if (!o.online) {
        std::string offline = head;
        offline += " is not in the world (";
        offline += o.connection_state.empty() ? "offline" : o.connection_state;
        offline += ").";
        return offline;
    }
    if (!o.zone_short.empty()) head += " in " + o.zone_short;

    std::string out = head;
    out += ". ";
    out += vitals(o.hp_pct, o.mana_pct, o.end_pct);
    out += o.target_id != 0 ? ", target \"" + (o.target_name.empty() ? std::string("<unnamed>") : o.target_name) +
                                  "\""
                            : ", no target";
    if (!o.combat_state.empty()) out += ", combat " + o.combat_state;
    if (!o.follow_target.empty()) out += ", following " + o.follow_target;
    if (!o.script_name.empty()) out += ", script " + o.script_name + " " + o.script_state;
    if (!o.script_health.empty()) out += ", script health " + o.script_health;
    if (!o.execution_state.empty()) out += ", " + o.execution_state;
    out += ".";

    if (o.group.empty() && o.group_leader.empty()) {
        out += " Group: none.";
        return out;
    }

    out += " Group (leader ";
    out += o.group_leader.empty() ? std::string("unnamed") : o.group_leader;
    out += ", " + std::to_string(o.group.size()) + (o.group.size() == 1 ? " peer): " : " peers): ");
    std::string members;
    for (const auto& peer : o.group) {
        std::string line = who(peer.character_name, peer.class_short, peer.level);
        if (peer.is_leader) line += " (leader)";
        if (!peer.zone_short.empty()) line += " in " + peer.zone_short;
        line += " " + vitals(peer.hp_pct, peer.mana_pct, peer.end_pct);
        if (!peer.combat_state.empty()) line += " combat " + peer.combat_state;
        if (!peer.target_name.empty()) line += " on \"" + peer.target_name + "\"";
        if (!peer.script_state.empty()) line += " script " + peer.script_state;
        append(members, line);
        members += ';';
    }
    out += members;
    return out;
}

} // namespace hailstone::cortex
