#include "simulation/attack_chain.h"
#include "simulation/status_effects.h"
#include "simulation/physics.h"        // TransformComponent
#include "simulation/mob_system.h"     // HealthComponent
#include <SDL3/SDL.h>
#include <cmath>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Returns base damage for an item (falls back to unarmed if def is null).
static float item_damage(const ItemStack* weapon)
{
    if (!weapon || !weapon->def) return BASE_UNARMED_DAMAGE;

    // Search verbs for an "attack" verb with a declared damage value.
    // TG uses /obj/item/force; we use a tag "force:<N>" on the item def.
    for (const auto& tag : weapon->def->tags) {
        if (tag.rfind("force:", 0) == 0) {
            try { return std::stof(tag.substr(6)); } catch (...) {}
        }
    }

    // Fall back: weight-based estimation (heavier = harder hit)
    return std::clamp(weapon->def->weight * 10.f, 5.f, 30.f);
}

// Returns damage type for an item ("brute" default; "burn" for electric, etc.)
static std::string item_damage_type(const ItemStack* weapon)
{
    if (!weapon || !weapon->def) return "brute";
    for (const auto& tag : weapon->def->tags) {
        if (tag == "burn")   return "burn";
        if (tag == "tox")    return "tox";
        if (tag == "shock")  return "burn"; // electrified = burn
    }
    return "brute";
}

// Distance between two entity transform positions.
static float entity_dist(EntityID a, EntityID b, EntityManager& entities)
{
    auto* ta = entities.get_component<TransformComponent>(a);
    auto* tb = entities.get_component<TransformComponent>(b);
    if (!ta || !tb) return 1e9f;
    return glm::length(ta->pos - tb->pos);
}

// ─────────────────────────────────────────────────────────────────────────────
//  attack_chain
// ─────────────────────────────────────────────────────────────────────────────
AttackResult attack_chain(AttackContext& ctx,
                          EntityManager& entities,
                          SignalBus&     signals)
{
    if (ctx.attacker == NULL_ENTITY || ctx.target == NULL_ENTITY)
        return AttackResult::Missed;

    // ── Range check ───────────────────────────────────────────────────────────
    float dist = entity_dist(ctx.attacker, ctx.target, entities);
    if (dist > ctx.reach) {
        SDL_Log("[attack_chain] out of range: %.2f > %.2f", dist, ctx.reach);
        return AttackResult::Missed;
    }

    const bool is_empty_hand = (ctx.weapon == nullptr);

    // ── Signal: pre-attack (COMSIG_ATOM_CLICK on target) ─────────────────────
    // Handlers can return COMPONENT_SIGNAL_CANCEL to abort the hit.
    SignalResult click_res = signals.send_signal(ctx.target, COMSIG_ATOM_CLICK,
        SigAttacked{ ctx.attacker, 0.f, "", !is_empty_hand });
    if (click_res & COMPONENT_SIGNAL_CANCEL)
        return AttackResult::Missed;

    // ── Determine damage ──────────────────────────────────────────────────────
    float  dmg  = item_damage(ctx.weapon);
    std::string dmg_type = item_damage_type(ctx.weapon);

    // Attacker status modifiers (stun → 0.6× damage while stunned like TG)
    auto* attacker_status = entities.get_component<StatusEffectsComponent>(ctx.attacker);
    if (attacker_status) {
        float spd = attacker_status->speed_multiplier();
        dmg *= std::lerp(0.6f, 1.f, spd);
    }

    // Kick-while-down bonus: 1.5× damage vs prone or stunned targets (TG behaviour).
    // This mirrors TG's passive stun-multiplier for hits on incapacitated mobs.
    auto* target_se_kwd = entities.get_component<StatusEffectsComponent>(ctx.target);
    if (target_se_kwd) {
        MobState ts = target_se_kwd->current_mob_state();
        if (ts == MobState::Hardcrit || ts == MobState::Softcrit)
            dmg *= 1.5f;
    }

    ctx.damage_dealt = dmg;
    ctx.damage_type  = dmg_type;

    if (is_empty_hand) {
        // ── Empty-hand path ───────────────────────────────────────────────────
        // Signal the target that an empty hand is attacking
        SignalResult res = signals.send_signal(ctx.target, COMSIG_ATOM_ATTACK_HAND,
            SigAttackHand{ ctx.attacker });
        if (res & COMPONENT_SIGNAL_CANCEL)
            return AttackResult::Missed;

        // Apply unarmed damage
        auto* hp = entities.get_component<HealthComponent>(ctx.target);
        if (hp) {
            hp->apply("brute", dmg);
            ctx.hit = true;
            SDL_Log("[attack_chain] %u punched %u for %.1f brute (hp=%.1f)",
                    ctx.attacker, ctx.target, dmg, hp->current());
        }

        return AttackResult::HitMob;
    }

    // ── Weapon path ───────────────────────────────────────────────────────────
    // COMSIG_ITEM_ATTACK fires on the weapon item entity (if it has one).
    // Since WorldItemComponent might not be in hand, we fire on attacker for now.
    SignalResult item_res = signals.send_signal(ctx.attacker, COMSIG_ITEM_ATTACK,
        SigAttacked{ ctx.attacker, dmg, dmg_type, true });
    if (item_res & COMPONENT_SIGNAL_CANCEL)
        return AttackResult::Missed;

    // COMSIG_ATOM_ATTACKED fires on the target
    SignalResult atk_res = signals.send_signal(ctx.target, COMSIG_ATOM_ATTACKED,
        SigAttacked{ ctx.attacker, dmg, dmg_type, true });
    if (atk_res & COMPONENT_SIGNAL_CANCEL)
        return AttackResult::BlockedByItem;

    // Apply damage to target HealthComponent
    auto* hp = entities.get_component<HealthComponent>(ctx.target);
    if (hp) {
        hp->apply(dmg_type, dmg);
        ctx.hit = true;

        // Fire death signal if mob just died
        if (hp->dead) {
            signals.send_signal(ctx.target, COMSIG_MOB_LIVING_DEATH,
                SigDeath{ ctx.attacker });
        } else {
            signals.send_signal(ctx.target, COMSIG_MOB_HEALTHUPDATE,
                SigAttacked{ ctx.attacker, dmg, dmg_type, true });
        }

        SDL_Log("[attack_chain] %u hit %u for %.1f %s (hp=%.1f)",
                ctx.attacker, ctx.target,
                dmg, dmg_type.c_str(), hp->current());

        // Tool / item-specific: stun effect on targets (e.g. stun baton)
        auto* target_status = entities.get_component<StatusEffectsComponent>(ctx.target);
        if (target_status && ctx.weapon && ctx.weapon->def) {
            for (const auto& tag : ctx.weapon->def->tags) {
                if (tag.rfind("stun:", 0) == 0) {
                    float dur = 2.f;
                    try { dur = std::stof(tag.substr(5)); } catch (...) {}
                    target_status->apply(StatusEffectType::Stun, dur);
                } else if (tag.rfind("knockdown:", 0) == 0) {
                    float dur = 3.f;
                    try { dur = std::stof(tag.substr(10)); } catch (...) {}
                    target_status->apply(StatusEffectType::Knockdown, dur);
                }
            }
        }
    }

    return AttackResult::HitMob;
}

// ─────────────────────────────────────────────────────────────────────────────
//  bump_attack
// ─────────────────────────────────────────────────────────────────────────────
void bump_attack(EntityID bumper, EntityID bumpee,
                 EntityManager& entities,
                 SignalBus&     signals)
{
    if (bumper == NULL_ENTITY || bumpee == NULL_ENTITY || bumper == bumpee) return;

    // Fire COMSIG_ATOM_BUMPED on the bumpee so components can react
    signals.send_signal(bumpee, COMSIG_ATOM_BUMPED,
        SigBumped{ bumper, false /* no weapon info here without inventory ptr */ });

    // If bumpee is alive and bumper has a transform…
    auto* hp_target = entities.get_component<HealthComponent>(bumpee);
    if (!hp_target || hp_target->dead) return;

    // In TG: bumping with an item in hand triggers an attack.
    // Without inventory access here, we fire a passive bump (no damage).
    // The game loop (main.cpp) should call attack_chain() after bump_attack()
    // when the player is the bumper and has a weapon.
    SDL_Log("[bump_attack] %u bumped into %u", bumper, bumpee);
}

// ─────────────────────────────────────────────────────────────────────────────
//  upgrade_grab
// ─────────────────────────────────────────────────────────────────────────────
bool upgrade_grab(EntityID grabber, EntityID grabbed,
                  EntityManager& entities,
                  SignalBus&     signals)
{
    if (grabber == NULL_ENTITY || grabbed == NULL_ENTITY || grabber == grabbed) return false;

    auto* hp = entities.get_component<HealthComponent>(grabbed);
    if (hp && hp->dead) return false;  // can't grab corpses (yet)

    GrabbedComponent* gc = entities.get_component<GrabbedComponent>(grabbed);

    if (!gc || gc->grabber != grabber) {
        // ── Initiate new grab (Passive) ─────────────────────────────────────
        GrabbedComponent ng;
        ng.grabber    = grabber;
        ng.state      = GrabState::Passive;
        ng.upgrade_at = 0.f;
        entities.add_component<GrabbedComponent>(grabbed, ng);
        SDL_Log("[grab] %u initiated Passive grab on %u", grabber, grabbed);
        signals.send_signal(grabbed, COMSIG_ATOM_ATTACKED,
            SigAttacked{ grabber, 0.f, "grab", false });
        return true;
    }

    // ── Upgrade existing grab ────────────────────────────────────────────────
    auto* se = entities.get_component<StatusEffectsComponent>(grabbed);
    switch (gc->state) {
        case GrabState::Passive:
            gc->state = GrabState::Aggressive;
            if (se) se->apply(StatusEffectType::Slowdown, -1.f, 0.4f); // semi-permanent
            SDL_Log("[grab] %u upgraded grab → Aggressive on %u", grabber, grabbed);
            return true;

        case GrabState::Aggressive:
            gc->state = GrabState::Neck;
            if (se) {
                se->apply(StatusEffectType::Slowdown,  -1.f, 0.7f);   // stronger slow
                se->apply(StatusEffectType::Silence,   -1.f, 1.f);    // can't speak
            }
            SDL_Log("[grab] %u upgraded grab → Neck on %u", grabber, grabbed);
            return true;

        case GrabState::Neck:
            gc->state = GrabState::Kill;
            if (se) {
                // Brief stun from the slam
                se->apply(StatusEffectType::Stun, 1.5f, 1.f);
            }
            SDL_Log("[grab] %u upgraded grab → Kill on %u", grabber, grabbed);
            return true;

        default:
            SDL_Log("[grab] %u grab on %u already at max state", grabber, grabbed);
            return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  release_grab
// ─────────────────────────────────────────────────────────────────────────────
void release_grab(EntityID grabber, EntityID grabbed,
                  EntityManager& entities,
                  SignalBus&     /*signals*/)
{
    if (grabber == NULL_ENTITY || grabbed == NULL_ENTITY) return;
    auto* gc = entities.get_component<GrabbedComponent>(grabbed);
    if (!gc || gc->grabber != grabber) return;

    // Remove grab-induced status effects (Slowdown + Silence added by upgrade)
    auto* se = entities.get_component<StatusEffectsComponent>(grabbed);
    if (se) {
        se->remove(StatusEffectType::Slowdown);
        se->remove(StatusEffectType::Silence);
    }

    // Destroy the GrabbedComponent
    entities.remove_component<GrabbedComponent>(grabbed);
    SDL_Log("[grab] %u released grab on %u", grabber, grabbed);
}
