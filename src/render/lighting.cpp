#include "render/lighting.h"
#include "data/voxel_registry.h"
#include <queue>
#include <algorithm>

// Six cardinal directions used by BFS.
static const glm::ivec3 k_dirs[6] = {
    {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
};

// â”€â”€ LightMap â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void LightMap::set(glm::ivec3 pos, LightColor c)
{
    if (c.r == 255 && c.g == 255 && c.b == 255)
        m_data.erase(pos);
    else
        m_data[pos] = c;
}

LightColor LightMap::get(glm::ivec3 pos) const
{
    auto it = m_data.find(pos);
    return it != m_data.end() ? it->second : LightColor{255, 255, 255};
}

// â”€â”€ LightingSystem â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

LightingSystem::LightingSystem(World& world)
    : m_world(world)
{}

// â”€â”€ Helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// Blend two colors weighted by their respective light levels.
// The brighter source dominates; equal levels produce an average.
static LightColor blend_colors(LightColor a, uint8_t la, LightColor b, uint8_t lb)
{
    int total = la + lb;
    if (total == 0) return LightColor{255, 255, 255};
    return {
        static_cast<uint8_t>((a.r * la + b.r * lb) / total),
        static_cast<uint8_t>((a.g * la + b.g * lb) / total),
        static_cast<uint8_t>((a.b * la + b.b * lb) / total),
    };
}

// â”€â”€ Full rebuild â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void LightingSystem::rebuild()
{
    m_light_map.clear();

    // Zero all light levels across every loaded chunk.
    m_world.for_each_chunk([&](const Chunk& chunk) {
        glm::ivec3 cp = chunk.chunk_pos();
        for (int lz = 0; lz < CHUNK_SIZE; ++lz)
        for (int ly = 0; ly < CHUNK_SIZE; ++ly)
        for (int lx = 0; lx < CHUNK_SIZE; ++lx) {
            glm::ivec3 wpos = cp * CHUNK_SIZE + glm::ivec3(lx, ly, lz);
            Voxel v = m_world.get_voxel(wpos);
            if (v.light_level != 0) {
                v.light_level = 0;
                m_world.set_voxel(wpos, v);
            }
        }
    });

    propagate_block();

    for (const auto& light : m_dynamic_lights)
        bfs_add(light.pos, light.level, light.color, false);
}

// â”€â”€ Incremental update â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void LightingSystem::update(const std::vector<glm::ivec3>& dirty_voxels)
{
    std::queue<glm::ivec3> relight;

    for (const auto& pos : dirty_voxels) {
        bfs_remove_colored(pos, relight);

        Voxel v = m_world.get_voxel(pos);
        if ((v.flags & VFLAG_LIGHT_SRC) && m_registry) {
            const VoxelTypeDef* def = m_registry->get(v.type_id);
            if (def && def->emit_light > 0) {
                LightColor col{def->emit_r, def->emit_g, def->emit_b};
                bfs_add(pos, def->emit_light, col, false);
            }
        }
    }

    while (!relight.empty()) {
        glm::ivec3 p = relight.front(); relight.pop();
        Voxel v = m_world.get_voxel(p);
        if (v.light_level > 0)
            bfs_add(p, v.light_level, m_light_map.get(p), false);
    }
}

// â”€â”€ Sky light â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void LightingSystem::propagate_sky(glm::ivec2 column)
{
    int max_chunk_y = 0;
    m_world.for_each_chunk([&](const Chunk& chunk) {
        int cy = chunk.chunk_pos().y;
        if (cy > max_chunk_y) max_chunk_y = cy;
    });
    int top_y = (max_chunk_y + 1) * CHUNK_SIZE - 1;

    int x = column.x, z = column.y;
    uint8_t sky = MAX_LIGHT;
    LightColor sky_col{200, 220, 255};  // pale blue daylight

    for (int y = top_y; y >= 0; --y) {
        glm::ivec3 pos{x, y, z};
        Voxel v = m_world.get_voxel(pos);
        if (v.type_id == 0) {
            if (sky > 0 && v.light_level < sky) {
                m_world.set_light_level(pos, sky);
                m_light_map.set(pos, sky_col);
            }
        } else if (v.flags & VFLAG_OPAQUE) {
            sky = 0;
        } else if (sky > 1) {
            --sky;
        }
    }
}

// â”€â”€ Block (emissive voxel) propagation â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void LightingSystem::propagate_block()
{
    if (!m_registry) return;

    m_world.for_each_chunk([&](const Chunk& chunk) {
        glm::ivec3 cp = chunk.chunk_pos();
        for (int lz = 0; lz < CHUNK_SIZE; ++lz)
        for (int ly = 0; ly < CHUNK_SIZE; ++ly)
        for (int lx = 0; lx < CHUNK_SIZE; ++lx) {
            glm::ivec3 wpos = cp * CHUNK_SIZE + glm::ivec3(lx, ly, lz);
            Voxel v = m_world.get_voxel(wpos);
            if (v.type_id == 0 || !(v.flags & VFLAG_LIGHT_SRC)) continue;
            const VoxelTypeDef* def = m_registry->get(v.type_id);
            if (!def || def->emit_light == 0) continue;
            LightColor col{def->emit_r, def->emit_g, def->emit_b};
            bfs_add(wpos, def->emit_light, col, false);
        }
    });
}

// â”€â”€ BFS flood-fill: add colored light outward from a source â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void LightingSystem::bfs_add(glm::ivec3 pos, uint8_t level, LightColor color, bool /*sky*/)
{
    if (level == 0) return;
    struct Entry { glm::ivec3 p; uint8_t l; LightColor c; };
    std::queue<Entry> q;
    q.push({pos, level, color});

    while (!q.empty()) {
        auto [p, l, col] = q.front(); q.pop();
        if (!m_world.get_chunk(World::to_chunk_pos(p))) continue;
        Voxel v = m_world.get_voxel(p);
        if (v.light_level >= l) {
            // Even if brightness won't improve, blend in the color if same level.
            if (v.light_level == l) {
                LightColor existing = m_light_map.get(p);
                m_light_map.set(p, blend_colors(existing, l, col, l));
            }
            continue;
        }
        // Use set_light_level (fast path) — avoids palette scan and only marks
        // m_light_dirty, not m_dirty, so it doesn’t cause unnecessary remeshes.
        m_world.set_light_level(p, l);
        m_light_map.set(p, col);
        if (l <= 1) continue;
        for (auto& d : k_dirs) {
            glm::ivec3 np = p + d;
            Voxel nv = m_world.get_voxel(np);
            if (!(nv.flags & VFLAG_OPAQUE) && nv.light_level < static_cast<uint8_t>(l - 1))
                q.push({np, static_cast<uint8_t>(l - 1), col});
        }
    }
}

// â”€â”€ BFS dark-flood with color removal â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void LightingSystem::bfs_remove_colored(glm::ivec3 pos, std::queue<glm::ivec3>& relight)
{
    std::queue<std::pair<glm::ivec3, uint8_t>> rmQ;
    {
        Voxel v = m_world.get_voxel(pos);
        uint8_t lvl = v.light_level;
        if (lvl == 0) return;
        // Fast path: only clears the light level, no palette scan.
        m_world.set_light_level(pos, 0);
        m_light_map.set(pos, {255, 255, 255});
        rmQ.push({pos, lvl});
    }

    while (!rmQ.empty()) {
        auto [p, l] = rmQ.front(); rmQ.pop();
        for (auto& d : k_dirs) {
            glm::ivec3 np = p + d;
            if (!m_world.get_chunk(World::to_chunk_pos(np))) continue;
            Voxel nv = m_world.get_voxel(np);
            if (nv.light_level == 0) continue;
            if (nv.light_level < l) {
                uint8_t old_lvl = nv.light_level;
                m_world.set_light_level(np, 0);
                m_light_map.set(np, {255, 255, 255});
                rmQ.push({np, old_lvl});
            } else {
                relight.push(np);
            }
        }
    }
}

// â”€â”€ Vertex AO stub â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

float LightingSystem::vertex_ao(glm::ivec3 voxel, FaceDir face, int corner) const
{
    (void)voxel; (void)face; (void)corner;
    return 1.0f;
}

// â”€â”€ Dynamic point lights â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void LightingSystem::add_dynamic_light(glm::ivec3 pos, uint8_t level, EntityID source,
                                        LightColor color)
{
    for (auto& l : m_dynamic_lights) {
        if (l.source == source) {
            l.pos   = pos;
            l.level = level;
            l.color = color;
            rebuild_dynamic();
            return;
        }
    }
    m_dynamic_lights.push_back({pos, level, color, source});
    rebuild_dynamic();
}

void LightingSystem::remove_dynamic_light(EntityID source)
{
    auto it = std::find_if(m_dynamic_lights.begin(), m_dynamic_lights.end(),
                           [source](const PointLight& l){ return l.source == source; });
    if (it != m_dynamic_lights.end()) {
        m_dynamic_lights.erase(it);
        rebuild_dynamic();
    }
}

void LightingSystem::rebuild_dynamic()
{
    m_light_map.clear();

    m_world.for_each_chunk([&](const Chunk& chunk) {
        glm::ivec3 cp = chunk.chunk_pos();
        for (int lz = 0; lz < CHUNK_SIZE; ++lz)
        for (int ly = 0; ly < CHUNK_SIZE; ++ly)
        for (int lx = 0; lx < CHUNK_SIZE; ++lx) {
            glm::ivec3 wpos = cp * CHUNK_SIZE + glm::ivec3(lx, ly, lz);
            Voxel v = m_world.get_voxel(wpos);
            if (v.light_level != 0) {
                v.light_level = 0;
                m_world.set_voxel(wpos, v);
            }
        }
    });

    propagate_block();

    for (const auto& light : m_dynamic_lights)
        bfs_add(light.pos, light.level, light.color, false);
}
