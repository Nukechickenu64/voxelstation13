#include "simulation/physics.h"
#include "simulation/model_objects.h"
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
            // Wall grab (Ctrl): active as long as within ~1 voxel of any solid surface
            if (cc->grab_wall) {
                // Broad probe — stays active within roughly one voxel of a wall
                constexpr float WALL_NEAR = 0.6f;
                bool near_wall = overlaps_solid(
                    tr.pos + glm::vec3(-cc->radius - WALL_NEAR, -WALL_NEAR,              -cc->radius - WALL_NEAR),
                    tr.pos + glm::vec3( cc->radius + WALL_NEAR,  cc->height + WALL_NEAR,  cc->radius + WALL_NEAR));
                if (near_wall) {
                    // Quake-style accel toward wish_move, with high friction when idle
                    // — same feel as the jetpack but surface-anchored.
                    constexpr float WALL_ACCEL     = 30.f;
                    constexpr float WALL_MAX_SPEED =  6.f;
                    constexpr float WALL_FRICTION_K = 12.f;

                    // Friction — exponential decay so the player stops when no input
                    float fric = std::exp(-WALL_FRICTION_K * fdt);
                    vel->linear.x *= fric;
                    vel->linear.y *= fric;
                    vel->linear.z *= fric;

                    // Accelerate toward wish (full 3D, like jetpack)
                    float wlen = glm::length(cc->wish_move);
                    if (wlen > 0.f) {
                        glm::vec3 wish_n = cc->wish_move / wlen;
                        float cur     = glm::dot(vel->linear, wish_n);
                        float add     = std::min(WALL_ACCEL * fdt, WALL_MAX_SPEED - cur);
                        if (add > 0.f)
                            vel->linear += wish_n * add;
                    }

                    // Cap total speed
                    float spd = glm::length(vel->linear);
                    if (spd > WALL_MAX_SPEED)
                        vel->linear = (vel->linear / spd) * WALL_MAX_SPEED;

                    cc->jetpack_input = {};

                    glm::vec3 delta   = vel->linear * fdt;
                    glm::vec3 new_pos = resolve_collisions(tr.pos, delta, cc->radius, cc->height);

                    if (std::abs(new_pos.x - (tr.pos.x + delta.x)) > 0.0001f) vel->linear.x = 0.f;
                    if (std::abs(new_pos.y - (tr.pos.y + delta.y)) > 0.0001f) vel->linear.y = 0.f;
                    if (std::abs(new_pos.z - (tr.pos.z + delta.z)) > 0.0001f) vel->linear.z = 0.f;

                    tr.pos        = new_pos;
                    cc->on_ground = false;
                    return;
                }
            }

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
            glm::vec3 new_pos = resolve_collisions(tr.pos, delta, cc->radius, cc->height);

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
            glm::vec3 new_pos = resolve_collisions(tr.pos, delta, cc->radius, cc->height);

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


void PhysicsSystem::prepare_character_movement(EntityID id, glm::vec3 wish_dir, bool sprint, bool grab_wall)
{
    auto* vel = m_entities.get_component<VelocityComponent>(id);
    auto* cc  = m_entities.get_component<CharacterControllerComponent>(id);
    if (!vel || !cc) return;

    cc->sprinting = sprint;
    cc->grab_wall = grab_wall;

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
        float wlen = glm::length(wish_dir);
        glm::vec3 wish_n = (wlen > 0.f) ? (wish_dir / wlen) : glm::vec3{};
        float speed = cc->move_speed * (cc->sprinting ? cc->sprint_mult : 1.f);

        if (cc->grab_wall) {
            // Wall crawl: store wish_move so tick() can drive velocity directly.
            cc->wish_move     = wish_n * speed;
            cc->jetpack_input = {};
        } else if (cc->jetpack_equipped) {
            // Store normalised thrust direction; tick() will apply it with dt.
            cc->jetpack_input = wish_n;
            cc->wish_move     = {};
        } else {
            // No jetpack, no grab — player is helpless. Preserve momentum.
            cc->jetpack_input = {};
            cc->wish_move     = {};
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
                                    bool sprint, bool grab_wall, double dt)
{
    prepare_character_movement(id, wish_dir, sprint, grab_wall);
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
        // Static 3-D model objects block movement when blocks_mobs = true
        if (m_model_objects && m_model_objects->blocks_mob_at({x, y, z})) return true;
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
