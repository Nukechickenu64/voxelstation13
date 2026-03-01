#include "inventory/inventory.h"
#include "core/object_types.h"
#include <algorithm>
#include <numeric>
#include <cmath>

// ── Free functions ───────────────────────────────────────────────────────────
const char* condition_label(float integrity)
{
    if (integrity >= 0.9f) return "pristine";
    if (integrity >= 0.7f) return "worn";
    if (integrity >= 0.4f) return "damaged";
    if (integrity >= 0.1f) return "badly damaged";
    return "destroyed";
}

bool istype(const ItemDef& def, std::string_view ancestor_path)
{
    return ::istype(def.type_path, ancestor_path);
}

bool istype(const ItemStack& stack, std::string_view ancestor_path)
{
    if (!stack.def) return false;
    return ::istype(stack.def->type_path, ancestor_path);
}

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

    // ── Two-handed item enforcement (hand slots only) ─────────────────────────
    if (stack.def && (slot_id == "l_hand" || slot_id == "r_hand")) {
        const std::string other_id = (slot_id == "l_hand") ? "r_hand" : "l_hand";
        InventorySlot* other = find_slot(other_id);
        if (stack.def->two_handed) {
            // A two-handed item needs the other hand to be free
            if (other && other->item.has_value()) return false;
        }
        // Placing anything into a hand is blocked while the other hand holds
        // a two-handed item
        if (other && other->item.has_value() &&
            other->item->def && other->item->def->two_handed) {
            return false;
        }
    }

    // Volume check for container-type slots
    if (slot->volume_capacity > 0.f && stack.def) {
        float used = 0.f;
        for (const auto& c : slot->children)
            if (c.item && c.item->def)
                used += c.item->def->volume * c.item->count;
        if (used + stack.def->volume * stack.count > slot->volume_capacity)
            return false;
    }

    // Volume check for Container-category child slots:
    // The parent slot (whose .children contains this slot) holds the pool volume_capacity.
    if (slot->category == SlotCategory::Container && stack.def) {
        for (auto& ps : m_slots) {
            bool found = false;
            for (auto& c : ps.children)
                if (&c == slot) { found = true; break; }
            if (!found) continue;
            if (ps.volume_capacity > 0.f) {
                float used = 0.f;
                for (auto& c : ps.children)
                    if (&c != slot && c.item && c.item->def)
                        used += c.item->def->volume * static_cast<float>(c.item->count);
                if (used + stack.def->volume * stack.count > ps.volume_capacity)
                    return false;
            }
            break;
        }
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
    // If this is an open container, sync children → contents before removing
    if (slot->open) close_container(slot_id);
    auto item = std::move(slot->item);
    slot->item = std::nullopt;
    slot->children.clear();
    slot->open = false;
    return item;
}

void Inventory::swap(const std::string& a, const std::string& b)
{
    InventorySlot* sa = find_slot_deep(a);
    InventorySlot* sb = find_slot_deep(b);
    if (!sa || !sb || sa == sb) return;

    // Verify each slot accepts the incoming item (accepts_tags {"*"} always passes).
    const ItemDef* def_a = sa->item ? sa->item->def : nullptr;
    const ItemDef* def_b = sb->item ? sb->item->def : nullptr;
    if (def_a && !sb->accepts(*def_a)) return;
    if (def_b && !sa->accepts(*def_b)) return;

    // Two-handed enforcement for hand slots.
    // If both sides are hand slots we skip the check — both will change at once.
    bool both_hands = (a == "l_hand" || a == "r_hand") &&
                      (b == "l_hand" || b == "r_hand");
    if (!both_hands) {
        auto hand_ok = [&](const std::string& hand_id, const ItemDef* incoming) -> bool {
            if (hand_id != "l_hand" && hand_id != "r_hand") return true;
            const std::string other_id = (hand_id == "l_hand") ? "r_hand" : "l_hand";
            InventorySlot* other = find_slot(other_id);
            if (!other || !other->item) return true;
            if (incoming && incoming->two_handed) return false;   // needs free other hand
            if (other->item->def && other->item->def->two_handed) return false;
            return true;
        };
        if (!hand_ok(a, def_b)) return;
        if (!hand_ok(b, def_a)) return;
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

std::optional<ItemStack> Inventory::auto_equip(ItemStack stack)
{
    if (!stack.def) return std::nullopt;  // bogus item — silently discard
    // 1. Try the designated equip slot
    if (!stack.def->equip_slot.empty()) {
        if (put(stack.def->equip_slot, stack)) return std::nullopt;
    }
    // 2. Try active hand
    if (put(m_active_hand, stack)) return std::nullopt;
    // 3. Try other hand
    const std::string other = (m_active_hand == "r_hand") ? "l_hand" : "r_hand";
    if (put(other, stack)) return std::nullopt;
    // 4. Try pockets
    if (put("l_pocket", stack)) return std::nullopt;
    if (put("r_pocket", stack)) return std::nullopt;
    // 5. Any accepting empty slot
    InventorySlot* s = find_empty_accepting(*stack.def);
    if (s && put(s->id, stack)) return std::nullopt;
    // Could not place — return the item so the caller can handle it
    return stack;
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
static float stack_weight_deep(const ItemStack& s)
{
    float w = s.def ? s.def->weight * static_cast<float>(s.count) : 0.f;
    for (const auto& c : s.contents) w += stack_weight_deep(c);
    return w;
}
static float stack_volume_deep(const ItemStack& s)
{
    float v = s.def ? s.def->volume * static_cast<float>(s.count) : 0.f;
    for (const auto& c : s.contents) v += stack_volume_deep(c);
    return v;
}

float Inventory::total_weight_deep() const
{
    float total = 0.f;
    for (const auto& s : m_slots)
        if (s.item) total += stack_weight_deep(*s.item);
    return total;
}

float Inventory::total_volume_deep() const
{
    float total = 0.f;
    for (const auto& s : m_slots)
        if (s.item) total += stack_volume_deep(*s.item);
    return total;
}

float Inventory::mobility_penalty(float max_carry_kg) const
{
    if (max_carry_kg <= 0.f) return 1.f;
    const float w = total_weight_deep();
    if (w <= 0.f) return 0.f;
    const float ratio = w / max_carry_kg;
    if (ratio <= 0.5f) return 0.f;       // under half capacity: no penalty
    if (ratio >= 1.0f) return 1.f;       // at or over capacity: fully encumbered
    // Linear ramp: 0.5 → 0.0 penalty, 1.0 → 1.0 penalty
    return (ratio - 0.5f) * 2.f;
}

InventorySlot* Inventory::active_hand()
{
    return find_slot(m_active_hand);
}

void Inventory::cycle_active_hand()
{
    // If a two-handed item is held, force active hand to stay on the holding hand.
    const std::string th = two_handed_hand_id();
    if (!th.empty()) {
        m_active_hand = th;
        return;
    }
    m_active_hand = (m_active_hand == "r_hand") ? "l_hand" : "r_hand";
}

std::string Inventory::two_handed_hand_id() const
{
    for (const std::string& id : {std::string("r_hand"), std::string("l_hand")}) {
        const InventorySlot* s = find_slot(id);
        if (s && s->item && s->item->def && s->item->def->two_handed)
            return id;
    }
    return {};
}

std::string Inventory::gripped_hand_id() const
{
    const std::string th = two_handed_hand_id();
    if (th.empty()) return {};
    return (th == "r_hand") ? "l_hand" : "r_hand";
}

// ── Container open/close ──────────────────────────────────────────────────────

bool Inventory::open_container(const std::string& slot_id)
{
    InventorySlot* slot = find_slot_deep(slot_id);
    if (!slot || !slot->item || !slot->item->def) return false;
    if (!slot->item->def->is_container) return false;
    if (slot->open) return true; // already open

    // Decide how many visual child slots to create.
    // Use up to 20 slots, but always enough for every item already in contents.
    const int base_slots  = std::max(10, static_cast<int>(
        std::ceil(slot->item->def->container_volume / 3.f)));
    const int num_slots   = std::min(base_slots, 24);

    // Preserve any items already in contents
    std::vector<ItemStack> existing = std::move(slot->item->contents);
    slot->item->contents.clear();

    // Determine which tags child slots will filter by.
    // If the container def specifies container_accepts_tags, honour them;
    // otherwise accept anything.
    const std::vector<std::string> child_tags =
        slot->item->def->container_accepts_tags.empty()
            ? std::vector<std::string>{"*"}
            : slot->item->def->container_accepts_tags;

    slot->children.clear();
    for (int i = 0; i < num_slots; ++i) {
        InventorySlot child;
        child.id              = slot_id + "_c" + std::to_string(i);
        child.display_name    = std::to_string(i + 1);
        child.category        = SlotCategory::Container;
        child.accepts_tags    = child_tags;
        child.volume_capacity = 0.f; // volume enforced at parent slot level
        if (i < static_cast<int>(existing.size()))
            child.item = std::move(existing[i]);
        slot->children.push_back(std::move(child));
    }

    slot->open                   = true;
    slot->item->container_open   = true;
    // Set a unified pool volume on the parent slot
    slot->volume_capacity        = slot->item->def->container_volume;
    return true;
}

void Inventory::close_container(const std::string& slot_id)
{
    InventorySlot* slot = find_slot_deep(slot_id);
    if (!slot || !slot->item) return;

    // Save children items back into contents list
    slot->item->contents.clear();
    for (auto& child : slot->children) {
        if (child.item) {
            // Recursively close nested open containers first
            if (child.open) close_container(child.id);
            slot->item->contents.push_back(std::move(*child.item));
        }
    }
    slot->children.clear();
    slot->open                 = false;
    slot->volume_capacity      = 0.f;
    slot->item->container_open = false;
}

InventorySlot* Inventory::first_open_container()
{
    for (auto& s : m_slots)
        if (s.open && s.item && s.item->def && s.item->def->is_container)
            return &s;
    return nullptr;
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
    slots.push_back(make_slot("head",    "Head",    C::Equipment, {"helmet","hat","head"}));
    // Face / eyes
    slots.push_back(make_slot("eyes",    "Eyes",    C::Equipment, {"glasses","visor","goggles"}));
    // Ears / comms
    slots.push_back(make_slot("ears",    "Ears",    C::Equipment, {"headset","ears"}));
    // Mask
    slots.push_back(make_slot("mask",    "Mask",    C::Equipment, {"mask","gas_mask"}));
    // Outer suit (spacesuit, hardsuit, coat)
    slots.push_back(make_slot("suit",    "Suit",    C::Equipment, {"suit","hardsuit","coat"}));
    // Uniform / jumpsuit (base layer worn under suit)
    slots.push_back(make_slot("uniform", "Uniform", C::Equipment, {"uniform","jumpsuit"}));
    // Back slot: tanks, jetpack, backpack
    slots.push_back(make_slot("back",    "Back",    C::Equipment, {"back","tank","jetpack","backpack"}));
    // Gloves
    slots.push_back(make_slot("gloves",  "Gloves",  C::Equipment, {"gloves"}));
    // Belt (accepts toolbelts and belts)
    slots.push_back(make_slot("belt",    "Belt",    C::Equipment, {"belt","toolbelt"}));
    // Boots / shoes
    slots.push_back(make_slot("boots",   "Boots",   C::Equipment, {"shoes","boots"}));
    // ID card
    slots.push_back(make_slot("id_card", "ID",      C::Equipment, {"id_card"}));
    // PDA / tablet
    slots.push_back(make_slot("pda",     "PDA",     C::Equipment, {"pda","tablet"}));

    // ── Hand slots ────────────────────────────────────────────────────────────
    slots.push_back(make_slot("l_hand",   "L.Hand",  C::General,   {"*"}));
    slots.push_back(make_slot("r_hand",   "R.Hand",  C::General,   {"*"}));

    // ── Pocket slots (only accept items tagged "small") ───────────────────────
    {
        InventorySlot lp = make_slot("l_pocket", "L.Pocket", C::General, {"small"});
        lp.volume_capacity = 6.f; // 6L pocket
        slots.push_back(std::move(lp));
    }
    {
        InventorySlot rp = make_slot("r_pocket", "R.Pocket", C::General, {"small"});
        rp.volume_capacity = 6.f;
        slots.push_back(std::move(rp));
    }

    return Inventory(std::move(slots));
}

Inventory make_mob_inventory()
{
    using C = SlotCategory;
    std::vector<InventorySlot> slots;
    slots.push_back(make_slot("l_hand", "L.Hand", C::General, {"*"}));
    slots.push_back(make_slot("r_hand", "R.Hand", C::General, {"*"}));
    return Inventory(std::move(slots));
}
