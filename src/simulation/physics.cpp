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
    const float fdt = static_cast<float>(dt);

    // Friction: exponential decay coefficient k — vel_horiz *= exp(-k * dt)
    //   k=10 on ground: stops in ~0.3 s when no input (snappy but not instant)
    //   k=0.4 in air:   barely damps so you keep momentum while airborne
    constexpr float GROUND_FRICTION_K = 10.f;
    constexpr float AIR_FRICTION_K    =  0.4f;
    // Quake-style additive acceleration (m/s²) toward wish_move
    constexpr float GROUND_ACCEL = 30.f;  // reach full speed in ~2 ticks on ground
    constexpr float AIR_ACCEL    =  6.f;  // gentle air-steering
    // Generic rigid body AABB
    constexpr float ITEM_RADIUS = 0.15f;
    constexpr float ITEM_HEIGHT = 0.15f;

    m_entities.each<TransformComponent>([&](EntityID id, TransformComponent& tr) {
        auto* vel = m_entities.get_component<VelocityComponent>(id);
        if (!vel) return;
        auto* cc = m_entities.get_component<CharacterControllerComponent>(id);

        tr.prev_pos = tr.pos;

        // ── Noclip ────────────────────────────────────────────────────────
        if (cc && cc->noclip) {
            tr.pos += vel->linear * fdt;
            cc->on_ground = false;
            return;
        }

        // ── Zero-G: pure momentum, no gravity, no friction ────────────────
        if (cc && cc->zero_g) {
            float thrust_len = glm::length(cc->jetpack_input);
            if (cc->jetpack_equipped && thrust_len > 0.f) {
                glm::vec3 dir = cc->jetpack_input / thrust_len;
                vel->linear += dir * JETPACK_ACCEL * fdt;
                float spd = glm::length(vel->linear);
                if (spd > JETPACK_MAX_SPEED)
                    vel->linear = (vel->linear / spd) * JETPACK_MAX_SPEED;
            }
            cc->jetpack_input = {};

            glm::vec3 delta = vel->linear * fdt;
            float h = cc->crouching ? cc->height * 0.5f : cc->height;
            glm::vec3 new_pos = resolve_collisions(tr.pos, delta, cc->radius, h);

            if (std::abs(new_pos.x - (tr.pos.x + delta.x)) > 0.0001f) vel->linear.x = 0.f;
            if (std::abs(new_pos.y - (tr.pos.y + delta.y)) > 0.0001f) vel->linear.y = 0.f;
            if (std::abs(new_pos.z - (tr.pos.z + delta.z)) > 0.0001f) vel->linear.z = 0.f;

            tr.pos = new_pos;
            cc->on_ground = false;
            return;
        }

        // ── Gravity ───────────────────────────────────────────────────────
        if (!cc || !cc->on_ground)
            vel->linear.y += GRAVITY * fdt;

        // ── Character controller ──────────────────────────────────────────
        if (cc) {
            // 1. Friction (horizontal only)
            float k = cc->on_ground ? GROUND_FRICTION_K : AIR_FRICTION_K;
            float f = std::exp(-k * fdt);
            vel->linear.x *= f;
            vel->linear.z *= f;

            // 2. Quake-style acceleration toward wish_move
            glm::vec3 wish_h{cc->wish_move.x, 0.f, cc->wish_move.z};
            float wish_speed = glm::length(wish_h);
            if (wish_speed > 0.f) {
                glm::vec3 wish_n = wish_h / wish_speed;
                float cur = glm::dot(glm::vec3(vel->linear.x, 0.f, vel->linear.z), wish_n);
                float add = wish_speed - cur;
                if (add > 0.f) {
                    float accel = cc->on_ground ? GROUND_ACCEL : AIR_ACCEL;
                    float accel_speed = std::min(accel * fdt * wish_speed, add);
                    vel->linear.x += wish_n.x * accel_speed;
                    vel->linear.z += wish_n.z * accel_speed;
                }
            }

            // 3. Move + collide
            glm::vec3 delta = vel->linear * fdt;
            float h = cc->crouching ? cc->height * 0.5f : cc->height;
            glm::vec3 new_pos = resolve_collisions(tr.pos, delta, cc->radius, h);

            if (std::abs(new_pos.x - (tr.pos.x + delta.x)) > 0.0001f) vel->linear.x = 0.f;
            if (std::abs(new_pos.z - (tr.pos.z + delta.z)) > 0.0001f) vel->linear.z = 0.f;
            if (vel->linear.y > 0.f &&
                std::abs(new_pos.y - (tr.pos.y + delta.y)) > 0.0001f)
                vel->linear.y = 0.f;

            tr.pos = new_pos;

            bool was_grounded = cc->on_ground;
            cc->on_ground = overlaps_solid(
                tr.pos + glm::vec3(-cc->radius, -0.1f, -cc->radius),
                tr.pos + glm::vec3( cc->radius,  0.0f,  cc->radius));

            if (cc->on_ground && !was_grounded && vel->linear.y < 0.f)
                vel->linear.y = 0.f;

        // ── Generic rigid body ────────────────────────────────────────────
        } else {
            glm::vec3 delta = vel->linear * fdt;
            glm::vec3 new_pos = resolve_collisions(tr.pos, delta, ITEM_RADIUS, ITEM_HEIGHT);

            if (std::abs(new_pos.x - (tr.pos.x + delta.x)) > 0.0001f) vel->linear.x = 0.f;
            if (std::abs(new_pos.z - (tr.pos.z + delta.z)) > 0.0001f) vel->linear.z = 0.f;
            if (std::abs(new_pos.y - (tr.pos.y + delta.y)) > 0.0001f) vel->linear.y = 0.f;

            tr.pos = new_pos;

            bool on_ground = overlaps_solid(
                tr.pos + glm::vec3(-ITEM_RADIUS, -0.05f, -ITEM_RADIUS),
                tr.pos + glm::vec3( ITEM_RADIUS,  0.0f,   ITEM_RADIUS));
            if (on_ground) {
                float f2 = std::exp(-GROUND_FRICTION_K * fdt);
                vel->linear.x *= f2;
                vel->linear.z *= f2;
            }
        }
    });
}


void PhysicsSystem::prepare_character_movement(EntityID id, glm::vec3 wish_dir,
                                                bool crouch, bool sprint)
{
    auto* vel = m_entities.get_component<VelocityComponent>(id);
    auto* cc  = m_entities.get_component<CharacterControllerComponent>(id);
    if (!vel || !cc) return;

    cc->crouching = crouch;
    cc->sprinting = sprint && !crouch;

    // ── Noclip: directly set velocity ───────────────────────────────
    if (cc->noclip) {
        float nlen = glm::length(wish_dir);
        if (nlen > 0.f) wish_dir /= nlen;
        float speed = cc->move_speed * (cc->sprinting ? cc->sprint_mult : 1.f);
        vel->linear = wish_dir * speed;
        return;
    }

    // ── Zero-G: no direct control — only a jetpack can apply thrust ──────
    if (cc->zero_g) {
        if (cc->jetpack_equipped) {
            // Store normalised thrust direction; tick() will apply it with dt
            float wlen = glm::length(wish_dir);
            cc->jetpack_input = (wlen > 0.f) ? (wish_dir / wlen) : glm::vec3{};
        } else {
            // No jetpack — player is helpless.  Preserve all existing momentum.
            cc->jetpack_input = {};
        }
        return;  // never override vel->linear here
    }

    // ── Normal walking — store desired velocity; tick() blends toward it ──
    float wlen = glm::length(wish_dir);
    if (wlen > 0.f) wish_dir /= wlen;
    float speed = cc->move_speed * (cc->sprinting ? cc->sprint_mult : 1.f);
    cc->wish_move = glm::vec3(wish_dir.x * speed, 0.f, wish_dir.z * speed);
}

void PhysicsSystem::move_character(EntityID id, glm::vec3 wish_dir,
                                    bool crouch, bool sprint,
                                    double dt)
{
    prepare_character_movement(id, wish_dir, crouch, sprint);
    tick(dt);
}

void PhysicsSystem::apply_wind(EntityID id, glm::vec3 force)
{
    auto* vel = m_entities.get_component<VelocityComponent>(id);
    if (vel) vel->linear += force;
}

bool PhysicsSystem::overlaps_solid(glm::vec3 min, glm::vec3 max) const
{
    using std::floor;
    glm::ivec3 imin{ (int)floor(min.x), (int)floor(min.y), (int)floor(min.z) };
    glm::ivec3 imax{ (int)floor(max.x), (int)floor(max.y), (int)floor(max.z) };
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
