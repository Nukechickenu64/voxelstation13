#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  VerbDispatch — named handler registry for item/voxel/mob verbs
// ─────────────────────────────────────────────────────────────────────────────
//
//  Inspired by TG SS13 / BYOND's proc-call dispatch and GZDoom ZScript's
//  virtual-function/action-function mechanism.
//
//  Every verb (interactive action) is identified by a handler name string
//  stored in JSON — e.g. ItemVerb::handler == "verb_pry_floor".
//  VerbDispatch maps those names to callable std::functions at runtime.
//
//  Two registration patterns:
//
//  1. Built-in handlers (registered once in init_verb_dispatch()):
//       verb_examine   — print item description to HUD
//       verb_throw     — throw item in aimed direction
//       verb_open      — open/close a container item
//       verb_stun      — stun a nearby mob
//       verb_show_id   — show ID card information
//       verb_open_pda  — open PDA interface
//
//  2. Game-logic handlers (registered in main() with lambda captures):
//       These capture simulation state (world, atmos, physics, etc.) by
//       reference from the enclosing scope, allowing them to drive
//       full-system interactions without requiring VerbContext to carry
//       heavy dependencies.
//
//  Prototype inheritance:
//       Items can declare  "parent": "some_item_id"  in their JSON.
//       ItemRegistry::resolve_prototypes() then copies unset fields
//       (icon, weight, verbs, tags, …) from the parent ItemDef.
//       Verb lists are merged: parent verbs first, child verbs override
//       any verb with the same display name.
//
// ─────────────────────────────────────────────────────────────────────────────

#include "core/types.h"
#include "inventory/inventory.h"
#include <functional>
#include <string>
#include <deque>
#include <unordered_map>
#include <glm/glm.hpp>

// ── VerbContext ───────────────────────────────────────────────────────────────
// Passed to every invoked verb handler.  Contains the minimum state needed by
// common verbs; complex verbs (registered via lambda in main.cpp) capture
// additional simulation state from their enclosing scope.
struct VerbContext {
    // ── Who is acting ─────────────────────────────────────────────────────────
    EntityID         actor     = NULL_ENTITY;    // entity performing the action
    glm::vec3        actor_pos {};               // actor's world position (eye pos)
    glm::vec3        actor_fwd {};               // unit view direction

    // ── What is being acted on ────────────────────────────────────────────────
    EntityID         target_ent = NULL_ENTITY;    // world entity (item, mob, etc.)
    const ItemDef*   item_def   = nullptr;        // def of the item in context
    ItemStack*       item_stack = nullptr;        // pointer to the item stack

    // Inventory slot that owns the item (empty if the item is a world entity).
    // Set this when the verb is triggered from the inventory panel so handlers
    // such as verb_open can call Inventory::open_container(slot_id).
    std::string      slot_id;

    // Voxel target — set when the verb was triggered by RMB on a voxel face.
    glm::ivec3       target_voxel    {};
    bool             has_target_voxel = false;

    // ── HUD feedback ──────────────────────────────────────────────────────────
    // Optional deque shared with the HUD radio log.  Call log() to print a
    // message to the player's HUD without coupling to the UI system directly.
    std::deque<std::string>* hud_log = nullptr;

    // Appends msg to the HUD log (silently no-ops when hud_log == nullptr).
    void log(const std::string& msg) const {
        if (!hud_log) return;
        if (hud_log->size() >= 30) hud_log->pop_front();
        hud_log->push_back(msg);
    }
};

// The handler function signature.
using VerbFn = std::function<void(const VerbContext&)>;

// ── VerbDispatch ──────────────────────────────────────────────────────────────
// Singleton registry of named verb handlers.
// Thread-safety: not thread-safe — all access must be from the main thread.
class VerbDispatch {
public:
    // Register (or overwrite) a handler function for the given handler name.
    // Calling this with the same name a second time replaces the earlier handler,
    // which is the intended override mechanism (main.cpp overrides init stubs).
    void register_handler(const std::string& name, VerbFn fn);

    // Invoke the handler for 'handler_name' with the supplied context.
    // Returns true on success, false if no handler was registered (and logs a
    // SDL_Log warning so TODO verbs are easy to spot during development).
    bool invoke(const std::string& handler_name, const VerbContext& ctx) const;

    // True if a handler is registered for this name.
    bool has(const std::string& handler_name) const;

    // Iterate all registered names (useful for debugging).
    const std::unordered_map<std::string, VerbFn>& all() const { return m_handlers; }

    // Global singleton accessor.
    static VerbDispatch& get();

private:
    std::unordered_map<std::string, VerbFn> m_handlers;
};

// ── Free-function accessors ───────────────────────────────────────────────────

// Returns the global VerbDispatch singleton.
VerbDispatch& verb_dispatch();

// Registers built-in handlers (verb_examine, verb_open, etc.).
// Call once at startup, before ItemRegistry::load_directory().
// Handlers registered here use only the fields already in VerbContext;
// main.cpp then overwrites those that need broader game-system access.
void init_verb_dispatch();
