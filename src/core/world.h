#pragma once
#include "core/types.h"
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>

// ── Chunk ─────────────────────────────────────────────────────────────────────
constexpr int CHUNK_SIZE = 16;
constexpr int CHUNK_VOL  = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;

class Chunk {
public:
    Chunk(glm::ivec3 chunk_pos);

    Voxel  get(int lx, int ly, int lz) const;
    void   set(int lx, int ly, int lz, Voxel v);
    bool   is_empty() const { return m_all_air; }
    bool   is_dirty() const { return m_dirty; }
    void   clear_dirty()    { m_dirty = false; }
    glm::ivec3 chunk_pos() const { return m_chunk_pos; }

private:
    glm::ivec3 m_chunk_pos;
    // Palette-compressed storage: palette[palette_index] = voxel
    std::vector<Voxel>    m_palette;
    std::vector<uint16_t> m_data;   // CHUNK_VOL entries into palette
    bool m_dirty   = true;
    bool m_all_air = true;

    static int idx(int lx, int ly, int lz) {
        return lx + CHUNK_SIZE * (ly + CHUNK_SIZE * lz);
    }
};

// ── World ─────────────────────────────────────────────────────────────────────
class World {
public:
    World();
    ~World();

    // Voxel access (world coordinates)
    Voxel  get_voxel(glm::ivec3 pos) const;
    void   set_voxel(glm::ivec3 pos, Voxel v);

    // Face access — derived on call; only simulation faces are persistent
    VoxelFace  get_face(glm::ivec3 pos, FaceDir dir) const;
    VoxelFace* get_sim_face(VoxelFaceCoord coord);        // null if not persistent
    VoxelFace& get_or_create_sim_face(VoxelFaceCoord coord);
    void       remove_sim_face(VoxelFaceCoord coord);

    // Ray cast (DDA, Amanatides & Woo)
    RayHit raycast(glm::vec3 origin, glm::vec3 dir, float max_dist = 10.f) const;

    // Chunk management
    Chunk*       get_chunk(glm::ivec3 chunk_pos);
    Chunk*       get_or_create_chunk(glm::ivec3 chunk_pos);
    void         unload_chunk(glm::ivec3 chunk_pos);
    std::vector<Chunk*> dirty_chunks();

    // Coord helpers
    static glm::ivec3 to_chunk_pos(glm::ivec3 world_pos);
    static glm::ivec3 to_local_pos(glm::ivec3 world_pos);

private:
    std::unordered_map<glm::ivec3, std::unique_ptr<Chunk>> m_chunks;
    std::unordered_map<VoxelFaceCoord, VoxelFace, VoxelFaceCoordHash> m_sim_faces;
};

// std::hash<glm::ivec3> is provided by <glm/gtx/hash.hpp> (included via types.h)
