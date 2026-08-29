#include "simulation/projectile_system.h"
#include "simulation/mob_system.h"      // HealthComponent, CorpseComponent
#include "simulation/status_effects.h"  // StatusEffectsComponent, StatusEffectType
#include "simulation/physics.h"         // TransformComponent
#include "core/signals.h"
#include <SDL3/SDL.h>
#include <cmath>
#include <vector>

ProjectileSystem::ProjectileSystem(World& world, EntityManager& entities)
    : m_world(world), m_entities(entities)
{}

bool ProjectileSystem::is_solid(glm::vec3 pos) const
{
    glm::ivec3 cell{
        static_cast<int>(std::floor(pos.x)),
        static_cast<int>(std::floor(pos.y)),
        static_cast<int>(std::floor(pos.z))
    };
    const Voxel v = m_world.get_voxel(cell);
    return (v.flags & VFLAG_SOLID) && !(v.flags & VFLAG_PASSABLE);
}

EntityID ProjectileSystem::spawn(glm::vec3 origin, const ProjectileComponent& props)
{
    EntityID id = m_entities.create();
    TransformComponent tc{};
    tc.pos = origin;
    m_entities.add_component<TransformComponent>(id, tc);
    m_entities.add_component<ProjectileComponent>(id, props);
    return id;
}

void ProjectileSystem::tick(double dt, SignalBus& sig)
{
    const float fdt = static_cast<float>(dt);

    // Collect entities to destroy after iteration (can't modify while iterating)
    std::vector<EntityID> to_destroy;

    m_entities.each<ProjectileComponent>([&](EntityID id, ProjectileComponent& proj) {
        auto* tc = m_entities.get_component<TransformComponent>(id);
        if (!tc) { to_destroy.push_back(id); return; }

        // Advance age; destroy on expiry
        proj.age += fdt;
        if (proj.age >= proj.lifetime) {
            to_destroy.push_back(id);
            return;
        }

        // Step forward in direction * speed * dt
        glm::vec3 prev_pos  = tc->pos;
        glm::vec3 step      = proj.direction * proj.speed * fdt;
        glm::vec3 next_pos  = prev_pos + step;

        // ── Voxel (wall) collision ────────────────────────────────────────────
        // Check a few sub-steps along the step to catch thin walls.
        const int   SUB = 4;
        bool hit_wall = false;
        for (int s = 1; s <= SUB; ++s) {
            glm::vec3 sample = prev_pos + step * (static_cast<float>(s) / SUB);
            if (is_solid(sample)) {
                hit_wall = true;
                break;
            }
        }
        if (hit_wall) {
            to_destroy.push_back(id);
            return;
        }

        // Move to new position
        tc->pos = next_pos;

        // ── Mob collision ─────────────────────────────────────────────────────
        // Find any mob whose position is within hit_radius of our new position,
        // excluding the owner.
        EntityID victim = NULL_ENTITY;
        float    best_dist2 = proj.hit_radius * proj.hit_radius;

        m_entities.each<HealthComponent>([&](EntityID mid, HealthComponent& /*hp*/) {
            if (mid == proj.owner) return;
            auto* mt = m_entities.get_component<TransformComponent>(mid);
            if (!mt) return;
            float d2 = 0.f;
            d2 += (mt->pos.x - next_pos.x) * (mt->pos.x - next_pos.x);
            d2 += (mt->pos.y - next_pos.y) * (mt->pos.y - next_pos.y);
            d2 += (mt->pos.z - next_pos.z) * (mt->pos.z - next_pos.z);
            if (d2 < best_dist2) {
                best_dist2 = d2;
                victim = mid;
            }
        });

        if (victim == NULL_ENTITY) return; // still in flight

        // ── Apply damage to victim ────────────────────────────────────────────
        auto* hp = m_entities.get_component<HealthComponent>(victim);
        if (hp) {
            hp->apply(proj.damage_type, proj.damage);

            // Stamina damage (taser/disabler)
            if (proj.stam_damage > 0.f) {
                hp->apply_stam(proj.stam_damage);
            }

            // Fire mob health update signal
            if (!hp->dead) {
                sig.send_signal(victim, COMSIG_MOB_HEALTHUPDATE,
                    SigAttacked{ proj.owner, proj.damage, proj.damage_type, true });
            } else {
                // Just killed — attach corpse component if not already present
                if (!m_entities.get_component<CorpseComponent>(victim)) {
                    CorpseComponent cc{};
                    cc.cause_of_death = "projectile";
                    m_entities.add_component<CorpseComponent>(victim, cc);
                }
                sig.send_signal(victim, COMSIG_MOB_LIVING_DEATH,
                    SigDeath{ proj.owner });
            }
        }

        // Hard stun (energy guns)
        if (proj.stun_dur > 0.f) {
            auto* se = m_entities.get_component<StatusEffectsComponent>(victim);
            if (se) se->apply(StatusEffectType::Stun, proj.stun_dur);
        }

        // Fire COMSIG_ATOM_ATTACKED on victim
        sig.send_signal(victim, COMSIG_ATOM_ATTACKED,
            SigAttacked{ proj.owner, proj.damage, proj.damage_type, true });

        SDL_Log("[projectile] %u hit %u for %.1f %s",
                proj.owner, victim, proj.damage, proj.damage_type.c_str());

        to_destroy.push_back(id);
    });

    for (EntityID id : to_destroy) {
        if (m_entities.alive(id))
            m_entities.destroy(id);
    }
}
