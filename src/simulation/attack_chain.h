#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  Attack chain — mirrors TG SS13's attack/click chain.
//
//  TG call order on left-click:
//    atom/proc/click(location, control, params)
//      → if target is mob:
//          mob/proc/attack_chain(target, user, def_zone)
//            → if user has weapon in hand:
//                item/proc/attack(target, user, def_zone)   [item attacks mob]
//                  SEND_SIGNAL(src, COMSIG_ITEM_ATTACK, ...)
//                  SEND_SIGNAL(target, COMSIG_ATOM_ATTACKED, ...)
//            → else:
//                mob/proc/attack_hand(user, modifiers)      [empty-hand attack]
//                  SEND_SIGNAL(src, COMSIG_ATOM_ATTACK_HAND, ...)
//
//  We also handle:
//    • bump_attack — called from PhysicsSystem when movement into a dense entity
//      is blocked; walks the same chain with an optional weapon.
//    • grab_entity / upgrade_grab — TG's grab cycling (passive→aggressive→neck→kill)
//
//  Usage
//  ─────
//    AttackContext ctx;
//    ctx.attacker    = player;
//    ctx.target      = mob_entity;
//    ctx.weapon      = &held_item;   // may be nullptr (empty-hand)
//    ctx.click_pos   = hit.hit_pos;
//    AttackResult r  = attack_chain(ctx, entities, signals());
// ─────────────────────────────────────────────────────────────────────────────

#include "core/types.h"
#include "core/entity_manager.h"
#include "core/signals.h"
#include "inventory/inventory.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>

// ── Attack result enum ────────────────────────────────────────────────────────
enum class AttackResult : uint8_t {
    Missed,           // out of range, or parried / blocked
    HitMob,           // struck a mob entity
    HitObject,        // struck an obj / world item
    BlockedByItem,    // target's held item blocked the hit
    Disarmed,         // empty-hand disarm succeeded
    GrabInitiated,    // grab started / upgraded
};

// ── Attack context ────────────────────────────────────────────────────────────
struct AttackContext {
    EntityID         attacker   = NULL_ENTITY;
    EntityID         target     = NULL_ENTITY;
    const ItemStack* weapon     = nullptr;  // null = empty hand
    glm::vec3        click_pos{};           // world-space hit point
    float            reach      = 1.8f;    // max attack range in metres

    // TG body-zone targeting (for future limb damage)
    std::string      def_zone   = "chest";

    // Filled in by attack_chain():
    float            damage_dealt = 0.f;
    std::string      damage_type  = "brute";
    bool             hit          = false;
};

// ── Grab state (TG cycling) ───────────────────────────────────────────────────
enum class GrabState : uint8_t {
    None       = 0,
    Passive    = 1,   // grabbed, can still act
    Aggressive = 2,   // partially immobilised
    Neck       = 3,   // strangling – causes oxy damage over time
    Kill       = 4,   // lethal choke
};

// Component: this entity is being grabbed by someone.
struct GrabbedComponent {
    EntityID  grabber     = NULL_ENTITY;
    GrabState state       = GrabState::Passive;
    float     upgrade_at  = 0.f;  // game time when grab can be upgraded
};

// ── API ───────────────────────────────────────────────────────────────────────

// Run the full TG attack / click chain.
// Fires COMSIG_ATOM_ATTACKED / COMSIG_ATOM_ATTACK_HAND on the target.
// Applies damage to the target's HealthComponent.
// Returns the outcome.
AttackResult attack_chain(AttackContext& ctx,
                          EntityManager& entities,
                          SignalBus&     signals);

// Called by PhysicsSystem when an entity's movement vector is blocked by a
// dense entity.  Equivalent to TG's Bump() proc.
// If bumper has a weapon in their active hand, triggers attack_chain.
// If empty-handed, performs a passive bump (no damage, just signal).
void bump_attack(EntityID bumper, EntityID bumpee,
                 EntityManager& entities,
                 SignalBus&     signals);

// ── Grab system (TG cycling) ─────────────────────────────────────────────────
// Initiate or upgrade a grab on the target:
//   None → Passive, Passive → Aggressive, Aggressive → Neck, Neck → Kill
// Applies status effects to the grabbed entity as the grab escalates.
// Returns true if grab was initiated or upgraded, false if already at max.
bool upgrade_grab(EntityID grabber, EntityID grabbed,
                  EntityManager& entities,
                  SignalBus&     signals);

// Release an existing grab and remove all associated status effects.
void release_grab(EntityID grabber, EntityID grabbed,
                  EntityManager& entities,
                  SignalBus&     signals);

// ── Default damage values ─────────────────────────────────────────────────────
// Base unarmed damage (matches TG: 5 brute per punch, rounded per species)
constexpr float BASE_UNARMED_DAMAGE = 5.f;

// Distance within which a melee attack can connect (same as TG ~1.5 tiles ≈ 1.8 m)
constexpr float MELEE_REACH = 1.8f;
