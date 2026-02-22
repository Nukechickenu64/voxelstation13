#pragma once
#include <string>
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
    std::string            id;
    std::string            name;
    std::string            icon;     // path in textures/items/
    std::vector<std::string> tags;
    float                  weight   = 0.1f;
    int                    stack_max= 1;
    bool                   is_container = false;
    std::vector<ItemVerb>  verbs;
};

struct ItemStack {
    const ItemDef* def    = nullptr;
    int            count  = 0;
    // Per-instance data (damage, custom name, etc.)
    std::string    custom_name;
    float          integrity = 1.f;  // 0-1
};

// ── Inventory slot ────────────────────────────────────────────────────────────
struct InventorySlot {
    std::string             id;
    std::vector<std::string> accepts_tags;  // "*" = anything
    std::optional<ItemStack> item;

    // For container items: child slots are stored here when opened
    std::vector<InventorySlot> children;
    bool                       open = false;

    bool accepts(const ItemDef& def) const;
};

// ── Inventory ─────────────────────────────────────────────────────────────────
class Inventory {
public:
    explicit Inventory(std::vector<InventorySlot> slots);

    InventorySlot*       find_slot(const std::string& id);
    const InventorySlot* find_slot(const std::string& id) const;

    // Try to place a stack into a specific slot; returns false on failure
    bool put(const std::string& slot_id, ItemStack stack);

    // Remove item from slot (returns it so caller can drop it)
    std::optional<ItemStack> take(const std::string& slot_id);

    // Swap two slots' contents
    void swap(const std::string& a, const std::string& b);

    // Returns first slot that accepts and is empty, else nullptr
    InventorySlot* find_empty_accepting(const ItemDef& def);

    const std::vector<InventorySlot>& slots() const { return m_slots; }
    std::vector<InventorySlot>&       slots()       { return m_slots; }

    // Active hand l_hand / r_hand
    InventorySlot* active_hand();
    void           cycle_active_hand();
    const std::string& active_hand_id() const { return m_active_hand; }

private:
    std::vector<InventorySlot> m_slots;
    std::string m_active_hand = "r_hand";
};
