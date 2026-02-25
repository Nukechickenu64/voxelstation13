#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <unordered_map>

// Describes one statically-placed 3-D model object in the world.
// Cell coordinates are voxel-grid integers; the model's world origin is
// placed at the horizontal centre of the cell (cell + vec3(0.5, 0, 0.5)).
struct StaticModelObject {
    std::string  name;           // key into renderer's loaded model map
    glm::ivec3   cell{};         // base voxel cell (model origin rests on top of this cell's floor)
    float        yaw   = 0.f;   // Y-axis rotation in degrees
    float        scale = 1.f;   // uniform scale

    // Collision / simulation flags
    bool blocks_mobs = true;    // solid to character controllers / mob pathfinding
    bool blocks_gas  = false;   // opaque to atmos gas flow

    // --- Derived fields (populated by ModelObjectManager::add) ---
    // World position of the model's local origin (= cell + {0.5,0,0.5})
    glm::vec3  world_pos{};
    // Inclusive voxel-cell AABB that the model occupies (scale applied, snapped to grid)
    glm::ivec3 voxel_min{};
    glm::ivec3 voxel_max{};
};

// Manages placed static model objects and their collision presence.
//
// Typical lifecycle:
//   1. After renderer.load_model(name, ...):
//        glm::vec3 mn, mx;
//        renderer.model_local_aabb(name, mn, mx);
//        model_objects.register_extents(name, mn, mx);
//   2. model_objects.add({...}); for each placed object
//   3. Pass &model_objects to server.set_model_objects() so physics and
//      atmos pick up the collision data.
//   4. Each render frame, iterate objects() and call renderer.queue_model().
class ModelObjectManager {
public:
    // Register the local-space AABB for a model name.
    // Must be called before add() for that model name.
    void register_extents(const std::string& name,
                          glm::vec3 local_min, glm::vec3 local_max);

    // Place a static object in the world.  Computes world_pos, voxel_min,
    // and voxel_max from the registered extents + cell + scale.
    // Returns an opaque integer ID (>= 0) that can be passed to remove().
    int  add(StaticModelObject def);

    // Remove a previously added object by ID.
    void remove(int id);

    // All currently placed objects (read-only).
    const std::vector<StaticModelObject>& objects() const { return m_objects; }

    // True when a model with blocks_mobs=true occupies voxel cell 'c'.
    bool blocks_mob_at(glm::ivec3 c) const;
    // True when a model with blocks_gas=true occupies voxel cell 'c'.
    bool blocks_gas_at(glm::ivec3 c) const;

private:
    struct Extents {
        glm::vec3 local_min;
        glm::vec3 local_max;
    };
    std::unordered_map<std::string, Extents> m_extents;
    std::vector<StaticModelObject>           m_objects;
    std::vector<int>                         m_ids;
    int                                      m_next_id = 0;
};
