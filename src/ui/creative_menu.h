#pragma once
#include "render/ui_renderer.h"
#include "inventory/inventory.h"
#include "inventory/item_registry.h"
#include "data/voxel_registry.h"
#include <vector>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// CreativeMenu — F2 overlay that lets the player scroll through every item and
// structure type and click to receive / select it.
// ─────────────────────────────────────────────────────────────────────────────
struct CreativeResult {
    const ItemDef* give_item   = nullptr;  // non-null → auto-equip this item
    uint16_t       place_voxel = 0;        // non-zero → select this voxel type
};

// Item category filter tabs — derived from the type_path classification system.
// Each value corresponds to a /obj/item/<category> prefix (All = no filter).
enum class ItemCategory {
    All,
    Tools,       // /obj/item/tool
    Equipment,   // /obj/item/clothing
    Storage,     // /obj/item/storage
    Medical,     // /obj/item/medical
    Weapons,     // /obj/item/weapon
    Misc,        // /obj/item/stack  +  anything that doesn't match above
};

class CreativeMenu {
public:
    explicit CreativeMenu(UIRenderer& ui);

    // Populate the sorted lists from the registries and open the overlay.
    void open(const ItemRegistry& items, const VoxelRegistry& voxels);
    void close();
    bool is_open() const { return m_open; }

    // Draw every frame while open. cursor / lmb_pressed come from the mouse.
    // scroll_y  = signed ticks from the scroll wheel (positive = up).
    // Returns a CreativeResult with the chosen item/voxel (both null/0 if none
    // chosen this frame).
    CreativeResult draw(glm::vec2 cursor, bool lmb_pressed,
                        float scroll_y, bool escape_pressed);

private:
    void draw_tab_bar(glm::vec2 origin, float width, float alpha);
    void draw_category_bar(glm::vec2 origin, float width, float alpha);

    // Returns a filtered view of m_items for the active ItemCategory.
    std::vector<const ItemDef*> filtered_items() const;

    UIRenderer& m_ui;
    bool        m_open = false;
    int         m_tab  = 0;                           // 0 = Items, 1 = Structures
    ItemCategory m_item_category = ItemCategory::All; // active item sub-filter
    float       m_scroll_offset = 0.f;                // vertical pixel scroll

    // Cached lists rebuilt every open()
    std::vector<const ItemDef*>                          m_items;
    std::vector<std::pair<uint16_t, const VoxelTypeDef*>> m_voxels;

    static constexpr float CELL_SIZE    = 72.f;
    static constexpr float CELL_PAD     = 6.f;
    static constexpr int   COLS         = 5;
    static constexpr float CATEGORY_H   = 28.f;
};
