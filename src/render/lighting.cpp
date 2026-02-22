#include "render/lighting.h"
#include <queue>
#include <algorithm>

LightingSystem::LightingSystem(World& world)
    : m_world(world)
{}

void LightingSystem::rebuild()
{
    // TODO: iterate all chunks, propagate sky light top-down per column,
    //       then propagate block light from all emissive voxels.
    propagate_block();
}

void LightingSystem::update(const std::vector<glm::ivec3>& dirty_voxels)
{
    for (const auto& pos : dirty_voxels) {
        // Recompute sky column for the XZ of each dirty voxel
        propagate_sky({pos.x, pos.z});
        bfs_add(pos, MAX_LIGHT, false);
    }
}

void LightingSystem::propagate_sky(glm::ivec2 /*column*/)
{
    // Walk downward from the world ceiling; each solid voxel stops sky light.
    // TODO: determine world height bounds from chunk map and sweep.
}

void LightingSystem::propagate_block()
{
    // Seed with all voxels that have emit_light > 0
    // TODO: iterate voxel registry to find emissive types, then BFS.
}

void LightingSystem::bfs_add(glm::ivec3 pos, uint8_t level, bool /*sky*/)
{
    if (level == 0) return;
    std::queue<std::pair<glm::ivec3, uint8_t>> q;
    q.push({pos, level});

    static const glm::ivec3 dirs[6] = {
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };

    while (!q.empty()) {
        auto [p, l] = q.front(); q.pop();
        Voxel v = m_world.get_voxel(p);
        if (v.light_level >= l) continue;
        v.light_level = l;
        m_world.set_voxel(p, v);
        if (l <= 1) continue;
        for (auto& d : dirs) {
            glm::ivec3 np = p + d;
            Voxel nv = m_world.get_voxel(np);
            if (!(nv.flags & VFLAG_OPAQUE) && nv.light_level < l - 1)
                q.push({np, static_cast<uint8_t>(l - 1)});
        }
    }
}

void LightingSystem::bfs_remove(glm::ivec3 /*pos*/, bool /*sky*/,
                                 std::queue<glm::ivec3>& /*relight*/)
{
    // Dark-flood fill then re-light — see Seed-based algorithm
    // TODO: implement removal propagation
}

float LightingSystem::vertex_ao(glm::ivec3 voxel, FaceDir face, int corner) const
{
    // AO is approximated by counting solid neighbours around the vertex
    // Each corner of a face has 3 neighbours: side1, side2, corner_diag
    (void)voxel; (void)face; (void)corner;
    // TODO: implement 3-sample AO lookup
    return 1.0f;
}
