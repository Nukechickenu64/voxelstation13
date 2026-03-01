#include "data/mob_species_registry.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <SDL3/SDL.h>

namespace fs = std::filesystem;
using json   = nlohmann::json;

bool MobSpeciesRegistry::load_directory(const std::string& path)
{
    bool ok = true;
    for (const auto& entry : fs::recursive_directory_iterator(path)) {
        if (entry.path().extension() == ".json")
            ok &= load_file(entry.path().string());
    }
    return ok;
}

bool MobSpeciesRegistry::load_file(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        SDL_Log("MobSpeciesRegistry: cannot open %s", path.c_str());
        return false;
    }
    json j;
    try { f >> j; }
    catch (const json::exception& e) {
        SDL_Log("MobSpeciesRegistry: JSON error in %s: %s", path.c_str(), e.what());
        return false;
    }

    try {
        MobSpeciesDef def;
        def.species     = j.at("species").get<std::string>();
        def.name        = j.value("name",        def.species);
        def.health_max  = j.value("health_max",  100.f);
        def.move_speed  = j.value("move_speed",  2.25f);
        def.sprint_mult = j.value("sprint_mult", 2.0f);
        def.jump_vel    = j.value("jump_vel",    6.f);
        def.height      = j.value("height",      1.8f);
        def.radius      = j.value("radius",      0.4f);

        if (j.contains("damage_types"))
            for (const auto& dt : j["damage_types"])
                def.damage_types.push_back(dt.get<std::string>());

        if (j.contains("slots")) {
            for (const auto& s : j["slots"]) {
                SpeciesSlotDef sd;
                sd.id = s.at("id").get<std::string>();
                if (s.contains("accepts"))
                    for (const auto& a : s["accepts"])
                        sd.accepts.push_back(a.get<std::string>());
                def.slots.push_back(std::move(sd));
            }
        }

        SDL_Log("MobSpeciesRegistry: loaded species '%s'", def.species.c_str());
        m_defs[def.species] = std::move(def);
        return true;
    } catch (const json::exception& e) {
        SDL_Log("MobSpeciesRegistry: parse error in %s: %s", path.c_str(), e.what());
        return false;
    }
}

const MobSpeciesDef* MobSpeciesRegistry::get(const std::string& species) const
{
    auto it = m_defs.find(species);
    return (it != m_defs.end()) ? &it->second : nullptr;
}
