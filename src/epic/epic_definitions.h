#pragma once

// Versioned, data-driven epic quest definition catalog.
// Loads external JSON definitions (e.g. <data_dir>/epic_quests.json) and
// provides built-in EQ1 + EZ Server epic variants as defaults.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "epic/epic_types.h"

namespace hailstone::epic {

class EpicDefinitionCatalog {
public:
    void clear();
    void load_builtin_defaults();

    bool load_from_json_string(std::string_view json_text, std::string* error = nullptr, bool merge = true);
    bool load_from_file(const std::filesystem::path& path, std::string* error = nullptr, bool merge = true);
    bool save_to_file(const std::filesystem::path& path, std::string* error = nullptr) const;
    std::string to_json_string() const;

    void upsert(EpicDefinition def);

    const std::vector<EpicDefinition>& all() const { return definitions_; }
    const EpicDefinition* find_by_id(std::string_view epic_id) const;
    const EpicDefinition* find_for_character(std::string_view class_short,
                                             std::string_view variant_name,
                                             std::string_view server_name = {}) const;
    std::vector<const EpicDefinition*> variants_for_class(std::string_view class_short,
                                                            std::string_view server_name = {}) const;

    int schema_version() const { return schema_version_; }

private:
    int schema_version_ = kEpicSchemaVersion;
    std::vector<EpicDefinition> definitions_;
};

} // namespace hailstone::epic
