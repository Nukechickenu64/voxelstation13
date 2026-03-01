#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  Reagent / chemistry system — mirrors TG SS13 /datum/reagents + /datum/reagent
//
//  Key concepts:
//    • ReagentDef       — static definition (metabolise rate, mob effects)
//    • ReagentContainer — mutable list of { id, volume } pairs
//    • ReagentContainerComponent — ECS component wrapping a ReagentContainer
//    • ReagentRegistry  — singleton that holds all ReagentDef records
//    • metabolise_reagents() — per-tick update for a mob's internal chemistry
//
//  TG analogues
//  ─────────────────────────────────────────────────────────────────────────────
//  /datum/reagents           → ReagentContainerComponent
//  /datum/reagent            → ReagentDef   (static)  +  Reagent   (instance)
//  reagents.add_reagent()    → ReagentContainer::add()
//  reagents.remove_reagent() → ReagentContainer::remove()
//  reagents.reaction_fire()  → (reactions not yet implemented — skeleton)
//  reagents.metabolize()     → metabolise_reagents()
// ─────────────────────────────────────────────────────────────────────────────

#include "core/types.h"
#include "core/entity_manager.h"
#include "core/signals.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "simulation/pipes.h"  // shared struct Reagent { std::string id; float volume; }

// ── Static reagent definition (loaded at startup) ─────────────────────────────
struct ReagentDef {
    std::string id;
    std::string name;

    // Metabolisation
    float metabolise_rate    = 0.2f;  // units removed from bloodstream per tick
    float overdose_threshold = 0.f;   // cumulative volume for overdose; 0 = none

    // Mob health effects when metabolised (per tick, per unit present)
    float oxy_damage   = 0.f;
    float tox_damage   = 0.f;
    float brute_damage = 0.f;
    float burn_damage  = 0.f;

    // Healing effects (negative damage)
    float heal_brute   = 0.f;
    float heal_burn    = 0.f;
    float heal_oxy     = 0.f;
    float heal_tox     = 0.f;

    // Status effects applied per tick while present
    float stun_time     = 0.f;  // seconds of Stun to apply
    float knockdown_time= 0.f;  // seconds of Knockdown to apply
    float drowsy_time   = 0.f;  // seconds of Drowsy to apply
    float slowdown      = 0.f;  // Slowdown strength (0-1)
    float jitter_time   = 0.f;  // seconds of Jitter to apply
    float dizzy_time    = 0.f;  // seconds of Dizzy to apply
};

// ── ReagentContainer ─────────────────────────────────────────────────────────
class ReagentContainer {
public:
    explicit ReagentContainer(float max_vol = 100.f) : m_max_volume(max_vol) {}

    float max_volume()   const { return m_max_volume; }
    float total_volume() const;
    float free_space()   const { return m_max_volume - total_volume(); }

    // Add `amount` units of reagent `id`.
    // Clamps to free_space(). Returns actual amount added.
    float add(std::string_view id, float amount);

    // Remove `amount` units of reagent `id`.
    // Clamps to available. Returns actual amount removed.
    float remove(std::string_view id, float amount);

    // Remove all reagents.
    void clear();

    // Transfer up to `amount` of `id` from this container to `dst`.
    // Returns amount actually transferred.
    float transfer_to(std::string_view id, float amount, ReagentContainer& dst);

    // Splash: transfer `amount` total volume (split proportionally) to `dst`.
    void splash_to(float amount, ReagentContainer& dst);

    // Find a slot (null if absent)
    const Reagent* find(std::string_view id) const;
    Reagent*       find(std::string_view id);

    const std::vector<Reagent>& reagents() const { return m_reagents; }
    std::vector<Reagent>&       reagents()       { return m_reagents; }

private:
    float               m_max_volume;
    std::vector<Reagent> m_reagents;
};

// ── ECS component ─────────────────────────────────────────────────────────────
// Attach to mobs (bloodstream), beakers, syringes, pills, food, etc.
struct ReagentContainerComponent {
    ReagentContainer container;
    bool is_open = true;  // false → cannot be injected without opening (e.g. sealed pill)
    // Cumulative overdose accumulators: id → total volume ingested
    std::unordered_map<std::string, float> overdose_levels;
};

// ── ReagentRegistry ───────────────────────────────────────────────────────────
// Singleton holding all known reagent definitions.
class ReagentRegistry {
public:
    // Register a definition.  Returns false if already registered (no overwrite).
    bool register_def(ReagentDef def);

    // Find by id. Returns nullptr if not found.
    const ReagentDef* find(std::string_view id) const;

    // All registered definitions (read-only).
    const std::unordered_map<std::string, ReagentDef>& all() const { return m_defs; }

    // Singleton access.
    static ReagentRegistry& get();

    // Populate the built-in reagent set (called by init_reagents()).
    static void init_defaults();

private:
    std::unordered_map<std::string, ReagentDef> m_defs;
};

// Initialise the global registry and built-in reagents.
void init_reagents();

// ── Metabolisation tick ───────────────────────────────────────────────────────
// Call once per server tick for every mob entity that has both
// ReagentContainerComponent and HealthComponent.
//
// For each reagent present:
//   1. Apply damage / healing to HealthComponent.
//   2. Apply status effects to StatusEffectsComponent (if present).
//   3. Drain metabolise_rate units.
//   4. Check overdose threshold; fire COMSIG_REAGENT_OVERDOSE if exceeded.
void metabolise_reagents(EntityID       mob,
                         EntityManager& entities,
                         SignalBus&     signals,
                         double         dt);
