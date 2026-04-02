#include "render/lighting.h"
#include "data/voxel_registry.h"
#include <functional>
#include <algorithm>

// Six cardinal directions used by BFS.
static const glm::ivec3 k_dirs[6] = {
    {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
};

// LightMap

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

// LightingSystem

LightingSystem::LightingSystem(World& world)
    : m_world(world)
{}

// Helpers

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

// Full rebuild

void LightingSystem::rebuild()
{
    m_light_map.clear();

    // Zero all light levels across every loaded chunk.
    // Use set_light_level (fast path) — avoids palette mutation entirely.
    m_world.for_each_chunk([&](const Chunk& chunk) {
        glm::ivec3 cp = chunk.chunk_pos();
        for (int lz = 0; lz < CHUNK_SIZE; ++lz)
        for (int ly = 0; ly < CHUNK_SIZE; ++ly)
        for (int lx = 0; lx < CHUNK_SIZE; ++lx) {
            glm::ivec3 wpos = cp * CHUNK_SIZE + glm::ivec3(lx, ly, lz);
            if (chunk.get_light(lx, ly, lz) != 0)
                m_world.set_light_level(wpos, 0);
        }
    });

    // Propagate sky light from the top of each column down through transparent voxels.
    // Collect all unique (x,z) columns from loaded chunks to avoid redundant work.
    std::unordered_set<glm::ivec2> columns;
    m_world.for_each_chunk([&](const Chunk& chunk) {
        glm::ivec3 cp = chunk.chunk_pos();
        for (int lz = 0; lz < CHUNK_SIZE; ++lz)
        for (int lx = 0; lx < CHUNK_SIZE; ++lx) {
            columns.insert({cp.x * CHUNK_SIZE + lx, cp.z * CHUNK_SIZE + lz});
        }
    });
    for (const auto& col : columns)
        propagate_sky(col);

    propagate_block();

    for (const auto& light : m_dynamic_lights)
        bfs_add(light.pos, light.level, light.color, false);

    if (m_auto_border_sky)
        flood_border_sky(m_auto_border_sky_level);
}

// Incremental update

void LightingSystem::update(const std::vector<glm::ivec3>& dirty_voxels)
{
    // Use a vector+set pair so we can iterate while deduplicating relight seeds.
    std::vector<glm::ivec3> relight_vec;
    std::unordered_set<glm::ivec3> relight_set;
    relight_vec.reserve(64);

    // Lambda pushed into bfs_remove_colored as a deduplicating inserter.
    auto enqueue_relight = [&](glm::ivec3 p) {
        if (relight_set.insert(p).second)
            relight_vec.push_back(p);
    };

    for (const auto& pos : dirty_voxels) {
        bfs_remove_colored(pos, enqueue_relight);

        Voxel v = m_world.get_voxel(pos);
        if ((v.flags & VFLAG_LIGHT_SRC) && m_registry) {
            const VoxelTypeDef* def = m_registry->get(v.type_id);
            if (def && def->emit_light > 0) {
                LightColor col{def->emit_r, def->emit_g, def->emit_b};
                bfs_add(pos, def->emit_light, col, false);
            }
        }
    }

    for (const auto& p : relight_vec) {
        Voxel v = m_world.get_voxel(p);
        if (v.light_level > 0)
            bfs_add(p, v.light_level, m_light_map.get(p), false);
    }
}

// Sky light

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

// Block (emissive voxel) propagation

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

// BFS flood-fill: add colored light outward from a source

void LightingSystem::bfs_add(glm::ivec3 pos, uint8_t level, LightColor color, bool /*sky*/)
{
    if (level == 0) return;
    struct Entry { glm::ivec3 p; uint8_t l; LightColor c; };

    // Vector-based flat BFS queue — avoids std::deque segment allocations.
    std::vector<Entry> q;
    q.reserve(4096);
    q.push_back({pos, level, color});
    size_t head = 0;

    while (head < q.size()) {
        auto [p, l, col] = q[head++];
        if (!m_world.get_chunk(World::to_chunk_pos(p))) continue;
        Voxel v = m_world.get_voxel(p);
        if (v.light_level >= l) {
            if (v.light_level == l) {
                LightColor existing = m_light_map.get(p);
                m_light_map.set(p, blend_colors(existing, l, col, l));
            }
            continue;
        }
        // Use set_light_level (fast path) — avoids palette scan.
        m_world.set_light_level(p, l);
        m_light_map.set(p, col);
        if (l <= 1) continue;
        const uint8_t nl = static_cast<uint8_t>(l - 1);
        for (const auto& d : k_dirs) {
            glm::ivec3 np = p + d;
            if (!m_world.get_chunk(World::to_chunk_pos(np))) continue;
            Voxel nv = m_world.get_voxel(np);
            if (nv.flags & VFLAG_OPAQUE) {
                // Stamp surface brightness onto solid voxels so that the
                // mesher can read it for exterior-face lighting without
                // needing to walk inward through the hull.
                if (nv.light_level < nl) {
                    m_world.set_light_level(np, nl);
                    m_light_map.set(np, col);
                }
            } else if (nv.light_level < nl) {
                q.push_back({np, nl, col});
            }
        }
    }
}
// BFS dark-flood with color removal

void LightingSystem::bfs_remove_colored(glm::ivec3 pos,
                                         const std::function<void(glm::ivec3)>& enqueue_relight)
{
    struct RmEntry { glm::ivec3 p; uint8_t l; };
    // Vector-based removal queue — avoids std::deque heap churn.
    std::vector<RmEntry> rmQ;
    rmQ.reserve(1024);
    {
        Voxel v = m_world.get_voxel(pos);
        uint8_t lvl = v.light_level;
        if (lvl == 0) return;
        // Fast path: only clears the light level, no palette scan.
        m_world.set_light_level(pos, 0);
        m_light_map.set(pos, {255, 255, 255});
        rmQ.push_back({pos, lvl});
    }

    size_t head = 0;
    while (head < rmQ.size()) {
        auto [p, l] = rmQ[head++];
        for (const auto& d : k_dirs) {
            glm::ivec3 np = p + d;
            if (!m_world.get_chunk(World::to_chunk_pos(np))) continue;
            Voxel nv = m_world.get_voxel(np);
            if (nv.light_level == 0) continue;
            if (nv.light_level < l) {
                uint8_t old_lvl = nv.light_level;
                m_world.set_light_level(np, 0);
                m_light_map.set(np, {255, 255, 255});
                rmQ.push_back({np, old_lvl});
            } else {
                enqueue_relight(np);
            }
        }
    }
}

// Vertex AO - computes ambient occlusion for a vertex corner of a voxel face.
// Samples the three neighbouring cells around the vertex and counts how many
// are opaque.  Returns 1.0 (fully lit) when none are opaque, down to 0.25
// when all three block ambient light.

// Direction offsets for each face's outward normal.
static const glm::ivec3 k_face_normal[6] = {
    { 1, 0, 0},  // PosX
    {-1, 0, 0},  // NegX
    { 0, 1, 0},  // PosY
    { 0,-1, 0},  // NegY
    { 0, 0, 1},  // PosZ
    { 0, 0,-1},  // NegZ
};
// Tangent axes for each face (which two axes span the face plane).
static const int k_face_t1[6] = {1, 1, 0, 0, 0, 0};  // Y, Y, X, X, X, X
static const int k_face_t2[6] = {2, 2, 2, 2, 1, 1};  // Z, Z, Z, Z, Y, Y

float LightingSystem::vertex_ao(glm::ivec3 voxel, FaceDir face, int corner) const
{
    int fi = static_cast<int>(face);
    if (fi < 0 || fi >= 6) return 1.0f;

    // Corner encodes which tangent directions to step (+1 or -1 along each tangent).
    // corner 0: (0,0) -> step -t1, -t2
    // corner 1: (1,0) -> step +t1, -t2
    // corner 2: (1,1) -> step +t1, +t2
    // corner 3: (0,1) -> step -t1, +t2
    int s1 = (corner == 1 || corner == 2) ? 1 : -1;
    int s2 = (corner == 2 || corner == 3) ? 1 : -1;

    glm::ivec3 n = k_face_normal[fi];
    int t1 = k_face_t1[fi];
    int t2 = k_face_t2[fi];

    // Three sample positions: one step along each tangent, and the diagonal.
    glm::ivec3 base = voxel + n;  // one cell outward from the voxel
    glm::ivec3 samples[3];
    {
        glm::ivec3 off1(0), off2(0), off12(0);
        off1[t1] = s1;
        off2[t2] = s2;
        off12 = off1 + off2;
        samples[0] = base + off1;
        samples[1] = base + off2;
        samples[2] = base + off12;
    }

    int blocked = 0;
    for (const auto& sp : samples) {
        Voxel sv = m_world.get_voxel(sp);
        if (sv.type_id != 0 && (sv.flags & VFLAG_OPAQUE))
            ++blocked;
    }

    // Map blocked count (0-3) to AO factor: 1.0, 0.75, 0.5, 0.25
    return 1.0f - blocked * 0.25f;
}

void LightingSystem::flood_border_sky(uint8_t level)
{
    if (level == 0) return;
    std::vector<glm::ivec3> seeds;
    m_world.for_each_chunk([&](const Chunk& chunk) {
        glm::ivec3 cp = chunk.chunk_pos();
        for (int lz = 0; lz < CHUNK_SIZE; ++lz)
        for (int ly = 0; ly < CHUNK_SIZE; ++ly)
        for (int lx = 0; lx < CHUNK_SIZE; ++lx) {
            glm::ivec3 wp = cp * CHUNK_SIZE + glm::ivec3(lx, ly, lz);
            for (const auto& d : k_dirs) {
                if (!m_world.get_chunk(World::to_chunk_pos(wp + d))) {
                    seeds.push_back(wp);
                    break;
                }
            }
        }
    });
    for (const auto& s : seeds)
        bfs_add(s, level, LightColor{255, 255, 255}, false);
}


// Dynamic point lights

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

    // Zero light levels using the fast set_light_level path.
    m_world.for_each_chunk([&](const Chunk& chunk) {
        glm::ivec3 cp = chunk.chunk_pos();
        for (int lz = 0; lz < CHUNK_SIZE; ++lz)
        for (int ly = 0; ly < CHUNK_SIZE; ++ly)
        for (int lx = 0; lx < CHUNK_SIZE; ++lx) {
            glm::ivec3 wpos = cp * CHUNK_SIZE + glm::ivec3(lx, ly, lz);
            if (chunk.get_light(lx, ly, lz) != 0)
                m_world.set_light_level(wpos, 0);
        }
    });

    propagate_block();

    for (const auto& light : m_dynamic_lights)
        bfs_add(light.pos, light.level, light.color, false);

    if (m_auto_border_sky)
        flood_border_sky(m_auto_border_sky_level);
}