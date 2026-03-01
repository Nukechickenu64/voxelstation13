#pragma once
#include "core/types.h"
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>
#include <functional>
#include <cstring>

// ── Chunk ─────────────────────────────────────────────────────────────────────
constexpr int CHUNK_SIZE = 16;
constexpr int CHUNK_VOL  = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;

class Chunk {
public:
    Chunk(glm::ivec3 chunk_pos);

    Voxel   get(int lx, int ly, int lz) const;
    void    set(int lx, int ly, int lz, Voxel v);
    // Light-only update: fast path that bypasses the palette scan.
    // Marks m_light_dirty but not m_dirty (geometry is unchanged).
    void    set_light(int lx, int ly, int lz, uint8_t lvl);
    uint8_t get_light(int lx, int ly, int lz) const { return m_light_data[idx(lx, ly, lz)]; }

    bool   is_empty() const { return m_non_air_count == 0; }
    // is_dirty() returns true when EITHER geometry OR lighting changed.
    bool   is_dirty()       const { return m_dirty || m_light_dirty; }
    bool   is_geom_dirty()  const { return m_dirty; }
    bool   is_light_dirty() const { return m_light_dirty; }
    void   clear_dirty()    { m_dirty = false; m_light_dirty = false; }
    void   mark_dirty()     { m_dirty = true;  }
    glm::ivec3 chunk_pos() const { return m_chunk_pos; }

private:
    glm::ivec3 m_chunk_pos;
    // Palette-compressed geometry storage (light_level NOT stored here).
    std::vector<Voxel>    m_palette;
    std::vector<uint16_t> m_data;             // CHUNK_VOL palette indices
    uint8_t               m_light_data[CHUNK_VOL]{}; // per-cell light level 0-15
    // O(1) reverse-lookup: key = 8-byte Voxel (light_level zeroed) cast to uint64_t
    std::unordered_map<uint64_t, uint16_t> m_palette_map;
    // Non-air voxel counter — lets is_empty() return correctly after removal
    int m_non_air_count = 0;
    bool m_dirty       = true;
    bool m_light_dirty = false;

    static int idx(int lx, int ly, int lz) {
        return lx + CHUNK_SIZE * (ly + CHUNK_SIZE * lz);
    }
    // Pack palette Voxel (light_level must already be 0) into a 64-bit lookup key.
    static uint64_t palette_key(const Voxel& v) {
        static_assert(sizeof(Voxel) == sizeof(uint64_t),
            "Voxel key packing assumes sizeof(Voxel)==8");
        uint64_t k;
        std::memcpy(&k, &v, sizeof(uint64_t));
        return k;
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
    // Light-only update — bypasses the chunk palette scan.
    // Used by LightingSystem BFS to avoid O(palette) work per propagation step.
    void   set_light_level(glm::ivec3 pos, uint8_t lvl);

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

    // Iterate over every loaded chunk (read-only)
    void for_each_chunk(std::function<void(const Chunk&)> fn) const;

    // Coord helpers
    static glm::ivec3 to_chunk_pos(glm::ivec3 world_pos);
    static glm::ivec3 to_local_pos(glm::ivec3 world_pos);

    // Wipe every voxel and persistent face (used by map loader)
    void clear_all();

private:
    std::unordered_map<glm::ivec3, std::unique_ptr<Chunk>> m_chunks;
    std::unordered_map<VoxelFaceCoord, VoxelFace, VoxelFaceCoordHash> m_sim_faces;
};

// std::hash<glm::ivec3> is provided by <glm/gtx/hash.hpp> (included via types.h)
