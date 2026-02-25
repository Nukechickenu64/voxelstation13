#include "data/map_io.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <SDL3/SDL.h>

using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────────
bool map_save(const World& world, const VoxelRegistry& reg, const std::string& path)
{
    json voxels_arr = json::array();

    world.for_each_chunk([&](const Chunk& chunk) {
        glm::ivec3 cp = chunk.chunk_pos();
        for (int lx = 0; lx < CHUNK_SIZE; ++lx)
        for (int ly = 0; ly < CHUNK_SIZE; ++ly)
        for (int lz = 0; lz < CHUNK_SIZE; ++lz) {
            Voxel v = chunk.get(lx, ly, lz);
            if (v.type_id == 0) continue;

            const VoxelTypeDef* def = reg.get(v.type_id);
            glm::ivec3 wp = cp * CHUNK_SIZE + glm::ivec3(lx, ly, lz);

            json entry;
            entry["x"]    = wp.x;
            entry["y"]    = wp.y;
            entry["z"]    = wp.z;
            entry["type"] = def ? def->id : std::to_string(v.type_id);
            if (v.orientation != 0)
                entry["orientation"] = v.orientation;
            voxels_arr.push_back(std::move(entry));
        }
    });

    json root;
    root["version"] = 1;
    root["voxels"]  = voxels_arr;

    // Ensure parent directory exists
    {
        size_t sep = path.find_last_of("/\\");
        if (sep != std::string::npos) {
            std::string dir = path.substr(0, sep);
            SDL_CreateDirectory(dir.c_str());
        }
    }

    std::ofstream f(path);
    if (!f.is_open()) {
        SDL_Log("map_save: cannot open '%s' for writing", path.c_str());
        return false;
    }
    f << root.dump(2);
    SDL_Log("map_save: saved %d voxels to '%s'",
            static_cast<int>(voxels_arr.size()), path.c_str());
    return f.good();
}

// ─────────────────────────────────────────────────────────────────────────────
bool map_load(World& world, const VoxelRegistry& reg, const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        SDL_Log("map_load: cannot open '%s'", path.c_str());
        return false;
    }

    json root;
    try {
        root = json::parse(f);
    } catch (const std::exception& e) {
        SDL_Log("map_load: JSON parse error in '%s': %s", path.c_str(), e.what());
        return false;
    }

    if (!root.contains("voxels") || !root["voxels"].is_array()) {
        SDL_Log("map_load: missing or invalid 'voxels' array in '%s'", path.c_str());
        return false;
    }

    // Wipe world before loading
    world.clear_all();

    int loaded = 0;
    int skipped = 0;
    for (const auto& entry : root["voxels"]) {
        if (!entry.contains("x") || !entry.contains("y") ||
            !entry.contains("z") || !entry.contains("type")) {
            ++skipped;
            continue;
        }

        int  x       = entry["x"].get<int>();
        int  y       = entry["y"].get<int>();
        int  z       = entry["z"].get<int>();
        auto type_str = entry["type"].get<std::string>();

        uint16_t type_id = reg.id_of(type_str);
        if (type_id == 0) {
            SDL_Log("map_load: unknown voxel type '%s' -- skipping", type_str.c_str());
            ++skipped;
            continue;
        }

        const VoxelTypeDef* def = reg.get(type_id);
        Voxel v;
        v.type_id = type_id;
        v.flags   = def ? def->default_flags
                        : static_cast<uint16_t>(VFLAG_SOLID | VFLAG_OPAQUE);
        if (entry.contains("orientation"))
            v.orientation = entry["orientation"].get<uint8_t>();

        world.set_voxel({x, y, z}, v);
        ++loaded;
    }

    SDL_Log("map_load: loaded %d voxels (%d skipped) from '%s'",
            loaded, skipped, path.c_str());
    return true;
}
