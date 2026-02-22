#pragma once
#include "core/world.h"
#include <queue>

// Block, sky, and dynamic light propagation on the voxel grid.
class LightingSystem {
public:
    explicit LightingSystem(World& world);

    // Full rebuild (expensive — call at map load)
    void rebuild();

    // Incremental update after voxel changes at dirty positions
    void update(const std::vector<glm::ivec3>& dirty_voxels);

    // Sky light: set sky light column from top chunk down
    void propagate_sky(glm::ivec2 column);

    // Block light: flood-fill from all emissive voxels
    void propagate_block();

    // Smooth lighting AO value at a vertex (0.0 = full shadow, 1.0 = full bright)
    float vertex_ao(glm::ivec3 voxel, FaceDir face, int corner) const;

private:
    World& m_world;

    void bfs_add(glm::ivec3 pos, uint8_t level, bool sky);
    void bfs_remove(glm::ivec3 pos, bool sky, std::queue<glm::ivec3>& relight);

    static constexpr uint8_t MAX_LIGHT = 15;
};
