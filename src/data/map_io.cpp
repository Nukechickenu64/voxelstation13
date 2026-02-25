#include "data/map_io.h"
#include "core/entity_manager.h"
#include "simulation/world_items.h"
#include "simulation/mob_system.h"
#include "simulation/physics.h"
#include "simulation/model_objects.h"
#include "inventory/item_registry.h"
#include "data/mob_species_registry.h"
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

// ─────────────────────────────────────────────────────────────────────────────
bool map_save_full(World& world,
                   const VoxelRegistry&      voxel_reg,
                   EntityManager&            entities,
                   const ItemRegistry&       item_reg,
                   const ModelObjectManager& model_objs,
                   const std::string&        path)
{
    (void)item_reg;  // item id is already stored in WorldItemComponent; registry not needed for save
    // ── voxels (same as v1) ───────────────────────────────────────────────────
    json voxels_arr = json::array();
    world.for_each_chunk([&](const Chunk& chunk) {
        glm::ivec3 cp = chunk.chunk_pos();
        for (int lx = 0; lx < CHUNK_SIZE; ++lx)
        for (int ly = 0; ly < CHUNK_SIZE; ++ly)
        for (int lz = 0; lz < CHUNK_SIZE; ++lz) {
            Voxel v = chunk.get(lx, ly, lz);
            if (v.type_id == 0) continue;
            const VoxelTypeDef* def = voxel_reg.get(v.type_id);
            glm::ivec3 wp = cp * CHUNK_SIZE + glm::ivec3(lx, ly, lz);
            json entry;
            entry["x"]    = wp.x;
            entry["y"]    = wp.y;
            entry["z"]    = wp.z;
            entry["type"] = def ? def->id : std::to_string(v.type_id);
            if (v.orientation != 0) entry["orientation"] = v.orientation;
            voxels_arr.push_back(std::move(entry));
        }
    });

    // ── world items ───────────────────────────────────────────────────────────
    json items_arr = json::array();
    entities.each<WorldItemComponent>([&](EntityID /*id*/, WorldItemComponent& wic) {
        if (!wic.item.def) return;
        glm::vec3 pos = WorldItemSystem::item_world_pos(wic);
        json entry;
        entry["x"]         = pos.x;
        entry["y"]         = pos.y;
        entry["z"]         = pos.z;
        entry["item"]      = wic.item.def->id;
        entry["count"]     = wic.item.count;
        entry["integrity"] = wic.item.integrity;
        items_arr.push_back(std::move(entry));
    });

    // ── mobs ──────────────────────────────────────────────────────────────────
    json mobs_arr = json::array();
    entities.each<MobComponent>([&](EntityID id, MobComponent& mob) {
        auto* tr = entities.get_component<TransformComponent>(id);
        if (!tr) return;
        json entry;
        entry["x"]       = tr->pos.x;
        entry["y"]       = tr->pos.y;
        entry["z"]       = tr->pos.z;
        entry["yaw"]     = tr->yaw;
        entry["species"] = mob.species;
        entry["variant"] = mob.variant;
        mobs_arr.push_back(std::move(entry));
    });

    // ── model objects ─────────────────────────────────────────────────────────
    json objs_arr = json::array();
    for (const auto& obj : model_objs.objects()) {
        json entry;
        entry["name"]       = obj.name;
        entry["cx"]         = obj.cell.x;
        entry["cy"]         = obj.cell.y;
        entry["cz"]         = obj.cell.z;
        entry["yaw"]        = obj.yaw;
        entry["scale"]      = obj.scale;
        entry["blocks_mobs"]= obj.blocks_mobs;
        entry["blocks_gas"] = obj.blocks_gas;
        objs_arr.push_back(std::move(entry));
    }

    json root;
    root["version"]       = 2;
    root["voxels"]        = voxels_arr;
    root["world_items"]   = items_arr;
    root["mobs"]          = mobs_arr;
    root["model_objects"] = objs_arr;

    {
        size_t sep = path.find_last_of("/\\");
        if (sep != std::string::npos) SDL_CreateDirectory(path.substr(0, sep).c_str());
    }
    std::ofstream f(path);
    if (!f.is_open()) {
        SDL_Log("map_save_full: cannot open '%s' for writing", path.c_str());
        return false;
    }
    f << root.dump(2);
    SDL_Log("map_save_full: saved %d voxels, %d items, %d mobs, %d model_objs to '%s'",
            (int)voxels_arr.size(), (int)items_arr.size(),
            (int)mobs_arr.size(),  (int)objs_arr.size(), path.c_str());
    return f.good();
}

// ─────────────────────────────────────────────────────────────────────────────
bool map_load_full(World&                    world,
                   const VoxelRegistry&      voxel_reg,
                   EntityManager&            entities,
                   WorldItemSystem&          world_items,
                   const ItemRegistry&       item_reg,
                   const MobSpeciesRegistry& mob_reg,
                   ModelObjectManager&       model_objs,
                   const std::string&        path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        SDL_Log("map_load_full: cannot open '%s'", path.c_str());
        return false;
    }
    json root;
    try { root = json::parse(f); }
    catch (const std::exception& e) {
        SDL_Log("map_load_full: JSON parse error: %s", e.what());
        return false;
    }

    // ── Clear everything ──────────────────────────────────────────────────────
    world.clear_all();

    {
        std::vector<EntityID> to_kill;
        entities.each<WorldItemComponent>([&](EntityID id, WorldItemComponent&)
            { to_kill.push_back(id); });
        entities.each<MobComponent>([&](EntityID id, MobComponent&)
            { to_kill.push_back(id); });
        for (EntityID id : to_kill) entities.destroy(id);
    }
    model_objs.clear();

    // ── Voxels ────────────────────────────────────────────────────────────────
    int loaded_v = 0, skipped_v = 0;
    if (root.contains("voxels") && root["voxels"].is_array()) {
        for (const auto& entry : root["voxels"]) {
            if (!entry.contains("x") || !entry.contains("y") ||
                !entry.contains("z") || !entry.contains("type")) { ++skipped_v; continue; }
            int x = entry["x"].get<int>();
            int y = entry["y"].get<int>();
            int z = entry["z"].get<int>();
            auto ts = entry["type"].get<std::string>();
            uint16_t tid = voxel_reg.id_of(ts);
            if (tid == 0) { ++skipped_v; continue; }
            const VoxelTypeDef* def = voxel_reg.get(tid);
            Voxel v;
            v.type_id = tid;
            v.flags   = def ? def->default_flags : static_cast<uint16_t>(VFLAG_SOLID | VFLAG_OPAQUE);
            if (entry.contains("orientation")) v.orientation = entry["orientation"].get<uint8_t>();
            world.set_voxel({x, y, z}, v);
            ++loaded_v;
        }
    }

    // ── World items ───────────────────────────────────────────────────────────
    int loaded_i = 0;
    if (root.contains("world_items") && root["world_items"].is_array()) {
        for (const auto& e : root["world_items"]) {
            if (!e.contains("item")) continue;
            const ItemDef* def = item_reg.get(e["item"].get<std::string>());
            if (!def) continue;
            ItemStack st;
            st.def       = def;
            st.count     = e.value("count", 1);
            st.integrity = e.value("integrity", 1.f);
            glm::vec3 pos{
                e.value("x", 0.f),
                e.value("y", 0.f),
                e.value("z", 0.f)
            };
            world_items.spawn_floating(pos, std::move(st));
            ++loaded_i;
        }
    }

    // ── Mobs ──────────────────────────────────────────────────────────────────
    int loaded_m = 0;
    if (root.contains("mobs") && root["mobs"].is_array()) {
        for (const auto& e : root["mobs"]) {
            std::string species = e.value("species", "human");
            std::string variant = e.value("variant", "female");
            const MobSpeciesDef* sdef = mob_reg.get(species);

            EntityID id = entities.create();
            TransformComponent tr{};
            tr.pos = { e.value("x", 0.f), e.value("y", 1.f), e.value("z", 0.f) };
            tr.yaw = e.value("yaw", 0.f);
            entities.add_component<TransformComponent>(id, tr);

            MobComponent mob{};
            mob.species = species;
            mob.variant = variant;
            entities.add_component<MobComponent>(id, mob);

            HealthComponent hp{};
            hp.health_max = sdef ? sdef->health_max : 100.f;
            entities.add_component<HealthComponent>(id, hp);
            ++loaded_m;
        }
    }

    // ── Model objects ─────────────────────────────────────────────────────────
    int loaded_o = 0;
    if (root.contains("model_objects") && root["model_objects"].is_array()) {
        for (const auto& e : root["model_objects"]) {
            if (!e.contains("name")) continue;
            StaticModelObject def;
            def.name       = e["name"].get<std::string>();
            def.cell       = { e.value("cx",0), e.value("cy",0), e.value("cz",0) };
            def.yaw        = e.value("yaw",   0.f);
            def.scale      = e.value("scale", 1.f);
            def.blocks_mobs= e.value("blocks_mobs", true);
            def.blocks_gas = e.value("blocks_gas",  false);
            model_objs.add(def);
            ++loaded_o;
        }
    }

    SDL_Log("map_load_full: %d voxels, %d items, %d mobs, %d model_objs (%d vox skipped) from '%s'",
            loaded_v, loaded_i, loaded_m, loaded_o, skipped_v, path.c_str());
    return true;
}
