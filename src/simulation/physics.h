#pragma once
#include "core/world.h"
#include "core/entity_manager.h"

// Components used by physics
struct TransformComponent {
    glm::vec3 pos{};
    glm::vec3 prev_pos{};   // for interpolation
    float     yaw  = 0.f;
    float     pitch= 0.f;
};

struct VelocityComponent {
    glm::vec3 linear{};
};

struct CharacterControllerComponent {
    float move_speed  = 4.5f;
    float sprint_mult = 1.8f;
    float jump_vel    = 6.f;
    float height      = 0.9f;   // < 1.0 so player fits in 1-tile-tall passages
    float radius      = 0.3f;   // 0.6 total width — fits in a 1-voxel gap
    bool  on_ground   = false;
    bool  crouching   = false;
    bool  sprinting   = false;
    bool  zero_g      = false;  // true when player is in vacuum / space
    bool  noclip      = false;  // ghost mode: no collision, no gravity

    // Set each frame by the game loop; reflects whether a jetpack is equipped
    // in the back slot.  In zero-G the player can only self-propel with a jetpack.
    bool      jetpack_equipped = false;
    // Normalised thrust direction supplied by prepare_character_movement;
    // consumed (with dt) inside tick().  Reset to zero each tick.
    glm::vec3 jetpack_input{};

    // Desired horizontal velocity written by prepare_character_movement each frame.
    // tick() accelerates the actual velocity toward this using GROUND/AIR_ACCEL.
    glm::vec3 wish_move{};
};

// Kinematic character physics and projectile movement.
class PhysicsSystem {
public:
    PhysicsSystem(World& world, EntityManager& entities);

    void tick(double dt);

    // Set up character velocity from a wish direction without ticking.
    // Call this before tick() — used by Server::tick() after receiving input.
    void prepare_character_movement(EntityID id, glm::vec3 wish_dir,
                                    bool crouch, bool sprint);

    // Move the character controller, resolving voxel collisions.
    // Equivalent to prepare_character_movement() + tick().
    void move_character(EntityID id, glm::vec3 wish_dir, bool crouch, bool sprint, double dt);

    // Apply standing wind forces from atmos decompression
    void apply_wind(EntityID id, glm::vec3 force);

    // Simple AABB vs voxel grid check
    bool overlaps_solid(glm::vec3 min, glm::vec3 max) const;

private:
    glm::vec3 resolve_collisions(glm::vec3 pos, glm::vec3 delta, float radius, float height) const;

    World&         m_world;
    EntityManager& m_entities;
    static constexpr float GRAVITY          = -9.8f;
    // Jetpack: acceleration (m/s²) and terminal speed (m/s) in zero-G
    static constexpr float JETPACK_ACCEL    = 6.f;
    static constexpr float JETPACK_MAX_SPEED= 8.f;
};
