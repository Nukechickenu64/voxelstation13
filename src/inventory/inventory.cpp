#include "inventory/inventory.h"
#include <algorithm>

bool InventorySlot::accepts(const ItemDef& def) const
{
    for (const auto& tag : accepts_tags) {
        if (tag == "*") return true;
        for (const auto& itag : def.tags)
            if (itag == tag) return true;
    }
    return false;
}

Inventory::Inventory(std::vector<InventorySlot> slots)
    : m_slots(std::move(slots))
{}

InventorySlot* Inventory::find_slot(const std::string& id)
{
    for (auto& s : m_slots)
        if (s.id == id) return &s;
    return nullptr;
}

const InventorySlot* Inventory::find_slot(const std::string& id) const
{
    for (const auto& s : m_slots)
        if (s.id == id) return &s;
    return nullptr;
}

bool Inventory::put(const std::string& slot_id, ItemStack stack)
{
    InventorySlot* slot = find_slot(slot_id);
    if (!slot) return false;
    if (slot->item.has_value()) return false;             // slot occupied
    if (stack.def && !slot->accepts(*stack.def)) return false;
    slot->item = std::move(stack);
    return true;
}

std::optional<ItemStack> Inventory::take(const std::string& slot_id)
{
    InventorySlot* slot = find_slot(slot_id);
    if (!slot || !slot->item) return std::nullopt;
    auto item = std::move(slot->item);
    slot->item = std::nullopt;
    return item;
}

void Inventory::swap(const std::string& a, const std::string& b)
{
    InventorySlot* sa = find_slot(a);
    InventorySlot* sb = find_slot(b);
    if (!sa || !sb) return;
    std::swap(sa->item, sb->item);
}

InventorySlot* Inventory::find_empty_accepting(const ItemDef& def)
{
    for (auto& s : m_slots)
        if (!s.item && s.accepts(def)) return &s;
    return nullptr;
}

InventorySlot* Inventory::active_hand()
{
    return find_slot(m_active_hand);
}

void Inventory::cycle_active_hand()
{
    m_active_hand = (m_active_hand == "r_hand") ? "l_hand" : "r_hand";
}
