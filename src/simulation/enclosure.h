#pragma once
#include "core/world.h"
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

// ── EnclosureDetector ─────────────────────────────────────────────────────────
// Detects whether the region containing a given world position is enclosed.
//
// Traversable cells: air (type_id == 0) and open doors (VERT_PLANE_Z, !SOLID).
// Barriers: walls and closed doors (VERT_PLANE_Z + SOLID).
//
// Algorithm: 6-connected BFS through traversable cells.
//   - Region larger than k_open_threshold → OPEN (infinite void).
//   - Region finishes below threshold    → ENCLOSED.
//
// Closed-door cells (VERT_PLANE_Z + SOLID) use a special rule:
//   The door belongs to the smaller adjacent enclosed region by default.
//   Exception: if that smaller region connects to unenclosed space through
//   another closed door, the door belongs to the larger enclosed region instead.
//
// Results are cached per-cell; call invalidate() after any world change.
class EnclosureDetector {
public:
    explicit EnclosureDetector(const World& world);

    // Returns true if `pos` is part of an enclosed region.
    //   Air / open-door cell  → enclosed if its BFS region is bounded.
    //   Closed-door cell      → enclosed per the smaller-region rule above.
    //   Other solid           → false.
    // Results are cached until invalidate() is called.
    bool is_enclosed(glm::ivec3 pos);

    // Invalidate all caches (call after any set_voxel).
    void invalidate();

    // BFS cell budget — regions larger than this are treated as "open space".
    static constexpr int k_open_threshold = 2048;

private:
    // Metadata stored per BFS-discovered region.
    struct RegionInfo {
        bool                    enclosed;      // true = bounded region
        int                     size;          // number of traversable cells
        std::vector<glm::ivec3> border_doors;  // closed-door voxels touching this region
    };

    // True if the voxel at pos is traversable for BFS (air or open door).
    bool is_traversable(glm::ivec3 pos) const;

    // True if the voxel at pos is a closed door (VERT_PLANE_Z + SOLID).
    bool is_closed_door(glm::ivec3 pos) const;

    // Ensure the region for a traversable cell is computed; returns region id
    // (>= 0 = enclosed, -1 = open/unenclosed).
    int  find_region(glm::ivec3 start);

    // Evaluate is_enclosed for a closed-door cell using the smaller-region rule.
    bool eval_door(glm::ivec3 pos);

    const World& m_world;

    // traversable cell → region id  (-1 = open)
    std::unordered_map<glm::ivec3, int>  m_cell_region;
    // region id → RegionInfo
    std::vector<RegionInfo>              m_regions;
    // closed-door cell → cached enclosed result
    std::unordered_map<glm::ivec3, bool> m_door_cache;
};
