#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  Signal bus — mirrors TG SS13's COMSIG / REGISTER_SIGNAL / SEND_SIGNAL system
//
//  Every entity can have signal handlers registered against it.
//  When a SEND_SIGNAL fires, all registered handlers for that (entity, signal)
//  pair are invoked in registration order.
//
//  Handler return value:
//    0                       → continue (do not cancel)
//    COMPONENT_SIGNAL_CANCEL → stop further handlers and block default behaviour
//  The bitmask of all returned values is the final result of send_signal().
//
//  Example:
//    auto h = signals.register_signal(entity, COMSIG_MOB_DEATH,
//        [](EntityID src, const SignalArgs& a) -> SignalResult {
//            SDL_Log("Entity %u died", src);
//            return 0; // don't cancel
//        });
//    signals.send_signal(entity, COMSIG_MOB_DEATH);
//    signals.unregister_signal(h);
// ─────────────────────────────────────────────────────────────────────────────

#include "core/types.h"
#include <any>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ── Signal result ─────────────────────────────────────────────────────────────
using SignalResult = int;
constexpr SignalResult COMPONENT_SIGNAL_CANCEL = 1;

// ── Type-erased argument bundle ───────────────────────────────────────────────
// Pass any struct as: SignalArgs{ MyArgs{...} }
// Retrieve with:      *std::any_cast<MyArgs>(&args)
using SignalArgs = std::any;

// ── Handler function type ─────────────────────────────────────────────────────
using SignalHandler = std::function<SignalResult(EntityID source, const SignalArgs& args)>;

// ── Handle returned by register_signal ───────────────────────────────────────
using SignalHandle = uint64_t;
constexpr SignalHandle NULL_SIGNAL_HANDLE = 0;

// ─────────────────────────────────────────────────────────────────────────────
//  SignalBus
// ─────────────────────────────────────────────────────────────────────────────
class SignalBus {
public:
    // Register a handler for (entity, signal). Returns an opaque handle
    // that must be stored and passed to unregister_signal when done.
    SignalHandle register_signal(EntityID entity, std::string_view signal,
                                 SignalHandler fn);

    // Unregister a previously registered handler.
    void unregister_signal(SignalHandle handle);

    // Fire all handlers for (entity, signal).
    // Returns the bitwise-OR of all handler return values.
    SignalResult send_signal(EntityID entity, std::string_view signal,
                             SignalArgs args = {}) const;

    // Remove every handler associated with entity (call when entity is destroyed).
    void purge_entity(EntityID entity);

private:
    struct Registration {
        EntityID    entity;
        std::string signal;
        SignalHandler fn;
    };

    SignalHandle m_next_handle = 1;
    std::unordered_map<SignalHandle, Registration> m_by_handle;

    // Inverted index: entity → signal_name → list of handles (for fast dispatch)
    std::unordered_map<EntityID,
        std::unordered_map<std::string, std::vector<SignalHandle>>> m_index;
};

// ── Convenience macros (mirror TG DM macros) ──────────────────────────────────
#define REGISTER_SIGNAL(bus, entity, signal, fn) \
    (bus).register_signal((entity), (signal), (fn))

#define UNREGISTER_SIGNAL(bus, handle) \
    (bus).unregister_signal(handle)

#define SEND_SIGNAL(bus, entity, signal, ...) \
    (bus).send_signal((entity), (signal), ##__VA_ARGS__)

// ── Common signal name constants (mirrors TG's comsig defines) ────────────────
// Atom / generic
inline constexpr const char* COMSIG_ATOM_CLICK          = "atom_click";
inline constexpr const char* COMSIG_ATOM_ATTACKED        = "atom_attacked";
inline constexpr const char* COMSIG_ATOM_ATTACK_HAND     = "atom_attack_hand";
inline constexpr const char* COMSIG_ATOM_ATTACK_ITEM     = "atom_attack_item";
inline constexpr const char* COMSIG_ATOM_BUMPED          = "atom_bumped";
inline constexpr const char* COMSIG_ATOM_EXAMINE         = "atom_examine";
inline constexpr const char* COMSIG_ATOM_ENTERED         = "atom_entered";
inline constexpr const char* COMSIG_ATOM_EXITED          = "atom_exited";
inline constexpr const char* COMSIG_ATOM_DESTROYABLE     = "atom_destroyable";

// Mob / living
inline constexpr const char* COMSIG_MOB_DEATH            = "mob_death";
inline constexpr const char* COMSIG_MOB_REVIVE           = "mob_revive";
inline constexpr const char* COMSIG_MOB_STAT_SET         = "mob_stat_set";
inline constexpr const char* COMSIG_MOB_HEALTHUPDATE     = "mob_health_update";
inline constexpr const char* COMSIG_MOB_APPLY_DAMAGE     = "mob_apply_damage";
inline constexpr const char* COMSIG_MOB_LIVING_DEATH     = "mob_living_death";
inline constexpr const char* COMSIG_MOB_LIVING_REVIVE    = "mob_living_revive";
inline constexpr const char* COMSIG_MOB_LIVING_STATUS    = "mob_living_status_update";

// Item
inline constexpr const char* COMSIG_ITEM_ATTACK          = "item_attack";
inline constexpr const char* COMSIG_ITEM_ATTACK_SELF     = "item_attack_self";
inline constexpr const char* COMSIG_ITEM_DROPPED         = "item_dropped";
inline constexpr const char* COMSIG_ITEM_PICKUP          = "item_pickup";
inline constexpr const char* COMSIG_ITEM_EQUIPPED        = "item_equipped";
inline constexpr const char* COMSIG_ITEM_UNEQUIPPED      = "item_unequipped";

// Status effects
inline constexpr const char* COMSIG_STATUS_APPLIED       = "status_effect_applied";
inline constexpr const char* COMSIG_STATUS_REMOVED       = "status_effect_removed";

// Reagents
inline constexpr const char* COMSIG_REAGENT_ADDED        = "reagent_added";
inline constexpr const char* COMSIG_REAGENT_REMOVED      = "reagent_removed";
inline constexpr const char* COMSIG_REAGENT_OVERDOSE     = "reagent_overdose";

// ── Signal arg structs ────────────────────────────────────────────────────────
// Callers std::any_cast<> these from SignalArgs inside handlers.

struct SigAttacked {
    EntityID       attacker    = NULL_ENTITY;
    float          damage      = 0.f;
    std::string    damage_type;   // "brute", "burn", "tox", "oxy"
    bool           used_weapon = false;
};

struct SigAttackHand {
    EntityID attacker = NULL_ENTITY;
};

struct SigBumped {
    EntityID bumper = NULL_ENTITY;
    bool     has_weapon = false;
};

struct SigDeath {
    EntityID killed_by = NULL_ENTITY;
};

struct SigStatusEffect {
    std::string type;      // e.g. "stun", "knockdown"
    float       duration;
    float       strength;
};

struct SigReagentChanged {
    std::string reagent_id;
    float       amount;
};

struct SigItemPickup {
    EntityID picker    = NULL_ENTITY;
    std::string slot_id;
};

struct SigItemDrop {
    EntityID dropper = NULL_ENTITY;
};

// Global (singleton-style) signal bus — owned by the Server.
// Accessed via signals() free function once initialised.
SignalBus& signals();
void       init_signals();
