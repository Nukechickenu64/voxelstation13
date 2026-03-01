#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  Master Controller — mirrors TG SS13's controller/subsystem/MC architecture.
//
//  The MC owns named "processing lists" (subsystems).  Entities subscribe to
//  one or more lists; each tick the list fires their process callback.
//
//  TG analogue:
//    START_PROCESSING(SSmobs,    src)    → mc.start("mobs",    id, cb)
//    START_PROCESSING(SSobjects, src)    → mc.start("objects", id, cb)
//    STOP_PROCESSING(SSmobs,     src)    → mc.stop("mobs",     id)
//
//  Subsystem tick order matches TG (configured in MasterController::tick):
//    1. "living"   – health updates, oxygen ticks, status-effect decay
//    2. "mobs"     – mob AI, player-controlled entities
//    3. "objects"  – machinery, doors, etc.
//    4. "items"    – floating-item physics helpers (settled by WorldItemSystem)
//
//  Each registered callback receives (EntityID, double dt).
//  The callback is responsible for any simulation it needs.
// ─────────────────────────────────────────────────────────────────────────────

#include "core/types.h"
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ── Process callback type ─────────────────────────────────────────────────────
using ProcessFn = std::function<void(EntityID, double /*dt*/)>;

// ── ProcessEntry ──────────────────────────────────────────────────────────────
struct ProcessEntry {
    EntityID  id = NULL_ENTITY;
    ProcessFn fn;
};

// ── ProcessingList (one subsystem) ───────────────────────────────────────────
class ProcessingList {
public:
    // Start processing entity id with callback fn.
    // If id is already registered the callback is replaced.
    void start(EntityID id, ProcessFn fn);

    // Stop processing entity id.
    void stop(EntityID id);

    // True if entity id is currently in this list.
    bool is_running(EntityID id) const;

    // Fire all callbacks with the given dt.
    void tick(double dt);

    // Remove an entity from the list (alias for stop — used by MC on entity death).
    void purge(EntityID id) { stop(id); }

private:
    std::vector<ProcessEntry>         m_entries;
    std::unordered_map<EntityID, int> m_idx;   // entity → index in m_entries

    // Deferred additions/removals accumulated during tick
    std::vector<ProcessEntry> m_pending_add;
    std::vector<EntityID>     m_pending_remove;
    bool m_ticking = false;

    void flush_pending();
};

// ── MasterController ──────────────────────────────────────────────────────────
class MasterController {
public:
    // ── Well-known subsystem names (match TG's subsystem names) ──────────────
    static constexpr const char* SS_LIVING  = "living";
    static constexpr const char* SS_MOBS    = "mobs";
    static constexpr const char* SS_OBJECTS = "objects";
    static constexpr const char* SS_ITEMS   = "items";

    // Register entity in a named list.
    void start_processing(std::string_view list, EntityID id, ProcessFn fn);

    // Deregister entity from a named list.
    void stop_processing(std::string_view list, EntityID id);

    // True if entity is in the given list.
    bool is_processing(std::string_view list, EntityID id) const;

    // Tick ALL lists in the canonical TG order.
    void tick(double dt);

    // Tick only one named list.
    void tick_list(std::string_view list, double dt);

    // Remove entity from every list (call when entity is destroyed).
    void purge(EntityID id);

    // Access a named list (creates it if needed).
    ProcessingList&       list(std::string_view name);
    const ProcessingList* find_list(std::string_view name) const;

    // Global singleton — initialised via init_master_controller().
    static MasterController& get();

private:
    std::unordered_map<std::string, ProcessingList> m_lists;
};

// ── Initialise the global singleton ──────────────────────────────────────────
void init_master_controller();
MasterController& master_controller();

// ── Convenience macros (mirror TG DM macros) ─────────────────────────────────
#define START_PROCESSING(list_name, id, fn) \
    master_controller().start_processing((list_name), (id), (fn))

#define STOP_PROCESSING(list_name, id) \
    master_controller().stop_processing((list_name), (id))
