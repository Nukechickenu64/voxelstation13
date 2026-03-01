#pragma once
#include "inventory/inventory.h"
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <string>

// Loads and owns all ItemDef records from data/item_types/*.json
class ItemRegistry {
public:
    ItemRegistry() = default;

    bool load_directory(const std::string& path);
    bool load_file(const std::string& path);

    const ItemDef* get(const std::string& id) const;
    bool           has(const std::string& id) const;

    const std::unordered_map<std::string, ItemDef>& all() const { return m_defs; }

    // --- Prototype resolution ---
    // After all files are loaded, call this to:
    //   1. Fill in any fields left at default from the nearest PrototypeDef ancestor.
    //   2. Set ItemDef::prototype_parent to the nearest parent ItemDef (if any).
    //   3. Ensure every def has a valid type_path (derives one if missing).
    // Called automatically at the end of load_directory().
    void resolve_prototypes();

private:
    std::unordered_map<std::string, ItemDef> m_defs;
    bool load_from_json(const nlohmann::json& j, const std::string& src);
};
