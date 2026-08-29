#pragma once
#include "core/types.h"
#include "core/entity_manager.h"
#include "core/world.h"
#include "inventory/inventory.h"
#include "inventory/item_registry.h"
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

    // Scatter offset within the face plane (in face-tangent space, metres).
    // Applied by item_world_pos() so items don't all pile at the turf centre.
    // Range is typically ±0.35 so the item stays inside the 1×1 face boundary.
    glm::vec2  face_offset{};

    // Visual tint (RGBA); set from a deterministic hash of the item id at spawn
    glm::vec4  tint{ 1.f, 0.85f, 0.4f, 0.9f };
};

// ── Label info for UI overlay ─────────────────────────────────────────────────
struct WorldItemLabel {
    glm::vec2   screen_pos;
    std::string name;
    std::string item_id;    // item def id — used for icon lookup
    EntityID    entity_id = NULL_ENTITY;
    bool        hovered   = false;
    bool        in_front  = false;  // dot(to_item, cam_fwd) > 0
};

// ── WorldItemSystem ───────────────────────────────────────────────────────────
// Manages creation, querying, and removal of in-world item entities.
class WorldItemSystem {
public:
    WorldItemSystem(World& world, EntityManager& entities, const ItemRegistry* item_reg = nullptr);

    // Spawn an item resting on a voxel face.
    // Returns the new entity ID.
    // face_offset is in the face's tangent plane (metres, ±0.5 max = inside face).
    EntityID spawn(glm::ivec3 face_voxel, FaceDir face, ItemStack item,
                   glm::vec2 face_offset = {});

    // Spawn with a random scatter offset (uniform within ±scatter_radius on
    // each face-tangent axis, clamped to ±0.38 so the item stays on the face).
    // Used by item-drop and container-spill code.
    EntityID spawn_scattered(glm::ivec3 face_voxel, FaceDir face, ItemStack item,
                             float scatter_radius = 0.35f);

    // Spawn a floating (non-resting) item at a world position.
    // An initial velocity can be supplied (e.g. for thrown items).
    EntityID spawn_floating(glm::vec3 pos, ItemStack item,
                            glm::vec3 velocity = {});

    // Advance floating item physics: settle items that come to rest.
    // Call once per simulation tick after PhysicsSystem::tick().
    void tick(double dt);

    // Client-side standalone tick: applies gravity, moves floating items, then
    // settles them onto the floor.  Use this instead of tick() when no
    // server-side PhysicsSystem is available (i.e. in the main client loop).
    void tick_standalone(double dt);

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

    // Spawn by item id at world position (uses optional ItemRegistry if set).
    EntityID spawn_by_id(const std::string& id, glm::vec3 pos);

private:
    // Returns a deterministic tint colour for a given item id string.
    static glm::vec4 tint_for_item(const std::string& item_id);

    World&         m_world;
    EntityManager& m_entities;
    const ItemRegistry* m_item_registry = nullptr;
};
