#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  ProjectileSystem — fired projectile entities that move through the world,
//  hit mobs, and apply damage.
//
//  Mirrors TG SS13's /obj/item/projectile design:
//   • Projectiles spawn at the muzzle, move in a straight line each tick.
//   • On voxel collision → destroyed (hit a wall).
//   • On mob collision  → apply damage + optional stun/stam effects, destroy.
//
//  Tag conventions on gun ItemDefs (parsed by verb_fire in main.cpp):
//   proj_dmg:<N>      — hit damage (brute for bullets, burn for energy)
//   proj_type:<s>     — "bullet", "shot", "laser", "taser", "disabler", "arrow"
//   proj_speed:<N>    — travel speed in m/s (default 30)
//   proj_stun:<N>     — hard stun seconds on hit  (energy weapons)
//   proj_stam:<N>     — stamina damage on hit      (taser/disabler)
//   mag_size:<N>      — magazine capacity for lazy ammo init
//   energy_max:<N>    — energy-gun cell capacity (substitutes for mag_size)
// ─────────────────────────────────────────────────────────────────────────────

#include "core/types.h"
#include "core/entity_manager.h"
#include "core/world.h"
#include "core/signals.h"
#include <glm/glm.hpp>
#include <string>

// ── ProjectileComponent ───────────────────────────────────────────────────────
// Attached to every in-flight projectile entity.
struct ProjectileComponent {
    glm::vec3   direction{};        // normalised travel direction
    float       speed       = 30.f; // m/s
    float       damage      = 20.f;
    std::string damage_type = "brute"; // "brute" or "burn"
    EntityID    owner       = NULL_ENTITY; // firer — won't self-hit
    float       age         = 0.f;  // seconds since spawn
    float       lifetime    = 2.f;  // destroy after this many seconds if no hit
    float       stam_damage = 0.f;  // stamina damage on hit (taser/disabler)
    float       stun_dur    = 0.f;  // hard stun seconds on hit (energy guns)
    float       hit_radius  = 0.25f; // mob-hit detection sphere radius (metres)
};

// ── ProjectileSystem ──────────────────────────────────────────────────────────
// Owns no data — operates purely on EntityManager components.
// Call tick() once per simulation frame (after physics, before rendering).
class ProjectileSystem {
public:
    ProjectileSystem(World& world, EntityManager& entities);

    // Advance all in-flight projectiles.
    // Applies damage, fires signals, destroys entities on impact or expiry.
    void tick(double dt, SignalBus& signals);

    // Spawn a new projectile entity at world-space origin.
    // Returns the new EntityID.
    EntityID spawn(glm::vec3 origin, const ProjectileComponent& props);

private:
    // Returns true if pos is inside (or enters) a solid voxel.
    bool is_solid(glm::vec3 pos) const;

    World&         m_world;
    EntityManager& m_entities;
};
