#pragma once
#include "core/world.h"
#include <glm/glm.hpp>
#include <unordered_map>

// ── EnclosureDetector ─────────────────────────────────────────────────────────
// Detects whether the air-filled region containing a given world position is
// fully enclosed by solid voxels.
//
// Algorithm: 6-connected BFS from the seed through air voxels (type_id == 0).
//   - If the BFS visits more than k_open_threshold cells without exhausting
//     the region, the space is considered OPEN (connected to infinite void).
//   - If the BFS finishes below the threshold, the space is ENCLOSED.
//
// Results are cached per-cell; call invalidate() after any world change.
class EnclosureDetector {
public:
    explicit EnclosureDetector(const World& world);

    // Returns true if the air region at `pos` is fully enclosed.
    // If `pos` is a solid voxel, returns false.
    // Results are cached until invalidate() is called.
    bool is_enclosed(glm::ivec3 pos);

    // Invalidate the full cache (call after any set_voxel).
    void invalidate();

    // BFS cell budget — regions larger than this are treated as "open space".
    static constexpr int k_open_threshold = 2048;

private:
    struct CacheEntry {
        bool enclosed;
    };

    bool run_bfs(glm::ivec3 start);

    const World& m_world;
    std::unordered_map<glm::ivec3, bool> m_cache;  // air cell → enclosed result
};
