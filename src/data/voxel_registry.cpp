#include "data/voxel_registry.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <SDL3/SDL.h>

namespace fs = std::filesystem;
using json   = nlohmann::json;

bool VoxelRegistry::load_directory(const std::string& path)
{
    bool ok = true;
    for (const auto& entry : fs::recursive_directory_iterator(path)) {
        if (entry.path().extension() == ".json")
            ok &= load_file(entry.path().string());
    }
    return ok;
}

bool VoxelRegistry::load_file(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        SDL_Log("VoxelRegistry: cannot open %s", path.c_str());
        return false;
    }
    json j;
    try { f >> j; }
    catch (const json::exception& e) {
        SDL_Log("VoxelRegistry: JSON error in %s: %s", path.c_str(), e.what());
        return false;
    }

    auto parse_one = [&](const json& obj) -> bool {
        try {
            VoxelTypeDef def;
            def.id        = obj.at("id").get<std::string>();
            def.name      = obj.value("name", def.id);
            def.parent_id = obj.value("parent", "");
            def.icon      = obj.value("icon", "");
            def.health    = obj.value("health", 100);
            def.material  = obj.value("material", "");
            def.on_hit    = obj.value("on_hit", "");
            def.on_walk   = obj.value("on_walk", "");
            def.emit_light= static_cast<uint8_t>(obj.value("emit_light", 0));
            def.tex_top     = obj.value("texture_top",    "");
            def.tex_bottom  = obj.value("texture_bottom", "");
            def.tex_sides   = obj.value("texture_sides",  "");

            if (obj.contains("flags"))
                for (const auto& flag : obj["flags"]) {
                    std::string fs = flag.get<std::string>();
                    if (fs == "SOLID")      def.default_flags |= VFLAG_SOLID;
                    if (fs == "OPAQUE")     def.default_flags |= VFLAG_OPAQUE;
                    if (fs == "PASSABLE")   def.default_flags |= VFLAG_PASSABLE;
                    if (fs == "CLIMBABLE")  def.default_flags |= VFLAG_CLIMBABLE;
                    if (fs == "FLAT_PLANE")    def.default_flags |= VFLAG_FLAT_PLANE;
                    if (fs == "FLAT_TOP")       def.default_flags |= VFLAG_FLAT_TOP;
                    if (fs == "VERT_PLANE_Z")   def.default_flags |= VFLAG_VERT_PLANE_Z;
                    if (fs == "GAS_PASSABLE")   def.default_flags |= VFLAG_GAS_PASSABLE;
                }

            def.type_id = m_next_id++;
            m_by_name[def.id] = def;
            m_by_id[def.type_id] = def;
            return true;
        } catch (const json::exception& e) {
            SDL_Log("VoxelRegistry: parse error in %s: %s", path.c_str(), e.what());
            return false;
        }
    };

    if (j.is_array()) {
        bool ok = true;
        for (auto& item : j) ok &= parse_one(item);
        return ok;
    }
    return parse_one(j);
}

void VoxelRegistry::resolve_inheritance(VoxelTypeDef& def)
{
    if (def.parent_id.empty()) return;
    const VoxelTypeDef* parent = get(def.parent_id);
    if (!parent) return;

    // Inherit fields that are zero/empty
    if (!def.default_flags) def.default_flags = parent->default_flags;
    if (!def.default_tags)  def.default_tags  = parent->default_tags;
    if (def.health == 100)  def.health        = parent->health;
    if (def.material.empty())def.material     = parent->material;
    if (def.on_hit.empty()) def.on_hit        = parent->on_hit;
    if (def.tex_top.empty())    def.tex_top    = parent->tex_top;
    if (def.tex_bottom.empty()) def.tex_bottom = parent->tex_bottom;
    if (def.tex_sides.empty())  def.tex_sides  = parent->tex_sides;
}

const VoxelTypeDef* VoxelRegistry::get(uint16_t type_id) const
{
    auto it = m_by_id.find(type_id);
    return it != m_by_id.end() ? &it->second : nullptr;
}

const VoxelTypeDef* VoxelRegistry::get(const std::string& id) const
{
    auto it = m_by_name.find(id);
    return it != m_by_name.end() ? &it->second : nullptr;
}

bool VoxelRegistry::has(const std::string& id) const
{
    return m_by_name.count(id) > 0;
}

uint16_t VoxelRegistry::id_of(const std::string& name_id) const
{
    auto it = m_by_name.find(name_id);
    return it != m_by_name.end() ? it->second.type_id : 0;
}

void VoxelRegistry::set_atlas_indices(
    uint16_t type_id,
    const std::array<uint16_t, static_cast<int>(FaceDir::COUNT)>& indices)
{
    auto it_id = m_by_id.find(type_id);
    if (it_id != m_by_id.end()) {
        it_id->second.atlas_indices = indices;
        // Sync the name-keyed copy too
        auto it_nm = m_by_name.find(it_id->second.id);
        if (it_nm != m_by_name.end())
            it_nm->second.atlas_indices = indices;
    }
}
