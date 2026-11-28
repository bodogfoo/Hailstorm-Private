#include "epic/epic_definitions.h"

#include <algorithm>
#include <format>
#include <sstream>

#include "core/ascii_case.h"
#include "core/atomic_file.h"
#include "core/json_value.h"

namespace hailstone::epic {
namespace {

using core::JsonArray;
using core::JsonObject;
using core::JsonValue;

void read_str_array(const JsonValue* v, std::vector<std::string>& out) {
    out.clear();
    if (!v || !v->is_array()) return;
    for (const auto& elem : std::get<JsonArray>(v->data)) {
        std::string s = elem.as_string();
        if (!s.empty()) out.push_back(std::move(s));
    }
}

void read_u32_array(const JsonValue* v, std::vector<std::uint32_t>& out) {
    out.clear();
    if (!v || !v->is_array()) return;
    for (const auto& elem : std::get<JsonArray>(v->data)) {
        std::uint32_t id = elem.as_u32(0);
        if (id != 0) out.push_back(id);
    }
}

EpicItemRequirement parse_item_req(const JsonValue& v) {
    EpicItemRequirement r;
    if (const auto* f = v.find_ci("item_id")) r.item_id = f->as_u32(0);
    if (const auto* f = v.find_ci("item_name")) r.item_name = f->as_string();
    if (const auto* f = v.find_ci("quantity")) r.quantity = std::max(1, f->as_int(1));
    read_u32_array(v.find_ci("alternative_item_ids"), r.alternative_item_ids);
    read_str_array(v.find_ci("alternative_item_names"), r.alternative_item_names);
    if (const auto* f = v.find_ci("no_drop")) r.no_drop = f->as_bool(false);
    if (const auto* f = v.find_ci("lore")) r.lore = f->as_bool(false);
    if (const auto* f = v.find_ci("tradeable")) r.tradeable = f->as_bool(true);
    if (const auto* f = v.find_ci("temporary")) r.temporary = f->as_bool(false);
    if (const auto* f = v.find_ci("quest_turn_in")) r.quest_turn_in = f->as_bool(false);
    if (const auto* f = v.find_ci("optional")) r.optional = f->as_bool(false);
    if (const auto* f = v.find_ci("obsolete_after_completion")) r.obsolete_after_completion = f->as_bool(true);
    if (const auto* f = v.find_ci("useful_for_others")) r.useful_for_others = f->as_bool(false);
    if (const auto* f = v.find_ci("source_zone")) r.source_zone = f->as_string();
    if (const auto* f = v.find_ci("source_npc")) r.source_npc = f->as_string();
    if (const auto* f = v.find_ci("notes")) r.notes = f->as_string();
    return r;
}

EpicStageDefinition parse_stage(const JsonValue& v, int index) {
    EpicStageDefinition st;
    if (const auto* f = v.find_ci("stage_id")) st.stage_id = f->as_string();
    if (st.stage_id.empty()) st.stage_id = std::format("stage_{}", index + 1);
    if (const auto* f = v.find_ci("order")) st.order = f->as_int(index + 1);
    else st.order = index + 1;
    if (const auto* f = v.find_ci("name")) st.name = f->as_string();
    if (const auto* f = v.find_ci("description")) st.description = f->as_string();
    read_str_array(v.find_ci("prerequisites"), st.prerequisites);
    read_str_array(v.find_ci("required_npcs"), st.required_npcs);
    read_str_array(v.find_ci("required_zones"), st.required_zones);
    read_str_array(v.find_ci("turn_in_npcs"), st.turn_in_npcs);
    read_str_array(v.find_ci("turn_in_items"), st.turn_in_items);
    read_u32_array(v.find_ci("completion_reward_ids"), st.completion_reward_ids);
    read_str_array(v.find_ci("completion_reward_names"), st.completion_reward_names);
    if (const auto* f = v.find_ci("is_completion_stage")) st.is_completion_stage = f->as_bool(false);
    if (const auto* f = v.find_ci("required_items"); f && f->is_array()) {
        for (const auto& iv : std::get<JsonArray>(f->data)) {
            if (iv.is_object()) st.required_items.push_back(parse_item_req(iv));
        }
    }
    return st;
}

ServerRuleOverride parse_server_rule(const JsonValue& v) {
    ServerRuleOverride r;
    if (const auto* f = v.find_ci("server_pattern")) r.server_pattern = f->as_string();
    if (const auto* f = v.find_ci("notes")) r.notes = f->as_string();
    read_str_array(v.find_ci("skip_stages"), r.skip_stages);
    read_u32_array(v.find_ci("extra_completion_item_ids"), r.extra_completion_item_ids);
    read_str_array(v.find_ci("extra_completion_item_names"), r.extra_completion_item_names);
    return r;
}

EpicDefinition parse_epic_def(const JsonValue& v) {
    EpicDefinition d;
    if (const auto* f = v.find_ci("epic_id")) d.epic_id = f->as_string();
    if (const auto* f = v.find_ci("family")) d.family = f->as_string();
    if (const auto* f = v.find_ci("variant_name")) d.variant_name = f->as_string();
    if (const auto* f = v.find_ci("version")) d.version = f->as_int(1);
    read_str_array(v.find_ci("class_restrictions"), d.class_restrictions);
    if (const auto* f = v.find_ci("server_rules"); f && f->is_array()) {
        for (const auto& rv : std::get<JsonArray>(f->data)) {
            if (rv.is_object()) d.server_rules.push_back(parse_server_rule(rv));
        }
    }
    if (const auto* f = v.find_ci("stages"); f && f->is_array()) {
        const auto& arr = std::get<JsonArray>(f->data);
        for (std::size_t i = 0; i < arr.size(); ++i) {
            if (arr[i].is_object()) d.stages.push_back(parse_stage(arr[i], static_cast<int>(i)));
        }
        std::sort(d.stages.begin(), d.stages.end(), [](const auto& a, const auto& b) {
            return a.order < b.order;
        });
    }
    read_u32_array(v.find_ci("completion_item_ids"), d.completion_item_ids);
    read_str_array(v.find_ci("completion_item_names"), d.completion_item_names);
    if (const auto* f = v.find_ci("upgrade_next_epic_id")) d.upgrade_next_epic_id = f->as_string();
    read_u32_array(v.find_ci("never_loot_after_completion_ids"), d.never_loot_after_completion_ids);
    read_str_array(v.find_ci("never_loot_after_completion_names"), d.never_loot_after_completion_names);
    read_u32_array(v.find_ci("shared_useful_item_ids"), d.shared_useful_item_ids);
    read_str_array(v.find_ci("shared_useful_item_names"), d.shared_useful_item_names);
    return d;
}

void write_str_array(std::ostringstream& out, const std::vector<std::string>& items) {
    out << "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out << ", ";
        out << core::json_escape(items[i]);
    }
    out << "]";
}

void write_u32_array(std::ostringstream& out, const std::vector<std::uint32_t>& items) {
    out << "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out << ", ";
        out << items[i];
    }
    out << "]";
}

EpicItemRequirement make_req(std::uint32_t id, std::string name, int qty, bool no_drop, bool lore,
                             std::string zone, std::string npc = {}, bool quest_turn_in = true) {
    EpicItemRequirement r;
    r.item_id = id;
    r.item_name = std::move(name);
    r.quantity = qty;
    r.no_drop = no_drop;
    r.lore = lore;
    r.tradeable = !no_drop;
    r.quest_turn_in = quest_turn_in;
    r.source_zone = std::move(zone);
    r.source_npc = std::move(npc);
    return r;
}

EpicDefinition make_simple_class_epic(std::string id, std::string family, std::string variant,
                                      std::string cls, std::uint32_t comp_id, std::string comp_name,
                                      std::vector<EpicStageDefinition> stages,
                                      std::string next_id = {}) {
    EpicDefinition d;
    d.epic_id = std::move(id);
    d.family = std::move(family);
    d.variant_name = std::move(variant);
    d.class_restrictions = {std::move(cls)};
    d.stages = std::move(stages);
    if (comp_id != 0) d.completion_item_ids.push_back(comp_id);
    if (!comp_name.empty()) d.completion_item_names.push_back(std::move(comp_name));
    d.upgrade_next_epic_id = std::move(next_id);
    return d;
}

} // namespace

void EpicDefinitionCatalog::clear() {
    definitions_.clear();
}

void EpicDefinitionCatalog::load_builtin_defaults() {
    definitions_.clear();

    // Warrior Epic 1.0 (Classic)
    {
        std::vector<EpicStageDefinition> stages;
        EpicStageDefinition s1;
        s1.stage_id = "stage_1_briar";
        s1.order = 1;
        s1.name = "Red Dragon Scales & Green Dragon Scales";
        s1.required_zones = {"nagafen", "skyfire"};
        s1.required_items = {
            make_req(11622, "Red Dragon Scales", 1, true, true, "nagafen", "Lord Nagafen"),
            make_req(11623, "Green Dragon Scales", 1, true, true, "skyfire", "Severilous"),
        };
        stages.push_back(std::move(s1));

        EpicStageDefinition s2;
        s2.stage_id = "stage_2_blades";
        s2.order = 2;
        s2.name = "Jagged Blade of War Components";
        s2.prerequisites = {"stage_1_briar"};
        s2.required_zones = {"planeofair", "karnor"};
        s2.required_items = {
            make_req(20669, "Spiroc Wingblade", 1, true, true, "planeofair"),
            make_req(20670, "Ancient Blade", 1, true, true, "karnor"),
        };
        s2.is_completion_stage = true;
        s2.completion_reward_ids = {10908};
        s2.completion_reward_names = {"Jagged Blade of War"};
        stages.push_back(std::move(s2));

        definitions_.push_back(make_simple_class_epic(
            "war_1_0", "EQ1", "Epic 1.0", "WAR", 10908, "Jagged Blade of War",
            std::move(stages), "war_ez_v2"));
    }

    // Warrior EZ Server Variant 2 (multi-stage progression with Stage 6 turn-in)
    {
        std::vector<EpicStageDefinition> stages;
        const char* stage_names[] = {
            "Qvic Stone & Mold",
            "Txevu Essence",
            "Anguish Signet",
            "Abyssal Shard",
            "Dragorn War Crest",
            "Champion's Final Seal",
        };
        const char* zones[] = {"qvic", "txevu", "anguish", "abyss", "tacvi", "anguish"};
        for (int i = 0; i < 6; ++i) {
            EpicStageDefinition st;
            st.stage_id = std::format("stage_{}", i + 1);
            st.order = i + 1;
            st.name = stage_names[i];
            if (i > 0) st.prerequisites = {std::format("stage_{}", i)};
            st.required_zones = {zones[i]};
            auto req = make_req(static_cast<std::uint32_t>(91001 + i),
                                std::format("Warrior Epic {}", stage_names[i]),
                                1, true, true, zones[i]);
            if (i == 5) {
                req.item_id = 91006;
                req.item_name = "Champion's Final Seal";
                req.alternative_item_ids = {91099};
                req.alternative_item_names = {"Champion's Radiant Seal"};
            }
            st.required_items.push_back(std::move(req));
            if (i == 5) {
                st.is_completion_stage = true;
                st.completion_reward_ids = {91100};
                st.completion_reward_names = {"Blade of the Eternal Champion"};
            }
            stages.push_back(std::move(st));
        }
        auto def = make_simple_class_epic(
            "war_ez_v2", "EZ_Server", "Variant 2", "WAR",
            91100, "Blade of the Eternal Champion", std::move(stages));
        def.server_rules.push_back({"ez", "EZ Server multi-tier Warrior epic", {}, {}, {}});
        definitions_.push_back(std::move(def));
    }

    // Cleric Epic 1.0 & EZ Variant
    {
        EpicStageDefinition s1;
        s1.stage_id = "stage_1";
        s1.order = 1;
        s1.name = "Singed Scroll & Pearlescent Fragment";
        s1.required_zones = {"burningwood", "skyfire"};
        s1.required_items = {
            make_req(28001, "Singed Scroll", 1, true, true, "burningwood"),
            make_req(28002, "Pearlescent Fragment", 1, true, true, "skyfire"),
        };
        EpicStageDefinition s2;
        s2.stage_id = "stage_2";
        s2.order = 2;
        s2.name = "Orb of Clear Water";
        s2.prerequisites = {"stage_1"};
        s2.required_zones = {"timorous"};
        s2.required_items = {make_req(28003, "Orb of Clear Water", 1, true, true, "timorous")};
        s2.is_completion_stage = true;
        s2.completion_reward_ids = {5532};
        s2.completion_reward_names = {"Water Sprinkler of Nem Ankh"};
        definitions_.push_back(make_simple_class_epic(
            "clr_1_0", "EQ1", "Epic 1.0", "CLR", 5532, "Water Sprinkler of Nem Ankh",
            {std::move(s1), std::move(s2)}, "clr_ez_2_0"));
    }

    // Remaining standard EQ1/EZ class epics so every class has a baseline variant
    struct ClassSeed {
        const char* id;
        const char* cls;
        const char* variant;
        std::uint32_t item_id;
        const char* item_name;
        std::uint32_t comp_id;
        const char* comp_name;
        const char* zone;
    };
    const ClassSeed seeds[] = {
        {"clr_ez_2_0", "CLR", "Epic 2.0", 92001, "Aegis of Harmony Fragment", 20076, "Aegis of Superior Divinity", "anguish"},
        {"pal_1_0", "PAL", "Epic 1.0", 11044, "SoulFire", 10099, "Fiery Defender", "skyfire"},
        {"shd_1_0", "SHD", "Epic 1.0", 14380, "Soul Leech, Dark Sword of Blood", 14383, "Innoruuk's Curse", "hole"},
        {"rng_1_0", "RNG", "Epic 1.0", 20487, "Shattered Emerald of Corruption", 20488, "Swiftwind", "planeofhate"},
        {"dru_1_0", "DRU", "Epic 1.0", 20490, "Pod of Seawater", 20490, "Nature Walkers Scimitar", "kedge"},
        {"mnk_1_0", "MNK", "Epic 1.0", 10651, "Celestial Fists", 10652, "Celestial Fists", "chardok"},
        {"brd_1_0", "BRD", "Epic 1.0", 20541, "Kedge Backbone", 20542, "Singing Short Sword", "kedge"},
        {"rog_1_0", "ROG", "Epic 1.0", 11056, "Stained Parchment", 11057, "Ragebringer", "kedge"},
        {"shm_1_0", "SHM", "Epic 1.0", 10649, "Child's Tear", 10650, "Spear of Fate", "fearplane"},
        {"nec_1_0", "NEC", "Epic 1.0", 20543, "Tome of Miragul", 20544, "Scythe of the Shadowed Soul", "everfrost"},
        {"wiz_1_0", "WIZ", "Epic 1.0", 14340, "Blue Crystal Staff", 14341, "Staff of the Four", "kedge"},
        {"mag_1_0", "MAG", "Epic 1.0", 28033, "Crown of Elemental Mastery", 28034, "Orb of Mastery", "planeofair"},
        {"enc_1_0", "ENC", "Epic 1.0", 10648, "Prince Selrach's Head", 10650, "Staff of the Serpent", "chardok"},
        {"bst_1_5", "BST", "Epic 1.5", 52910, "Totem of the Feral", 52911, "Savage Lord's Totem", "anguish"},
        {"ber_1_5", "BER", "Epic 1.5", 18397, "Bloodied Axe Head", 18398, "Raging Taelosian Alloy Axe", "anguish"},
    };

    for (const auto& s : seeds) {
        EpicStageDefinition st;
        st.stage_id = "stage_1";
        st.order = 1;
        st.name = std::string(s.cls) + " " + s.variant + " Primary Quest";
        st.required_zones = {s.zone};
        st.required_items = {make_req(s.item_id, s.item_name, 1, true, true, s.zone)};
        st.is_completion_stage = true;
        st.completion_reward_ids = {s.comp_id};
        st.completion_reward_names = {s.comp_name};
        definitions_.push_back(make_simple_class_epic(
            s.id, "EQ1", s.variant, s.cls, s.comp_id, s.comp_name, {std::move(st)}));
    }
}

void EpicDefinitionCatalog::upsert(EpicDefinition def) {
    for (auto& existing : definitions_) {
        if (iequals(existing.epic_id, def.epic_id)) {
            existing = std::move(def);
            return;
        }
    }
    definitions_.push_back(std::move(def));
}

const EpicDefinition* EpicDefinitionCatalog::find_by_id(std::string_view epic_id) const {
    for (const auto& d : definitions_) {
        if (iequals(d.epic_id, epic_id)) return &d;
    }
    return nullptr;
}

const EpicDefinition* EpicDefinitionCatalog::find_for_character(std::string_view class_short,
                                                                  std::string_view variant_name,
                                                                  std::string_view server_name) const {
    const EpicDefinition* fallback = nullptr;
    for (const auto& d : definitions_) {
        if (!d.supports_class(class_short)) continue;
        if (!d.supports_server(server_name)) continue;
        if (!variant_name.empty()) {
            if (iequals(d.variant_name, variant_name) || iequals(d.epic_id, variant_name)) {
                return &d;
            }
        } else if (!fallback) {
            fallback = &d;
        }
    }
    return fallback;
}

std::vector<const EpicDefinition*> EpicDefinitionCatalog::variants_for_class(std::string_view class_short,
                                                                               std::string_view server_name) const {
    std::vector<const EpicDefinition*> out;
    for (const auto& d : definitions_) {
        if (d.supports_class(class_short) && d.supports_server(server_name)) {
            out.push_back(&d);
        }
    }
    return out;
}

bool EpicDefinitionCatalog::load_from_json_string(std::string_view json_text, std::string* error, bool merge) {
    JsonValue root;
    std::string err;
    if (!core::parse_json(json_text, root, err)) {
        if (error) *error = err;
        return false;
    }
    if (!root.is_object()) {
        if (error) *error = "Epic definitions root must be a JSON object";
        return false;
    }
    if (const auto* sv = root.find_ci("schema_version")) {
        schema_version_ = sv->as_int(kEpicSchemaVersion);
    }
    const auto* epics = root.find_ci("epics");
    if (!epics || !epics->is_array()) {
        if (error) *error = "Epic definitions JSON missing 'epics' array";
        return false;
    }
    if (!merge) definitions_.clear();
    for (const auto& ev : std::get<JsonArray>(epics->data)) {
        if (!ev.is_object()) continue;
        EpicDefinition def = parse_epic_def(ev);
        if (!def.epic_id.empty()) upsert(std::move(def));
    }
    return true;
}

bool EpicDefinitionCatalog::load_from_file(const std::filesystem::path& path, std::string* error, bool merge) {
    auto text = hailstone::read_text_file(path);
    if (!text) {
        if (error) *error = "Failed to read epic definitions file: " + path.string();
        return false;
    }
    return load_from_json_string(*text, error, merge);
}

std::string EpicDefinitionCatalog::to_json_string() const {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema_version\": " << schema_version_ << ",\n";
    out << "  \"epics\": [\n";
    for (std::size_t i = 0; i < definitions_.size(); ++i) {
        const auto& d = definitions_[i];
        out << "    {\n";
        out << "      \"epic_id\": " << core::json_escape(d.epic_id) << ",\n";
        out << "      \"family\": " << core::json_escape(d.family) << ",\n";
        out << "      \"variant_name\": " << core::json_escape(d.variant_name) << ",\n";
        out << "      \"version\": " << d.version << ",\n";
        out << "      \"class_restrictions\": ";
        write_str_array(out, d.class_restrictions);
        out << ",\n";
        out << "      \"completion_item_ids\": ";
        write_u32_array(out, d.completion_item_ids);
        out << ",\n";
        out << "      \"completion_item_names\": ";
        write_str_array(out, d.completion_item_names);
        out << ",\n";
        out << "      \"upgrade_next_epic_id\": " << core::json_escape(d.upgrade_next_epic_id) << ",\n";
        out << "      \"never_loot_after_completion_ids\": ";
        write_u32_array(out, d.never_loot_after_completion_ids);
        out << ",\n";
        out << "      \"never_loot_after_completion_names\": ";
        write_str_array(out, d.never_loot_after_completion_names);
        out << ",\n";
        out << "      \"shared_useful_item_ids\": ";
        write_u32_array(out, d.shared_useful_item_ids);
        out << ",\n";
        out << "      \"shared_useful_item_names\": ";
        write_str_array(out, d.shared_useful_item_names);
        out << ",\n";
        out << "      \"stages\": [\n";
        for (std::size_t j = 0; j < d.stages.size(); ++j) {
            const auto& st = d.stages[j];
            out << "        {\n";
            out << "          \"stage_id\": " << core::json_escape(st.stage_id) << ",\n";
            out << "          \"order\": " << st.order << ",\n";
            out << "          \"name\": " << core::json_escape(st.name) << ",\n";
            out << "          \"prerequisites\": ";
            write_str_array(out, st.prerequisites);
            out << ",\n";
            out << "          \"required_zones\": ";
            write_str_array(out, st.required_zones);
            out << ",\n";
            out << "          \"is_completion_stage\": " << (st.is_completion_stage ? "true" : "false") << ",\n";
            out << "          \"required_items\": [\n";
            for (std::size_t k = 0; k < st.required_items.size(); ++k) {
                const auto& r = st.required_items[k];
                out << "            {\"item_id\": " << r.item_id
                    << ", \"item_name\": " << core::json_escape(r.item_name)
                    << ", \"quantity\": " << r.quantity
                    << ", \"no_drop\": " << (r.no_drop ? "true" : "false")
                    << ", \"lore\": " << (r.lore ? "true" : "false")
                    << ", \"tradeable\": " << (r.tradeable ? "true" : "false")
                    << ", \"temporary\": " << (r.temporary ? "true" : "false")
                    << ", \"quest_turn_in\": " << (r.quest_turn_in ? "true" : "false")
                    << ", \"optional\": " << (r.optional ? "true" : "false")
                    << ", \"obsolete_after_completion\": " << (r.obsolete_after_completion ? "true" : "false")
                    << ", \"useful_for_others\": " << (r.useful_for_others ? "true" : "false")
                    << ", \"source_zone\": " << core::json_escape(r.source_zone)
                    << "}";
                if (k + 1 < st.required_items.size()) out << ",";
                out << "\n";
            }
            out << "          ]\n";
            out << "        }";
            if (j + 1 < d.stages.size()) out << ",";
            out << "\n";
        }
        out << "      ]\n";
        out << "    }";
        if (i + 1 < definitions_.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

bool EpicDefinitionCatalog::save_to_file(const std::filesystem::path& path, std::string* error) const {
    if (!hailstone::write_file_atomic(path, to_json_string())) {
        if (error) *error = "Failed to write epic definitions file: " + path.string();
        return false;
    }
    return true;
}

} // namespace hailstone::epic
