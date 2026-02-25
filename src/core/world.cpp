#include "core/world.h"
#include <cstring>

// ── Chunk ─────────────────────────────────────────────────────────────────────

Chunk::Chunk(glm::ivec3 chunk_pos)
    : m_chunk_pos(chunk_pos)
{
    // Single-entry palette: 0 = air
    m_palette.push_back(Voxel{});
    m_data.assign(CHUNK_VOL, 0);
}

Voxel Chunk::get(int lx, int ly, int lz) const
{
    return m_palette[m_data[idx(lx, ly, lz)]];
}

void Chunk::set(int lx, int ly, int lz, Voxel v)
{
    // Find or insert palette entry
    uint16_t pal_idx = 0;
    for (uint16_t i = 0; i < static_cast<uint16_t>(m_palette.size()); ++i) {
        if (std::memcmp(&m_palette[i], &v, sizeof(Voxel)) == 0) {
            pal_idx = i;
            goto found;
        }
    }
    // Not found — add
    pal_idx = static_cast<uint16_t>(m_palette.size());
    m_palette.push_back(v);
found:
    m_data[idx(lx, ly, lz)] = pal_idx;
    m_dirty = true;
    if (v.type_id != 0) m_all_air = false;
}

// ── World ─────────────────────────────────────────────────────────────────────

World::World()  = default;
World::~World() = default;

void World::for_each_chunk(std::function<void(const Chunk&)> fn) const
{
    for (const auto& [pos, chunk] : m_chunks)
        fn(*chunk);
}

glm::ivec3 World::to_chunk_pos(glm::ivec3 p)
{
    // Floor division for negative coords
    auto floordiv = [](int a, int b) { return a / b - (a % b != 0 && (a ^ b) < 0); };
    return { floordiv(p.x, CHUNK_SIZE),
             floordiv(p.y, CHUNK_SIZE),
             floordiv(p.z, CHUNK_SIZE) };
}

glm::ivec3 World::to_local_pos(glm::ivec3 p)
{
    auto mod = [](int a, int b) { return ((a % b) + b) % b; };
    return { mod(p.x, CHUNK_SIZE),
             mod(p.y, CHUNK_SIZE),
             mod(p.z, CHUNK_SIZE) };
}

Chunk* World::get_chunk(glm::ivec3 cp)
{
    auto it = m_chunks.find(cp);
    return it != m_chunks.end() ? it->second.get() : nullptr;
}

Chunk* World::get_or_create_chunk(glm::ivec3 cp)
{
    auto& ptr = m_chunks[cp];
    if (!ptr) ptr = std::make_unique<Chunk>(cp);
    return ptr.get();
}

void World::unload_chunk(glm::ivec3 cp)
{
    m_chunks.erase(cp);
}

Voxel World::get_voxel(glm::ivec3 pos) const
{
    glm::ivec3 cp = to_chunk_pos(pos);
    auto it = m_chunks.find(cp);
    if (it == m_chunks.end()) return Voxel{};
    glm::ivec3 lp = to_local_pos(pos);
    return it->second->get(lp.x, lp.y, lp.z);
}

void World::set_voxel(glm::ivec3 pos, Voxel v)
{
    glm::ivec3 cp = to_chunk_pos(pos);
    glm::ivec3 lp = to_local_pos(pos);
    get_or_create_chunk(cp)->set(lp.x, lp.y, lp.z, v);
}

VoxelFace World::get_face(glm::ivec3 pos, FaceDir dir) const
{
    VoxelFace face;
    face.coord = {pos, dir};
    // Derive tags from voxel type flags
    Voxel v = get_voxel(pos);
    if (v.flags & VFLAG_SOLID)   face.tags |= TAG_DENSE | TAG_OPAQUE;
    if (v.flags & VFLAG_OPAQUE)  face.tags |= TAG_OPAQUE;
    face.is_visible = true; // mesher will cull; this is just data
    // Check sparse sim face for extra state
    VoxelFaceCoord coord{pos, dir};
    auto it = m_sim_faces.find(coord);
    if (it != m_sim_faces.end()) {
        face.atmos_id   = it->second.atmos_id;
        face.pipe_flags = it->second.pipe_flags;
        face.wire_flags = it->second.wire_flags;
        face.decal_id   = it->second.decal_id;
        face.layers     = it->second.layers;
        face.tags      |= it->second.tags;
    }
    return face;
}

VoxelFace* World::get_sim_face(VoxelFaceCoord coord)
{
    auto it = m_sim_faces.find(coord);
    return it != m_sim_faces.end() ? &it->second : nullptr;
}

VoxelFace& World::get_or_create_sim_face(VoxelFaceCoord coord)
{
    return m_sim_faces[coord];
}

void World::remove_sim_face(VoxelFaceCoord coord)
{
    m_sim_faces.erase(coord);
}

std::vector<Chunk*> World::dirty_chunks()
{
    std::vector<Chunk*> result;
    for (auto& [pos, chunk] : m_chunks)
        if (chunk->is_dirty()) result.push_back(chunk.get());
    return result;
}

// Amanatides & Woo DDA ray cast
RayHit World::raycast(glm::vec3 origin, glm::vec3 dir, float max_dist) const
{
    RayHit hit;
    glm::vec3 d = glm::normalize(dir);
    glm::ivec3 step, pos;
    glm::vec3  tmax, tdelta;

    for (int i = 0; i < 3; ++i) {
        pos[i]    = static_cast<int>(std::floor(origin[i]));
        step[i]   = (d[i] > 0) ? 1 : -1;
        if (d[i] != 0) {
            tdelta[i] = std::abs(1.f / d[i]);
            float b   = (d[i] > 0) ? (pos[i] + 1 - origin[i]) : (origin[i] - pos[i]);
            tmax[i]   = b * std::abs(1.f / d[i]);
        } else {
            tdelta[i] = 1e30f;
            tmax[i]   = 1e30f;
        }
    }

    int axis = 0;
    float dist = 0.f;
    while (dist < max_dist) {
        Voxel v = get_voxel(pos);
        if (v.type_id != 0) {
            hit.valid   = true;
            hit.voxel   = pos;
            hit.distance= dist;
            hit.hit_pos = origin + d * dist;
            // Face is opposite of step direction on struck axis
            if      (axis == 0) hit.face = (step.x > 0) ? FaceDir::NegX : FaceDir::PosX;
            else if (axis == 1) hit.face = (step.y > 0) ? FaceDir::NegY : FaceDir::PosY;
            else                hit.face = (step.z > 0) ? FaceDir::NegZ : FaceDir::PosZ;
            return hit;
        }
        // Advance to next voxel boundary
        if (tmax.x < tmax.y && tmax.x < tmax.z) {
            axis = 0; dist = tmax.x; pos.x += step.x; tmax.x += tdelta.x;
        } else if (tmax.y < tmax.z) {
            axis = 1; dist = tmax.y; pos.y += step.y; tmax.y += tdelta.y;
        } else {
            axis = 2; dist = tmax.z; pos.z += step.z; tmax.z += tdelta.z;
        }
    }
    return hit;
}
