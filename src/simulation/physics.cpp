#include "simulation/physics.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

PhysicsSystem::PhysicsSystem(World& world, EntityManager& entities)
    : m_world(world)
    , m_entities(entities)
{}

void PhysicsSystem::tick(double dt)
{
    m_entities.each<TransformComponent>([&](EntityID id, TransformComponent& tr) {
        auto* vel = m_entities.get_component<VelocityComponent>(id);
        if (!vel) return;
        auto* cc = m_entities.get_component<CharacterControllerComponent>(id);

        // Save previous position for interpolation
        tr.prev_pos = tr.pos;

        // Apply gravity to non-grounded entities
        if (!cc || !cc->on_ground)
            vel->linear.y += GRAVITY * static_cast<float>(dt);

        glm::vec3 delta = vel->linear * static_cast<float>(dt);

        if (cc) {
            float h = cc->crouching ? cc->height * 0.5f : cc->height;
            glm::vec3 new_pos = resolve_collisions(tr.pos, delta, cc->radius, h);
            glm::vec3 actual  = new_pos - tr.pos;
            tr.pos = new_pos;

            // Detect ground contact
            bool was_grounded = cc->on_ground;
            cc->on_ground = overlaps_solid(
                tr.pos + glm::vec3(-cc->radius, -0.1f, -cc->radius),
                tr.pos + glm::vec3( cc->radius,  0.0f,  cc->radius));

            // Zero vertical velocity on landing
            if (cc->on_ground && !was_grounded && vel->linear.y < 0)
                vel->linear.y = 0;

            // Dampen horizontal velocity (friction)
            vel->linear.x *= 0.85f;
            vel->linear.z *= 0.85f;
        } else {
            tr.pos += delta;
        }
    });
}

void PhysicsSystem::move_character(EntityID id, glm::vec3 wish_dir,
                                   bool jump, bool crouch, bool sprint,
                                   double dt)
{
    auto* tr  = m_entities.get_component<TransformComponent>(id);
    auto* vel = m_entities.get_component<VelocityComponent>(id);
    auto* cc  = m_entities.get_component<CharacterControllerComponent>(id);
    if (!tr || !vel || !cc) return;

    cc->crouching = crouch;
    cc->sprinting = sprint && !crouch;

    float speed = cc->move_speed * (cc->sprinting ? cc->sprint_mult : 1.f);

    if (glm::length(wish_dir) > 0.f)
        wish_dir = glm::normalize(wish_dir);

    // Project movement onto the ground plane
    vel->linear.x = wish_dir.x * speed;
    vel->linear.z = wish_dir.z * speed;

    if (jump && cc->on_ground) {
        vel->linear.y = cc->jump_vel;
        cc->on_ground = false;
    }

    tick(dt);
}

void PhysicsSystem::apply_wind(EntityID id, glm::vec3 force)
{
    auto* vel = m_entities.get_component<VelocityComponent>(id);
    if (vel) vel->linear += force;
}

bool PhysicsSystem::overlaps_solid(glm::vec3 min, glm::vec3 max) const
{
    using std::floor; using std::ceil;
    glm::ivec3 imin{ (int)floor(min.x), (int)floor(min.y), (int)floor(min.z) };
    glm::ivec3 imax{ (int)ceil(max.x),  (int)ceil(max.y),  (int)ceil(max.z)  };
    for (int z = imin.z; z <= imax.z; ++z)
    for (int y = imin.y; y <= imax.y; ++y)
    for (int x = imin.x; x <= imax.x; ++x) {
        Voxel v = m_world.get_voxel({x, y, z});
        if (v.flags & VFLAG_SOLID) return true;
    }
    return false;
}

glm::vec3 PhysicsSystem::resolve_collisions(glm::vec3 pos, glm::vec3 delta,
                                             float radius, float height) const
{
    // Sweep each axis independently (simple axis-separated method)
    glm::vec3 result = pos;

    auto aabb_min = [&](glm::vec3 p) {
        return p + glm::vec3(-radius, 0,       -radius);
    };
    auto aabb_max = [&](glm::vec3 p) {
        return p + glm::vec3( radius, height,   radius);
    };

    // X
    result.x += delta.x;
    if (overlaps_solid(aabb_min(result), aabb_max(result)))
        result.x = pos.x;

    // Y
    result.y += delta.y;
    if (overlaps_solid(aabb_min(result), aabb_max(result)))
        result.y = pos.y;

    // Z
    result.z += delta.z;
    if (overlaps_solid(aabb_min(result), aabb_max(result)))
        result.z = pos.z;

    return result;
}
