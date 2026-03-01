#include "simulation/npc_ai.h"
#include "simulation/mob_system.h"
#include "simulation/physics.h"
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <cmath>
#include <cstdlib>
#include <limits>

// ── Pseudo-random float in [lo, hi] ──────────────────────────────────────────
static float randf(float lo, float hi)
{
    return lo + (hi - lo) * (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX));
}

// ── Find nearest MobPlayerTag entity and return its transform ─────────────────
// Returns NULL_ENTITY if no player is found.
static EntityID nearest_player(EntityID self_eid, EntityManager& entities,
                                const TransformComponent& self_tr,
                                float max_dist, float& out_dist)
{
    EntityID best      = NULL_ENTITY;
    float    best_dist = max_dist + 0.001f;

    entities.each<MobPlayerTag>([&](EntityID pid, MobPlayerTag&) {
        if (pid == self_eid) return;
        auto* ptr = entities.get_component<TransformComponent>(pid);
        if (!ptr) return;
        glm::vec3 delta = ptr->pos - self_tr.pos;
        delta.y = 0.f;
        float d = glm::length(delta);
        if (d < best_dist) {
            best_dist = d;
            best      = pid;
        }
    });

    out_dist = best_dist;
    return best;
}

// ─────────────────────────────────────────────────────────────────────────────
void tick_npc_ai(EntityManager& entities, const World& /*world*/, double dt)
{
    const float fdt = static_cast<float>(dt);

    entities.each<NpcAiComponent>([&](EntityID eid, NpcAiComponent& ai) {
        auto* tr = entities.get_component<TransformComponent>(eid);
        auto* cc = entities.get_component<CharacterControllerComponent>(eid);
        if (!tr || !cc) return;

        // Dead mobs stop thinking — zero wish_move so leftover velocity drains via friction
        auto* hp = entities.get_component<HealthComponent>(eid);
        if (hp && hp->dead) {
            cc->wish_move = {};
            return;
        }

        // Hard-crit / paralysis: don't issue AI movement
        if (cc->mob_state == MobState::Hardcrit) {
            cc->wish_move = {};
            return;
        }

        // ── Sense phase: check for nearby players ────────────────────────────
        // Flee takes priority over Chase.  Only runs when at least one sense
        // radius is configured to avoid scanning every tick for vanilla mobs.
        const bool has_flee  = (ai.flee_dist  > 0.f);
        const bool has_aggro = (ai.aggro_dist > 0.f);

        if (has_flee || has_aggro) {
            float sense_range = std::max(ai.flee_dist, ai.aggro_dist);
            float player_dist = sense_range + 1.f;
            EntityID pid = nearest_player(eid, entities, *tr, sense_range, player_dist);

            if (has_flee && pid != NULL_ENTITY && player_dist <= ai.flee_dist) {
                // Threat detected — enter/renew Flee
                ai.state      = NpcAiState::Flee;
                ai.threat_eid = pid;
                ai.flee_timer = 2.0f;   // keep fleeing ≥2 s even if threat backs off
            } else if (has_aggro && pid != NULL_ENTITY && player_dist <= ai.aggro_dist
                       && ai.state != NpcAiState::Flee) {
                // Prey detected — enter Chase (only if not already fleeing)
                ai.state      = NpcAiState::Chase;
                ai.threat_eid = pid;
            } else if (ai.state == NpcAiState::Flee) {
                // Drain forced-flee timer; return to wander when it expires
                ai.flee_timer -= fdt;
                if (ai.flee_timer <= 0.f) {
                    ai.threat_eid = NULL_ENTITY;
                    cc->wish_move  = {};
                    ai.idle_timer  = randf(0.5f, 2.0f);
                    ai.state       = NpcAiState::Idle;
                }
            } else if (ai.state == NpcAiState::Chase && pid == NULL_ENTITY) {
                // Lost sight — give up and idle
                ai.threat_eid = NULL_ENTITY;
                cc->wish_move  = {};
                ai.idle_timer  = randf(1.f, 3.f);
                ai.state       = NpcAiState::Idle;
            }
        }

        // ── Behaviour phase ──────────────────────────────────────────────────
        switch (ai.state) {

        // ── Idle ─────────────────────────────────────────────────────────────
        case NpcAiState::Idle:
            cc->wish_move = {};
            ai.idle_timer -= fdt;
            if (ai.idle_timer <= 0.f && ai.wanders) {
                // Pick a random destination within wander_radius of the spawn spot
                float angle = randf(0.f, 6.2832f);
                float dist  = randf(1.5f, ai.wander_radius);
                ai.wander_target = ai.spawn_pos + glm::vec3{
                    std::cos(angle) * dist,
                    0.f,
                    std::sin(angle) * dist
                };
                ai.wander_timer = 8.f;  // give up after 8 s
                ai.state        = NpcAiState::Wander;
            }
            break;

        // ── Wander ───────────────────────────────────────────────────────────
        case NpcAiState::Wander: {
            ai.wander_timer -= fdt;
            glm::vec3 to = ai.wander_target - tr->pos;
            to.y = 0.f;
            float d = glm::length(to);

            if (d < 0.4f || ai.wander_timer <= 0.f) {
                // Arrived or timed out — rest for 1–4 s then wander again
                cc->wish_move = {};
                ai.idle_timer = randf(1.f, 4.f);
                ai.state      = NpcAiState::Idle;
            } else {
                // Advance toward target
                glm::vec3 dir = to / d;
                cc->wish_move = dir * cc->move_speed;
                // Rotate to face direction of travel
                tr->yaw = glm::degrees(std::atan2(dir.x, -dir.z));
            }
            break;
        }

        // ── Flee ─────────────────────────────────────────────────────────────
        // Move directly away from threat_eid.  Mob faces away from the threat.
        case NpcAiState::Flee: {
            auto* threat_tr = entities.get_component<TransformComponent>(ai.threat_eid);
            if (!threat_tr) {
                // Threat disappeared — idle
                cc->wish_move = {};
                ai.idle_timer = randf(0.5f, 1.5f);
                ai.state      = NpcAiState::Idle;
                break;
            }

            glm::vec3 away = tr->pos - threat_tr->pos;
            away.y = 0.f;
            float d = glm::length(away);
            if (d < 0.001f) {
                // Exactly on top — pick a random escape direction
                float angle = randf(0.f, 6.2832f);
                away = {std::cos(angle), 0.f, std::sin(angle)};
                d    = 1.f;
            }
            glm::vec3 dir = away / d;
            cc->wish_move = dir * (cc->move_speed * ai.flee_speed_mult);
            // Face away from threat (mob looks in the direction it's running)
            tr->yaw = glm::degrees(std::atan2(dir.x, -dir.z));
            break;
        }

        // ── Chase ─────────────────────────────────────────────────────────────
        // Advance toward threat_eid.  Mob faces toward the target.
        case NpcAiState::Chase: {
            auto* target_tr = entities.get_component<TransformComponent>(ai.threat_eid);
            if (!target_tr) {
                cc->wish_move = {};
                ai.idle_timer = randf(1.f, 3.f);
                ai.state      = NpcAiState::Idle;
                break;
            }

            glm::vec3 to = target_tr->pos - tr->pos;
            to.y = 0.f;
            float d = glm::length(to);

            // Stop right outside bump range (~1.2 m)
            if (d < 1.2f) {
                cc->wish_move = {};
                // Face target while standing close
                if (d > 0.05f) {
                    glm::vec3 dir = to / d;
                    tr->yaw = glm::degrees(std::atan2(dir.x, -dir.z));
                }
            } else {
                glm::vec3 dir = to / d;
                cc->wish_move = dir * cc->move_speed;
                tr->yaw = glm::degrees(std::atan2(dir.x, -dir.z));
            }
            break;
        }

        } // switch
    });
}
