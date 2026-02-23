#pragma once
#include "core/types.h"
#include "core/entity_manager.h"
#include "core/world.h"
#include "inventory/inventory.h"
#include <glm/glm.hpp>
#include <optional>
#include <vector>
#include <string>

// ── WorldItemComponent ────────────────────────────────────────────────────────
// Stored on entities that represent items lying in the world (dropped items).
struct WorldItemComponent {
    ItemStack  item;

    // Resting state: item is lying flat against a voxel face.
    // When false the item hovers / floats at its current position and is rendered
    // as a camera-facing billboard.
    bool       is_resting  = false;
    FaceDir    rest_face   = FaceDir::PosY;   // face the item rests ON
    glm::ivec3 rest_voxel{};                  // voxel owning that face

    // Visual tint (RGBA); set from a deterministic hash of the item id at spawn
    glm::vec4  tint{ 1.f, 0.85f, 0.4f, 0.9f };
};

// ── Label info for UI overlay ─────────────────────────────────────────────────
struct WorldItemLabel {
    glm::vec2   screen_pos;
    std::string name;
    EntityID    entity_id = NULL_ENTITY;
    bool        hovered   = false;
    bool        in_front  = false;  // dot(to_item, cam_fwd) > 0
};

// ── WorldItemSystem ───────────────────────────────────────────────────────────
// Manages creation, querying, and removal of in-world item entities.
class WorldItemSystem {
public:
    WorldItemSystem(World& world, EntityManager& entities);

    // Spawn an item resting on a voxel face.
    // Returns the new entity ID.
    EntityID spawn(glm::ivec3 face_voxel, FaceDir face, ItemStack item);

    // Spawn a floating (non-resting) item at a world position.
    // An initial velocity can be supplied (e.g. for thrown items).
    EntityID spawn_floating(glm::vec3 pos, ItemStack item,
                            glm::vec3 velocity = {});

    // Advance floating item physics: settle items that come to rest.
    // Call once per simulation tick after PhysicsSystem::tick().
    void tick(double dt);

    // Remove and return the item from the entity; entity is destroyed.
    // Returns std::nullopt if entity has no WorldItemComponent.
    std::optional<ItemStack> pick_up(EntityID id);

    // Find closest world item whose bounds the ray enters, within max_dist.
    // out_dist is set to the intersection distance (0 if no hit).
    // Compares against voxel_hit_dist:  if the item is behind a wall ignore it.
    EntityID ray_cast_items(glm::vec3 origin, glm::vec3 dir,
                            float max_dist, float voxel_hit_dist,
                            float& out_dist) const;

    // Like ray_cast_items but returns ALL candidates sorted nearest-first.
    std::vector<EntityID> ray_cast_items_all(glm::vec3 origin, glm::vec3 dir,
                                              float max_dist,
                                              float voxel_hit_dist) const;

    // Alt-mode: return the entity whose projected screen position is closest to
    // cursor and within threshold_px screen pixels.
    // mvp should be the view-projection matrix.
    EntityID screen_hover(glm::vec2 cursor,
                          const glm::mat4& mvp,
                          int fb_w, int fb_h,
                          float threshold_px = 40.f) const;

    // Like screen_hover but returns ALL candidates sorted nearest-to-cursor first.
    std::vector<EntityID> screen_hover_all(glm::vec2 cursor,
                                            const glm::mat4& mvp,
                                            int fb_w, int fb_h,
                                            float threshold_px = 40.f) const;

    // Project all item positions to screen space for UI label rendering.
    // Call AFTER draw_world() so the caller has a valid VP matrix.
    std::vector<WorldItemLabel> build_labels(const glm::mat4& vp,
                                             int fb_w, int fb_h,
                                             EntityID hovered) const;

    // Compute the world-space centre of an item's rest position.
    static glm::vec3 item_world_pos(const WorldItemComponent& wic);

private:
    // Returns a deterministic tint colour for a given item id string.
    static glm::vec4 tint_for_item(const std::string& item_id);

    World&         m_world;
    EntityManager& m_entities;
};
