#pragma once
#include <string>
#include <vector>
#include <unordered_map>

// Equipment slot descriptor loaded from a mob species JSON file.
struct SpeciesSlotDef {
    std::string              id;
    std::vector<std::string> accepts;   // tag filter list; "*" = anything
};

// Full descriptor for one mob species, resolved from its JSON file.
struct MobSpeciesDef {
    std::string              species;       // primary key, e.g. "human"
    std::string              name;          // display name
    float                    health_max   = 100.f;
    float                    move_speed   = 4.5f;
    float                    sprint_mult  = 1.8f;
    float                    jump_vel     = 6.f;
    float                    height       = 1.8f;
    float                    radius       = 0.4f;
    std::vector<std::string> damage_types;  // e.g. ["brute","burn","tox","oxy"]
    std::vector<SpeciesSlotDef> slots;
};

// Loads all mob_species/*.json files.  Provides lookup by species id.
class MobSpeciesRegistry {
public:
    MobSpeciesRegistry() = default;

    bool load_directory(const std::string& path);
    bool load_file(const std::string& path);

    const MobSpeciesDef* get(const std::string& species) const;

    const std::unordered_map<std::string, MobSpeciesDef>& all() const { return m_defs; }

private:
    std::unordered_map<std::string, MobSpeciesDef> m_defs;
};
