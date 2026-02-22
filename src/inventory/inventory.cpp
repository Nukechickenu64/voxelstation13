#include "inventory/inventory.h"
#include <algorithm>
#include <numeric>

// ── InventorySlot ─────────────────────────────────────────────────────────────
bool InventorySlot::accepts(const ItemDef& def) const
{
    for (const auto& tag : accepts_tags) {
        if (tag == "*") return true;
        for (const auto& itag : def.tags)
            if (itag == tag) return true;
    }
    return false;
}

// ── Inventory ─────────────────────────────────────────────────────────────────
Inventory::Inventory(std::vector<InventorySlot> slots)
    : m_slots(std::move(slots))
{}

/*static*/ InventorySlot* Inventory::find_in_list(std::vector<InventorySlot>& list,
                                                    const std::string& id)
{
    for (auto& s : list) {
        if (s.id == id) return &s;
        // Recurse into open container children
        if (!s.children.empty()) {
            if (auto* c = find_in_list(s.children, id)) return c;
        }
    }
    return nullptr;
}

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

InventorySlot* Inventory::find_slot_deep(const std::string& id)
{
    return find_in_list(m_slots, id);
}

bool Inventory::put(const std::string& slot_id, ItemStack stack)
{
    InventorySlot* slot = find_slot_deep(slot_id);
    if (!slot) return false;

    // Volume check for container-type slots
    if (slot->volume_capacity > 0.f && stack.def) {
        float used = 0.f;
        for (const auto& c : slot->children)
            if (c.item && c.item->def)
                used += c.item->def->volume * c.item->count;
        if (used + stack.def->volume * stack.count > slot->volume_capacity)
            return false;
    }

    // Stack merging: if same item and stackable
    if (slot->item.has_value() && stack.def &&
        slot->item->def == stack.def && stack.def->stack_max > 1)
    {
        int space = stack.def->stack_max - slot->item->count;
        if (space <= 0) return false;
        int take = std::min(space, stack.count);
        slot->item->count += take;
        return true;
    }

    if (slot->item.has_value()) return false;             // occupied, not stackable
    if (stack.def && !slot->accepts(*stack.def)) return false;
    slot->item = std::move(stack);
    return true;
}

std::optional<ItemStack> Inventory::take(const std::string& slot_id)
{
    InventorySlot* slot = find_slot_deep(slot_id);
    if (!slot || !slot->item) return std::nullopt;
    auto item = std::move(slot->item);
    slot->item = std::nullopt;
    // When a container item is taken out, collapse its children
    slot->children.clear();
    slot->open = false;
    return item;
}

void Inventory::swap(const std::string& a, const std::string& b)
{
    InventorySlot* sa = find_slot_deep(a);
    InventorySlot* sb = find_slot_deep(b);
    if (!sa || !sb) return;
    // Check cross-slot compatibility
    if (sa->item && sb->item) {
        // Both occupied: only swap if each accepts the other
        bool sa_accepts = !sa->item->def || sb->accepts(*sa->item->def) ||
                          sa->accepts_tags == sb->accepts_tags;
        bool sb_accepts = !sb->item->def || sa->accepts(*sb->item->def) ||
                          sa->accepts_tags == sb->accepts_tags;
        (void)sa_accepts; (void)sb_accepts;
    }
    std::swap(sa->item, sb->item);
}

bool Inventory::split(const std::string& src_slot, const std::string& dst_slot, int count)
{
    InventorySlot* src = find_slot_deep(src_slot);
    if (!src || !src->item || !src->item->def) return false;
    if (src->item->def->stack_max <= 1) return false;
    if (count <= 0 || count >= src->item->count) return false;

    ItemStack split_stack = *src->item;
    split_stack.count = count;
    src->item->count -= count;

    return put(dst_slot, std::move(split_stack));
}

InventorySlot* Inventory::find_empty_accepting(const ItemDef& def)
{
    for (auto& s : m_slots)
        if (!s.item && s.accepts(def)) return &s;
    return nullptr;
}

bool Inventory::auto_equip(ItemStack stack)
{
    if (!stack.def) return false;
    // 1. Try the designated equip slot
    if (!stack.def->equip_slot.empty()) {
        if (put(stack.def->equip_slot, stack)) return true;
    }
    // 2. Try active hand
    if (put(m_active_hand, stack)) return true;
    // 3. Try other hand
    const std::string other = (m_active_hand == "r_hand") ? "l_hand" : "r_hand";
    if (put(other, stack)) return true;
    // 4. Try pockets
    if (put("l_pocket", stack)) return true;
    if (put("r_pocket", stack)) return true;
    // 5. Any accepting empty slot
    InventorySlot* s = find_empty_accepting(*stack.def);
    if (s) { s->item = stack; return true; }
    return false;
}

float Inventory::total_weight() const
{
    float total = 0.f;
    for (const auto& s : m_slots)
        if (s.item && s.item->def)
            total += s.item->def->weight * static_cast<float>(s.item->count);
    return total;
}

float Inventory::total_volume() const
{
    float total = 0.f;
    for (const auto& s : m_slots)
        if (s.item && s.item->def)
            total += s.item->def->volume * static_cast<float>(s.item->count);
    return total;
}

InventorySlot* Inventory::active_hand()
{
    return find_slot(m_active_hand);
}

void Inventory::cycle_active_hand()
{
    m_active_hand = (m_active_hand == "r_hand") ? "l_hand" : "r_hand";
}

// ── Factory ───────────────────────────────────────────────────────────────────
// Helper — build a slot quickly
static InventorySlot make_slot(const char* id, const char* display,
                                SlotCategory cat,
                                std::initializer_list<const char*> tags)
{
    InventorySlot s;
    s.id           = id;
    s.display_name = display;
    s.category     = cat;
    for (const char* t : tags) s.accepts_tags.push_back(t);
    return s;
}

Inventory make_player_inventory()
{
    using C = SlotCategory;
    std::vector<InventorySlot> slots;

    // ── Equipment slots (body silhouette) ─────────────────────────────────────
    // Head
    slots.push_back(make_slot("head",     "Head",    C::Equipment, {"helmet","hat","head"}));
    // Face / eyes
    slots.push_back(make_slot("glasses",  "Glasses", C::Equipment, {"glasses","visor"}));
    // Ears / comms
    slots.push_back(make_slot("ears",     "Ears",    C::Equipment, {"headset","ears"}));
    // Mask
    slots.push_back(make_slot("mask",     "Mask",    C::Equipment, {"mask","gas_mask"}));
    // Suit / jumpsuit worn on body
    slots.push_back(make_slot("suit",     "Suit",    C::Equipment, {"suit","hardsuit","jumpsuit"}));
    // Back slot: tanks, jetpack, backpack
    slots.push_back(make_slot("back",     "Back",    C::Equipment, {"back","tank","jetpack","backpack"}));
    // Gloves
    slots.push_back(make_slot("gloves",   "Gloves",  C::Equipment, {"gloves"}));
    // Belt (accepts toolbelts and belts; children will be the 8 tool slots)
    {
        InventorySlot belt = make_slot("belt", "Belt", C::Equipment, {"belt","toolbelt"});
        // Pre-populate 8 belt sub-slots (only usable when a toolbelt is worn)
        for (int i = 1; i <= 8; ++i) {
            InventorySlot bs = make_slot(
                ("belt_" + std::to_string(i)).c_str(),
                std::to_string(i).c_str(),
                C::BeltTool, {"*"});
            belt.children.push_back(std::move(bs));
        }
        slots.push_back(std::move(belt));
    }
    // Shoes
    slots.push_back(make_slot("shoes",    "Shoes",   C::Equipment, {"shoes","boots"}));
    // ID card
    slots.push_back(make_slot("id_slot",  "ID",      C::Equipment, {"id_card"}));

    // ── Hand slots ────────────────────────────────────────────────────────────
    slots.push_back(make_slot("l_hand",   "L.Hand",  C::General,   {"*"}));
    slots.push_back(make_slot("r_hand",   "R.Hand",  C::General,   {"*"}));

    // ── Pocket slots (unlocked by wearing a suit with pockets) ────────────────
    {
        InventorySlot lp = make_slot("l_pocket", "L.Pocket", C::General, {"small","*"});
        lp.volume_capacity = 6.f; // 6L pocket
        slots.push_back(std::move(lp));
    }
    {
        InventorySlot rp = make_slot("r_pocket", "R.Pocket", C::General, {"small","*"});
        rp.volume_capacity = 6.f;
        slots.push_back(std::move(rp));
    }

    return Inventory(std::move(slots));
}
