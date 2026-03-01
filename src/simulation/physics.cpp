#include "simulation/physics.h"
#include "simulation/model_objects.h"
#include "simulation/mob_system.h"   // DensityComponent
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
        if (!cc || !cc->on_ground) {
            vel->linear.y += GRAVITY * fdt;
            // Terminal velocity: prevent delta.y from exceeding 1 voxel per tick
            // which would cause the player to tunnel through floors/ceilings.
            constexpr float TERMINAL_VEL = -50.f;
            if (vel->linear.y < TERMINAL_VEL)
                vel->linear.y = TERMINAL_VEL;
        }

        // ── Character controller ──────────────────────────────────────────
        if (cc) {
            // Prone mobs (Hardcrit / Softcrit) use a reduced collider height so
            // they slide along the ground and upright entities can step over them.
            const float cc_height = (cc->mob_state == MobState::Hardcrit ||
                                     cc->mob_state == MobState::Softcrit)
                                    ? std::min(cc->height, 0.3f)
                                    : cc->height;

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
            glm::vec3 new_pos = resolve_collisions(tr.pos, delta, cc->radius, cc_height);

            if (std::abs(new_pos.x - (tr.pos.x + delta.x)) > 0.0001f) vel->linear.x = 0.f;
            if (std::abs(new_pos.z - (tr.pos.z + delta.z)) > 0.0001f) vel->linear.z = 0.f;
            if (vel->linear.y > 0.f &&
                std::abs(new_pos.y - (tr.pos.y + delta.y)) > 0.0001f)
                vel->linear.y = 0.f;

            // ── Density check: capsule-slide around other dense entities ────────
            // When the proposed position overlaps a dense entity, try to slide
            // the movement around the other mob's circular/capsule footprint.
            // The mover's velocity is projected onto the tangent of the line
            // connecting the two mob centres (XZ plane), and that perpendicular
            // component is attempted as the slide motion.  Only if the slide
            // position is also blocked (or velocity is purely head-on) does
            // movement stop and the bump callback fire.
            if (m_bump_cb) {
                EntityID blocker = NULL_ENTITY;
                glm::vec3 nmin = new_pos + glm::vec3(-cc->radius, 0.f,       -cc->radius);
                glm::vec3 nmax = new_pos + glm::vec3( cc->radius, cc_height,  cc->radius);
                if (check_entity_density(id, nmin, nmax, blocker)) {
                    bool slid = false;

                    auto* blocker_tr = m_entities.get_component<TransformComponent>(blocker);
                    if (blocker_tr) {
                        // Vector from blocker centre → mover centre (XZ only)
                        glm::vec2 away{
                            tr.pos.x - blocker_tr->pos.x,
                            tr.pos.z - blocker_tr->pos.z
                        };
                        float away_len = glm::length(away);
                        glm::vec2 vel_xz{ vel->linear.x, vel->linear.z };

                        // If the centres are exactly coincident, pick an arbitrary
                        // separation direction so we don't divide by zero.
                        if (away_len < 0.0001f) {
                            away     = { 1.f, 0.f };
                            away_len = 1.f;
                        }

                        if (glm::length(vel_xz) > 0.0001f) {
                            // Tangent direction is perpendicular to the separation axis
                            glm::vec2 away_n  = away / away_len;
                            glm::vec2 tangent{ -away_n.y, away_n.x };

                            // Project velocity onto tangent — this is the slide speed
                            float slide_speed = glm::dot(vel_xz, tangent);

                            if (std::abs(slide_speed) > 0.0001f) {
                                glm::vec2 slide_vel = tangent * slide_speed;

                                // Calculate how far the mover needs to be pushed out of
                                // the blocker's footprint along the separation axis so
                                // the slide position starts outside the overlap region.
                                float b_rad = 0.3f;
                                if (auto* bcc_bl = m_entities.get_component<CharacterControllerComponent>(blocker))
                                    b_rad = bcc_bl->radius;
                                float min_sep   = cc->radius + b_rad + 0.01f;
                                float push_dist = (away_len < min_sep) ? (min_sep - away_len) : 0.f;

                                glm::vec3 slid_pos{
                                    tr.pos.x + away_n.x * push_dist + slide_vel.x * fdt,
                                    new_pos.y,
                                    tr.pos.z + away_n.y * push_dist + slide_vel.y * fdt
                                };
                                glm::vec3 smin = slid_pos + glm::vec3(-cc->radius, 0.f, -cc->radius);
                                glm::vec3 smax = slid_pos + glm::vec3( cc->radius, cc_height, cc->radius);
                                EntityID  dummy = NULL_ENTITY;
                                // Accept the slide only if it is clear of voxel
                                // geometry and dense entities OTHER than the mob
                                // we're explicitly sliding around.
                                if (!overlaps_solid(smin, smax) &&
                                    !check_entity_density(id, smin, smax, dummy, blocker)) {
                                    new_pos       = slid_pos;
                                    vel->linear.x = slide_vel.x;
                                    vel->linear.z = slide_vel.y;
                                    slid = true;
                                }
                            }
                        }
                    }

                    if (!slid) {
                        // Truly blocked — cancel horizontal movement and attack
                        new_pos.x     = tr.pos.x;
                        new_pos.z     = tr.pos.z;
                        vel->linear.x = 0.f;
                        vel->linear.z = 0.f;
                        m_bump_cb(id, blocker);
                    }
                }
            }

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
    auto aabb_min = [&](glm::vec3 p) {
        return p + glm::vec3(-radius, 0,      -radius);
    };
    auto aabb_max = [&](glm::vec3 p) {
        return p + glm::vec3( radius, height,  radius);
    };

    // Sub-step to prevent tunneling: each step must be small enough that the
    // AABB cannot skip over a 1-voxel-wide wall.  A step size of 0.5 units
    // guarantees the AABB always overlaps any voxel it passes through because
    // the AABB half-extents (radius=0.3, height=0.9) are wider than 0.5 in
    // every direction.
    constexpr float MAX_STEP = 0.5f;
    const float max_delta = std::max({std::abs(delta.x),
                                      std::abs(delta.y),
                                      std::abs(delta.z)});
    const int steps = (max_delta > MAX_STEP)
                      ? static_cast<int>(std::ceil(max_delta / MAX_STEP))
                      : 1;
    const glm::vec3 step_delta = delta / static_cast<float>(steps);

    glm::vec3 result = pos;

    for (int s = 0; s < steps; ++s) {
        const glm::vec3 step_start = result;

        // X
        result.x += step_delta.x;
        if (overlaps_solid(aabb_min(result), aabb_max(result)))
            result.x = step_start.x;

        // Y
        result.y += step_delta.y;
        if (overlaps_solid(aabb_min(result), aabb_max(result)))
            result.y = step_start.y;

        // Z
        result.z += step_delta.z;
        if (overlaps_solid(aabb_min(result), aabb_max(result)))
            result.z = step_start.z;
    }

    return result;
}

// ── check_entity_density ─────────────────────────────────────────────────────
// Returns true if any dense entity (other than `mover`) has its centre inside
// the AABB [mn, mx].  Sets out_blocker to that entity (or NULL_ENTITY).
// This is intentionally simple (point-vs-AABB) for the skeleton; a more robust
// implementation would do AABB-vs-AABB but that requires knowing every entity's
// extents at query time.
bool PhysicsSystem::check_entity_density(EntityID mover,
                                          glm::vec3 mn, glm::vec3 mx,
                                          EntityID& out_blocker,
                                          EntityID exclude2) const
{
    out_blocker = NULL_ENTITY;
    bool found = false;

    m_entities.each<DensityComponent>([&](EntityID id, DensityComponent& dc) {
        if (found || id == mover || id == exclude2 || !dc.dense) return;
        auto* tr = m_entities.get_component<TransformComponent>(id);
        if (!tr) return;

        // AABB-vs-AABB check — use the blocker's CharacterControllerComponent
        // extents when available; fall back to a small fixed capsule otherwise.
        float b_rad = 0.3f, b_ht = 0.9f;
        if (auto* bcc = m_entities.get_component<CharacterControllerComponent>(id)) {
            b_rad = bcc->radius;
            // Prone mobs shrink their collider — use the same effective height
            // as the physics tick so you can step over a knocked-down body.
            b_ht = (bcc->mob_state == MobState::Hardcrit ||
                    bcc->mob_state == MobState::Softcrit)
                   ? std::min(bcc->height, 0.3f)
                   : bcc->height;
        }
        const glm::vec3 bmin = tr->pos + glm::vec3(-b_rad, 0.f,   -b_rad);
        const glm::vec3 bmax = tr->pos + glm::vec3( b_rad,  b_ht,  b_rad);
        if (bmax.x >= mn.x && bmin.x <= mx.x &&
            bmax.y >= mn.y && bmin.y <= mx.y &&
            bmax.z >= mn.z && bmin.z <= mx.z) {
            out_blocker = id;
            found = true;
        }
    });

    return found;
}
