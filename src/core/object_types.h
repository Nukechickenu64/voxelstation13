#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
//  Type-path prototype system  (inspired by TG-Station13's DM type hierarchy)
// ─────────────────────────────────────────────────────────────────────────────
//
//  Every game object has a TYPE PATH — a slash-delimited string that describes
//  its place in the inheritance hierarchy, e.g.
//
//      /obj/item/tool/wrench
//      /obj/item/clothing/suit/space
//      /mob/living/carbon/human
//      /obj/structure/table
//      /obj/machinery/power/smes
//      /turf/floor/plating
//
//  The hierarchy for this game mirrors TG SS13's broad structure:
//
//    /atom                    ← base class for everything placed in the world
//    /atom/movable            ← things that can be moved
//    /obj                     ← inanimate objects
//    /obj/item                ← items that can be picked up
//    /obj/item/tool           ← hand tools (wrench, screwdriver, …)
//    /obj/item/clothing       ← wearable items
//    /obj/item/clothing/head  ← head-slot clothing
//    /obj/item/clothing/suit  ← suit-slot clothing
//    /obj/item/clothing/gloves
//    /obj/item/clothing/shoes
//    /obj/item/clothing/mask
//    /obj/item/storage        ← container/bag items
//    /obj/item/stack          ← stackable consumable items
//    /obj/item/medical        ← bandages, syringes, etc.
//    /obj/item/weapon         ← hand weapons
//    /obj/structure           ← non-carryable structures (tables, walls, …)
//    /obj/machinery           ← powered machines
//    /obj/machinery/power     ← power infrastructure
//    /mob                     ← living / animated entities
//    /mob/living              ← mortal entities
//    /mob/living/carbon       ← carbon-based life
//    /mob/living/carbon/human ← playable human
//    /turf                    ← voxel-face tiles
//    /area                    ← gameplay regions
//
// ─────────────────────────────────────────────────────────────────────────────

// ── Path utilities ────────────────────────────────────────────────────────────

// Returns the parent path.
// parent_type("/obj/item/tool/wrench") → "/obj/item/tool"
// parent_type("/obj")                 → "/atom/movable"   (canonical parent)
// parent_type("/atom")                → ""  (no parent)
std::string parent_type(std::string_view path);

// Returns true if `derived` is the same as or a subtype of `ancestor`.
// Mirrors DM's istype() semantics.
// istype("/obj/item/tool/wrench", "/obj/item") → true
// istype("/mob/living",          "/obj")       → false
bool istype(std::string_view derived, std::string_view ancestor);

// Returns the canonical ancestry chain from most-derived to root.
// ancestry("/obj/item/tool/wrench") →
//   { "/obj/item/tool/wrench", "/obj/item/tool", "/obj/item", "/obj",
//     "/atom/movable", "/atom" }
std::vector<std::string> type_ancestry(std::string_view path);

// Returns the last segment of the path (the "leaf" type name).
// type_short("/obj/item/tool/wrench") → "wrench"
std::string_view type_short(std::string_view path);

// ── Canonical parent overrides ────────────────────────────────────────────────
// Most types just strip the last segment for their parent, but a few top-level
// classes have non-obvious parents.  This table stores those overrides.
// Populated once at program start — call register_type_parent() at init time.
void register_type_parent(std::string_view type_path, std::string_view parent_path);

// ── Prototype default record ──────────────────────────────────────────────────
// A prototype carries inheritable defaults for a type.  Child types that don't
// explicitly define a field inherit the nearest ancestor that does.
//
// Currently used by ItemRegistry to fill in weight / volume / tags etc. when a
// specific item def omits them.
struct PrototypeDef {
    std::string              type_path;
    std::string              display_name;   // optional display name
    float                    weight      = -1.f;  // < 0 = unset
    float                    volume      = -1.f;
    int                      stack_max   = -1;
    std::vector<std::string> tags;       // merged with child (not replaced)
};

// ── Prototype registry ────────────────────────────────────────────────────────
// A lightweight lookup table for PrototypeDef records.
// ItemRegistry populates this when it loads parent-only defs.
class PrototypeRegistry {
public:
    // Register a prototype. Returns false if a prototype with the same
    // type_path was already registered (and does NOT overwrite).
    bool register_prototype(PrototypeDef def);

    // Find the prototype for exactly this type_path.
    const PrototypeDef* get(std::string_view type_path) const;

    // Walk the ancestry chain and return the nearest ancestor prototype that
    // has weight set (i.e. weight >= 0).  Returns nullptr if none found.
    const PrototypeDef* resolve_weight  (std::string_view type_path) const;
    const PrototypeDef* resolve_volume  (std::string_view type_path) const;
    const PrototypeDef* resolve_stack_max(std::string_view type_path) const;

    // Collect all tags from the entire ancestry chain (nearest-first, deduped).
    std::vector<std::string> resolve_tags(std::string_view type_path) const;

    const std::unordered_map<std::string, PrototypeDef>& all() const { return m_defs; }

private:
    std::unordered_map<std::string, PrototypeDef> m_defs;
};

// ── Global prototype registry singleton ──────────────────────────────────────
// Call init_prototype_registry() once at startup.
PrototypeRegistry& prototype_registry();
void               init_prototype_registry();
