#include "simulation/model_objects.h"
#include <cmath>
#include <algorithm>

void ModelObjectManager::register_extents(const std::string& name,
                                           glm::vec3 local_min,
                                           glm::vec3 local_max)
{
    m_extents[name] = { local_min, local_max };
}

int ModelObjectManager::add(StaticModelObject def)
{
    // World origin: horizontal centre of the cell, on the cell floor
    def.world_pos = glm::vec3(def.cell) + glm::vec3(0.5f, 0.f, 0.5f);

    auto it = m_extents.find(def.name);
    if (it != m_extents.end()) {
        // Scale local AABB and translate to world space
        glm::vec3 scaled_min = it->second.local_min * def.scale;
        glm::vec3 scaled_max = it->second.local_max * def.scale;
        glm::vec3 world_min  = def.world_pos + scaled_min;
        glm::vec3 world_max  = def.world_pos + scaled_max;

        // Snap to inclusive voxel range.
        // For the max edge: if the mesh reaches exactly integer boundary N,
        // it ends at that boundary (not inside cell N), so subtract epsilon.
        def.voxel_min = {
            (int)std::floor(world_min.x),
            (int)std::floor(world_min.y),
            (int)std::floor(world_min.z)
        };
        def.voxel_max = {
            (int)std::floor(world_max.x - 1e-4f),
            (int)std::floor(world_max.y - 1e-4f),
            (int)std::floor(world_max.z - 1e-4f)
        };

        // Ensure max >= min in each axis (handles zero-extent models)
        def.voxel_max.x = std::max(def.voxel_max.x, def.voxel_min.x);
        def.voxel_max.y = std::max(def.voxel_max.y, def.voxel_min.y);
        def.voxel_max.z = std::max(def.voxel_max.z, def.voxel_min.z);
    } else {
        // No extents registered — default to a single 1×1×1 voxel at the cell
        def.voxel_min = def.cell;
        def.voxel_max = def.cell;
    }

    const int id = m_next_id++;
    m_ids.push_back(id);
    m_objects.push_back(std::move(def));
    return id;
}

void ModelObjectManager::remove(int id)
{
    for (int i = 0; i < (int)m_ids.size(); ++i) {
        if (m_ids[i] == id) {
            m_ids.erase(m_ids.begin() + i);
            m_objects.erase(m_objects.begin() + i);
            return;
        }
    }
}

void ModelObjectManager::clear()
{
    m_objects.clear();
    m_ids.clear();
}

bool ModelObjectManager::blocks_mob_at(glm::ivec3 c) const
{
    for (const auto& obj : m_objects) {
        if (!obj.blocks_mobs) continue;
        if (c.x >= obj.voxel_min.x && c.x <= obj.voxel_max.x &&
            c.y >= obj.voxel_min.y && c.y <= obj.voxel_max.y &&
            c.z >= obj.voxel_min.z && c.z <= obj.voxel_max.z)
            return true;
    }
    return false;
}

bool ModelObjectManager::blocks_gas_at(glm::ivec3 c) const
{
    for (const auto& obj : m_objects) {
        if (!obj.blocks_gas) continue;
        if (c.x >= obj.voxel_min.x && c.x <= obj.voxel_max.x &&
            c.y >= obj.voxel_min.y && c.y <= obj.voxel_max.y &&
            c.z >= obj.voxel_min.z && c.z <= obj.voxel_max.z)
            return true;
    }
    return false;
}
