#include "inventory/item_registry.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <SDL3/SDL.h>

namespace fs = std::filesystem;
using json   = nlohmann::json;

bool ItemRegistry::load_directory(const std::string& path)
{
    bool ok = true;
    for (const auto& entry : fs::recursive_directory_iterator(path)) {
        if (entry.path().extension() == ".json")
            ok &= load_file(entry.path().string());
    }
    return ok;
}

bool ItemRegistry::load_file(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        SDL_Log("ItemRegistry: cannot open %s", path.c_str());
        return false;
    }
    json j;
    try {
        f >> j;
    } catch (const json::exception& e) {
        SDL_Log("ItemRegistry: JSON parse error in %s: %s", path.c_str(), e.what());
        return false;
    }

    // Support a single object or an array of items
    if (j.is_array()) {
        bool ok = true;
        for (auto& item : j) ok &= load_from_json(item, path);
        return ok;
    }
    return load_from_json(j, path);
}

bool ItemRegistry::load_from_json(const nlohmann::json& j, const std::string& src)
{
    try {
        ItemDef def;
        def.id        = j.at("id").get<std::string>();
        def.name      = j.at("name").get<std::string>();
        def.icon      = j.value("icon", "");
        def.weight    = j.value("weight", 0.1f);
        def.stack_max = j.value("stack_max", 1);
        def.is_container = j.value("is_container", false);

        if (j.contains("tags"))
            for (const auto& t : j["tags"])
                def.tags.push_back(t.get<std::string>());

        if (j.contains("verbs"))
            for (const auto& v : j["verbs"]) {
                ItemVerb verb;
                verb.name    = v.at("name").get<std::string>();
                verb.range   = v.value("range", 1.5f);
                verb.handler = v.value("handler", "");
                def.verbs.push_back(std::move(verb));
            }

        m_defs[def.id] = std::move(def);
        return true;
    } catch (const json::exception& e) {
        SDL_Log("ItemRegistry: error parsing item in %s: %s", src.c_str(), e.what());
        return false;
    }
}

const ItemDef* ItemRegistry::get(const std::string& id) const
{
    auto it = m_defs.find(id);
    return it != m_defs.end() ? &it->second : nullptr;
}

bool ItemRegistry::has(const std::string& id) const
{
    return m_defs.count(id) > 0;
}
