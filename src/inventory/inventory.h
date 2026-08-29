#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <memory>
#include <cstdint>

// ── Item ──────────────────────────────────────────────────────────────────────
struct ItemVerb {
    std::string name;
    float       range    = 1.5f;
    std::string handler; // name of C++ handler or Lua function
};

struct ItemDef {
    // ── Type-path prototype system (TG SS13-style) ───────────────────────────
    // Full type path in the object hierarchy, e.g. "/obj/item/tool/wrench".
    // Used by istype() for classification checks.
    // Defaults to "/obj/item/<id>" when not specified in JSON.
    std::string              type_path;

    // Pointer to the parent ItemDef that acts as this type's prototype.
    // Resolved by ItemRegistry after all defs are loaded.
    // nullptr if no parent ItemDef exists (only a PrototypeDef may exist).
    const ItemDef*           prototype_parent = nullptr;

    // ── BYOND-style prototype inheritance ────────────────────────────────────
    // Set from JSON "parent": "some_item_id".
    // ItemRegistry::resolve_prototypes() walks the chain and copies fields that
    // were NOT explicitly set in this def's JSON from its nearest ancestor that
    // has them.  Verb lists are merged (parent verbs first; child overrides by
    // display name).  Tags are additively merged (never replaced).
    std::string              parent_id;

    // Bitmask tracking which fields were explicitly present in the source JSON.
    // Any field whose bit is NOT set here may be inherited from parent_id.
    uint32_t                 explicit_json_fields = 0;
    static constexpr uint32_t EJF_ICON                   = 1u <<  0;
    static constexpr uint32_t EJF_WEIGHT                 = 1u <<  1;
    static constexpr uint32_t EJF_VOLUME                 = 1u <<  2;
    static constexpr uint32_t EJF_STACK_MAX              = 1u <<  3;
    static constexpr uint32_t EJF_IS_CONTAINER           = 1u <<  4;
    static constexpr uint32_t EJF_CONTAINER_VOLUME       = 1u <<  5;
    static constexpr uint32_t EJF_EQUIP_SLOT             = 1u <<  6;
    static constexpr uint32_t EJF_TWO_HANDED             = 1u <<  7;
    static constexpr uint32_t EJF_PLACES_VOXEL           = 1u <<  8;
    static constexpr uint32_t EJF_TAGS                   = 1u <<  9;
    static constexpr uint32_t EJF_VERBS                  = 1u << 10;
    static constexpr uint32_t EJF_CONTAINER_ACCEPTS_TAGS = 1u << 11;
    static constexpr uint32_t EJF_TYPE_PATH              = 1u << 12;

    std::string              id;
    std::string              name;
    std::string              icon;       // path in textures/items/
    std::vector<std::string> tags;
    float                    weight           = 0.1f;   // kg
    float                    volume           = 0.5f;   // litres (Stationeers-style)
    int                      stack_max        = 1;
    bool                     is_container     = false;
    float                    container_volume = 0.f;    // litres capacity when container
    // Tags that items must match to be placed inside this container (empty = any)
    std::vector<std::string> container_accepts_tags;
    // Which equipment slot this auto-equips to ("" = hand/general)
    std::string              equip_slot;
    // Requires both hand slots to be free when equipping to a hand
    bool                     two_handed       = false;
    // Voxel type ID to place when this tile item is used on plating ("" = none)
    std::string              places_voxel;
    std::vector<ItemVerb>    verbs;
};

struct ItemStack {
    const ItemDef* def    = nullptr;
    int            count  = 0;
    // Per-instance data (damage, custom name, etc.)
    std::string    custom_name;
    float          integrity = 1.f;  // 0-1

    // Gun ammo tracking for energy weapons.
    // -1 = uninitialized (lazy-init from energy_max tag on first fire).
    //  0 = empty / not an energy gun.
    // >0 = charges remaining.
    int            ammo_remaining = -1;

    // Hidden magazine slot for ballistic guns (max 1 element).
    // Loaded via verb_reload; ejected via verb_eject_mag.
    // Not exposed through the normal container UI.
    std::vector<ItemStack> magazine_slot;

    // Container state — only meaningful when def->is_container == true
    bool                   container_open = false;
    std::vector<ItemStack> contents;       // items stored inside this container
    // Lock state — used by secure_briefcase and similar items
    bool                   locked    = false;
    std::string            lock_code;        // empty = no code set
};

// Returns a human-readable description of an item's condition based on integrity.
// integrity is clamped to [0, 1]: 1.0 = pristine, 0.0 = destroyed.
const char* condition_label(float integrity);

// ── Type-path helpers for ItemDef ─────────────────────────────────────────────
// Convenience wrappers around the global istype() from object_types.h.
// istype(def, "/obj/item/tool")     → true for wrench, screwdriver, etc.
// istype(stack, "/obj/item/medical") → true for bandages, syringes, etc.
bool istype(const ItemDef&   def,   std::string_view ancestor_path);
bool istype(const ItemStack& stack, std::string_view ancestor_path);

// ── Inventory slot ────────────────────────────────────────────────────────────
enum class SlotCategory {
    General,    // hands, pockets — accept anything small
    Equipment,  // typed equipment slots (head, suit, etc.)
    BeltTool,   // sub-slots on a worn toolbelt
    Container,  // virtual sub-slots inside an open bag
};

struct InventorySlot {
    std::string              id;
    std::string              display_name;  // shown in UI (e.g. "Head")
    std::vector<std::string> accepts_tags;  // "*" = anything
    SlotCategory             category  = SlotCategory::General;
    std::optional<ItemStack> item;

    // Volume limit when this slot acts as a container (0 = unlimited)
    float   volume_capacity = 0.f;

    // For container items: child slots are stored here when opened
    std::vector<InventorySlot> children;
    bool                       open = false;

    bool accepts(const ItemDef& def) const;
};

// ── Inventory ─────────────────────────────────────────────────────────────────
class Inventory {
public:
    Inventory() = default;  // empty (no slots) — use factory functions for real inventories
    explicit Inventory(std::vector<InventorySlot> slots);

    InventorySlot*       find_slot(const std::string& id);
    const InventorySlot* find_slot(const std::string& id) const;

    // Recursively search slots and all open container children
    InventorySlot*       find_slot_deep(const std::string& id);

    // Try to place a stack into a specific slot; returns false on failure
    bool put(const std::string& slot_id, ItemStack stack);

    // Remove item from slot (returns it so caller can drop it)
    std::optional<ItemStack> take(const std::string& slot_id);

    // Swap two slots' contents (works across top-level slots)
    void swap(const std::string& a, const std::string& b);

    // Split count items from src into target slot; returns false on failure
    bool split(const std::string& src_slot, const std::string& dst_slot, int count);

    // Returns first slot that accepts and is empty, else nullptr
    InventorySlot* find_empty_accepting(const ItemDef& def);

    // Auto-equip: try equip_slot first, then find_empty_accepting
    // Tries to place `stack` into the best available slot.
    // Returns nullopt on success; returns the item back on failure (nothing was modified).
    std::optional<ItemStack> auto_equip(ItemStack stack);

    // Container management: open/close a container item in the given slot.
    // open_container generates child slots from the item's contents list and
    // sets slot->open = true.  Returns false if the slot has no container item.
    bool open_container (const std::string& slot_id);
    void close_container(const std::string& slot_id); // syncs children → contents

    // Returns the first open container slot (used by the UI panel)
    InventorySlot* first_open_container();

    // Aggregate stats
    float total_weight()  const; // sum of all held item weights * count
    float total_volume()  const; // sum of all held item volumes * count
    // Deep versions include contents of every open container
    float total_weight_deep() const;
    float total_volume_deep() const;

    // Returns a 0..1 speed penalty based on carried weight vs capacity.
    // 0 = no penalty (under half capacity), 1 = fully encumbered (at or over max_carry_kg).
    float mobility_penalty(float max_carry_kg = 25.f) const;

    const std::vector<InventorySlot>& slots() const { return m_slots; }
    std::vector<InventorySlot>&       slots()       { return m_slots; }

    // Active hand l_hand / r_hand
    InventorySlot* active_hand();
    void           cycle_active_hand();
    const std::string& active_hand_id() const { return m_active_hand; }

    // Returns the hand slot id that currently holds a two-handed item,
    // or "" if neither hand holds one.
    std::string two_handed_hand_id() const;

    // True if either hand holds a two-handed item (the other hand is gripped).
    bool is_two_handed_held() const { return !two_handed_hand_id().empty(); }

    // Returns the id of the hand that is gripped (not the primary holder).
    // Returns "" if no two-handed item is held.
    std::string gripped_hand_id() const;

private:
    std::vector<InventorySlot> m_slots;
    std::string m_active_hand = "r_hand";

    // Helpers used by find_slot_deep / split
    static InventorySlot* find_in_list(std::vector<InventorySlot>& list,
                                        const std::string& id);
};

// ── Factory ───────────────────────────────────────────────────────────────────
// Build the standard Stationeers-style player inventory.
Inventory make_player_inventory();

// Build a minimal mob inventory (l_hand + r_hand only).
Inventory make_mob_inventory();

// ── InventoryComponent ────────────────────────────────────────────────────────
// ECS component: attach alongside HumanAppearance to give any mob entity
// held/equipped items.  The per-frame overlay-rebuild system in main.cpp
// watches this component and updates HumanAppearance layers automatically
// whenever the held/equipped items change.
struct InventoryComponent {
    Inventory   inv;
    std::string overlay_fp;   // last fingerprint, used for dirty detection

    InventoryComponent() : inv(make_mob_inventory()) {}
    explicit InventoryComponent(Inventory i) : inv(std::move(i)) {}
};
