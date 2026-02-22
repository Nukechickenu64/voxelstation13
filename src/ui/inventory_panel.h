#pragma once
#include "render/ui_renderer.h"
#include "inventory/inventory.h"
#include "core/types.h"
#include <optional>
#include <string>

// Drag operation state
struct DragState {
    bool        active    = false;
    std::string src_slot;       // inventory slot id, or "" for world
    glm::vec2   origin{};       // screen position drag started
    glm::vec2   current{};
    ItemStack   dragged_item{};
};

// Result of a completed panel interaction this frame
struct PanelInteraction {
    enum class Type { None, SlotClicked, DragDrop, RightClick };
    Type        type = Type::None;
    std::string slot_id;
    std::string target_slot_id; // for DragDrop
    glm::vec2   screen_pos{};   // for RightClick context menu origin
};

// Translucent inventory overlay drawn in Alt-mode.
class InventoryPanel {
public:
    InventoryPanel(UIRenderer& ui);

    // Draw and handle input; returns any interaction that happened
    PanelInteraction draw(Inventory& inv, glm::vec2 cursor, bool lmb_down,
                          bool lmb_released, float alpha);

    // Notify the panel that a world-face item is being dragged to it
    void begin_world_drag(ItemStack item, glm::vec2 start_pos);
    void cancel_drag();

private:
    void draw_slot(InventorySlot& slot, glm::vec2 pos, glm::vec2 size,
                   glm::vec2 cursor, bool& hovered, PanelInteraction& out);
    void draw_tooltip(const ItemStack& item, glm::vec2 cursor);
    void draw_drag_ghost(glm::vec2 cursor);

    UIRenderer& m_ui;
    DragState   m_drag{};

    static constexpr float PANEL_WIDTH  = 280.f;
    static constexpr float SLOT_SIZE    = 48.f;
    static constexpr float SLOT_PADDING = 6.f;
};
