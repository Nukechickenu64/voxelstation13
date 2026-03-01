#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  Status effects — mirrors TG SS13's status_effects/ datum hierarchy.
//
//  Each status effect type is an enum value.  An entity with
//  StatusEffectsComponent can have multiple simultaneous active effects:
//
//    • Apply:   comp.apply(StatusEffectType::Stun, 2.f);
//    • Check:   comp.has(StatusEffectType::Stun)
//    • Remove:  comp.remove(StatusEffectType::Stun)
//    • Tick:    comp.tick(dt) → returns updated MobState for CharacterController
//
//  TG analogues
//  ─────────────────────────────────────────────────────────────────────────
//  Stun      → /datum/status_effect/grouped/stun
//  Knockdown → /datum/status_effect/grouped/knockdown
//  Paralysis → /datum/status_effect/grouped/paralysis
//  Slowdown  → /datum/status_effect/stacked/slowdown
//  etc.
// ─────────────────────────────────────────────────────────────────────────────

#include "core/types.h"
#include "simulation/physics.h"   // MobState
#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

// ── Status effect type enum ───────────────────────────────────────────────────
enum class StatusEffectType : uint8_t {
    // Incapacitation (priority order for MobState)
    Stun       = 0,   // cannot act, moves to Hardcrit; duration drains each tick
    Paralysis  = 1,   // catatonic; like Stun but harder to cure (reagents needed)
    Knockdown  = 2,   // lying prone, moves to Softcrit (crawl allowed)

    // Movement modifiers
    Slowdown   = 3,   // reduces move speed by strength (0..1)
    Drowsy     = 4,   // mild slowdown; prolonged exposure causes Knockdown

    // Sensory
    Blind      = 5,   // signals the renderer to apply a vision blackout
    Deaf       = 6,   // suppresses audio events
    Confusion  = 7,   // random directional drift added to wish_move

    // Social / verbal
    Silence    = 8,   // cannot speak/radio (future text/audio system)

    // Visual / cosmetic
    Jitter     = 9,   // camera micro-shake in FirstPersonCamera
    Dizzy      = 10,  // screen tilt overlay

    COUNT
};

// String names for signals and logging
std::string_view status_effect_name(StatusEffectType t);

// ── One active instance of a status effect ────────────────────────────────────
struct ActiveStatusEffect {
    StatusEffectType type;
    float            duration;    // seconds remaining; < 0 = permanent
    float            strength;    // 0..1 or 0..N depending on effect type
};

// ── StatusEffectsComponent ────────────────────────────────────────────────────
// Attach to any entity that can receive status effects (mobs, mainly).
struct StatusEffectsComponent {
    std::vector<ActiveStatusEffect> effects;

    // ── Query ────────────────────────────────────────────────────────────────
    bool has(StatusEffectType t) const;
    const ActiveStatusEffect* get(StatusEffectType t) const;

    // ── Modification ─────────────────────────────────────────────────────────
    // Apply or refresh a status effect.
    // If the effect is already active:
    //   • duration is set to max(existing, new)   (TG "grouped" behaviour)
    //   • strength is set to max(existing, new)
    void apply(StatusEffectType t, float duration, float strength = 1.f);

    // Forcibly remove a status effect.
    void remove(StatusEffectType t);

    // Remove all effects.
    void clear();

    // ── Tick ─────────────────────────────────────────────────────────────────
    // Reduce durations by dt; remove expired effects.
    // Returns the most severe MobState dictated by remaining effects.
    //   Stun / Paralysis → Hardcrit
    //   Knockdown        → Softcrit
    //   (none)           → Normal
    // Callers should AND this with their own logic (e.g. health-based crit).
    MobState tick(double dt);

    // Returns the MobState implied by current effects WITHOUT advancing time.
    // Use this in pre-physics passes that need to read state without side effects.
    MobState current_mob_state() const;

    // ── Convenience accessors ─────────────────────────────────────────────────
    bool  is_stunned()      const { return has(StatusEffectType::Stun)
                                       || has(StatusEffectType::Paralysis); }
    bool  is_knocked_down() const { return has(StatusEffectType::Knockdown); }
    bool  is_blinded()      const { return has(StatusEffectType::Blind); }
    bool  is_confused()     const { return has(StatusEffectType::Confusion); }
    bool  is_silenced()     const { return has(StatusEffectType::Silence); }
    bool  is_jittering()    const { return has(StatusEffectType::Jitter); }
    bool  is_dizzy()        const { return has(StatusEffectType::Dizzy); }

    // Cumulative speed multiplier from Slowdown + Drowsy (0 = fully stopped).
    float speed_multiplier() const;
};
