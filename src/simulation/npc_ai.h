#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  NPC AI — autonomous wander / flee / chase behaviour for non-player mobs.
//
//  tick_npc_ai() is called once per server tick, before PhysicsSystem::tick().
//  It reads each NpcAiComponent and writes cc->wish_move so physics sees a
//  valid movement vector.  When the entity is dead, wish_move is zeroed.
//
//  States (NpcAiState, defined in mob_system.h):
//    Idle   — stationary; idle_timer counts down then transitions to Wander
//    Wander — moving toward a random point near spawn_pos; wander_timer acts
//             as a deadline in case the destination is unreachable
//    Flee   — fleeing directly away from threat_eid; speed scaled by
//             flee_speed_mult.  flee_timer prevents rubber-banding.
//    Chase  — pursuing threat_eid; stops at ~1.2 m (bump range).
//
//  Sense priority: Flee > Chase > Wander/Idle.
//  Set flee_dist > 0 to enable flee sensing; aggro_dist > 0 for chase sensing.
// ─────────────────────────────────────────────────────────────────────────────

#include "core/entity_manager.h"
#include "core/world.h"

// Update all NpcAiComponent entities for one tick of dt seconds.
// Sets cc->wish_move on their CharacterControllerComponent so the physics
// system can move them.  Must be called before PhysicsSystem::tick().
void tick_npc_ai(EntityManager& entities, const World& world, double dt);
