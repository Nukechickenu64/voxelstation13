#pragma once
#include "core/world.h"
#include "core/types.h"
#include <queue>
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

class VoxelRegistry;

// ── LightMap ──────────────────────────────────────────────────────────────────
// Side-channel that stores per-cell RGB light color independently of Voxel.
// Each component is stored in the range 0-255.  The intensity (brightness) is
// kept separately in Voxel::light_level (0-15) so the existing BFS logic still
// controls propagation distances.  The color is simply the tint of that light.
struct LightColor {
    uint8_t r = 255, g = 255, b = 255;  // default: white
};

class LightMap {
public:
    // Set the light color at a cell.  Erases entry if color is default white.
    void   set(glm::ivec3 pos, LightColor c);
    // Get the light color at a cell (returns white {255,255,255} if missing).
    LightColor get(glm::ivec3 pos) const;
    void   clear() { m_data.clear(); }

    // Direct access for snapshot copy (used by chunk mesher worker threads).
    const std::unordered_map<glm::ivec3, LightColor>& data() const { return m_data; }

private:
    // Only non-white cells are stored to keep memory use low.
    std::unordered_map<glm::ivec3, LightColor> m_data;
};

// ── LightingSystem ────────────────────────────────────────────────────────────
// Block, sky, and dynamic light propagation on the voxel grid.
// Also maintains a LightMap for colored light tinting.
class LightingSystem {
public:
    explicit LightingSystem(World& world);

    // Provide the voxel registry so the system can look up emit_light values.
    // Must be called before rebuild() to get correct static emissive lighting.
    void set_registry(const VoxelRegistry* reg) { m_registry = reg; }

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

    // Read-only access to the color map (used by the chunk mesher snapshot).
    const LightMap& light_map() const { return m_light_map; }

    // ── Dynamic point lights (e.g. held flashlight) ──────────────────────
    // color defaults to warm white (255,220,180).
    void add_dynamic_light(glm::ivec3 pos, uint8_t level, EntityID source,
                           LightColor color = {255, 220, 180});
    void remove_dynamic_light(EntityID source);
    void rebuild_dynamic();

private:
    World&    m_world;
    LightMap  m_light_map;

    // BFS add/remove: the color-tinted variants propagate the source color into
    // the light map alongside the brightness BFS in Voxel::light_level.
    void bfs_add(glm::ivec3 pos, uint8_t level, LightColor color, bool sky);
    void bfs_remove_colored(glm::ivec3 pos, std::queue<glm::ivec3>& relight);

    static constexpr uint8_t MAX_LIGHT = 15;

    struct PointLight { glm::ivec3 pos; uint8_t level; LightColor color; EntityID source; };
    std::vector<PointLight> m_dynamic_lights;
    const VoxelRegistry* m_registry = nullptr;
};
