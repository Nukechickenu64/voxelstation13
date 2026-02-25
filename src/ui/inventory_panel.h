#pragma once
#include "render/ui_renderer.h"
#include "inventory/inventory.h"
#include "core/types.h"
#include <optional>
#include <string>
#include <vector>

// Drag operation state
struct DragState {
    bool        active    = false;
    std::string src_slot;       // inventory slot id, or "" for world
    glm::vec2   origin{};       // screen position drag started
    glm::vec2   current{};
    ItemStack   dragged_item{};
    bool        split_mode = false; // true when shift is held → split half stack
};

// Result of a completed panel interaction this frame
struct PanelInteraction {
    enum class Type { None, SlotClicked, DragDrop, RightClick };
    Type        type = Type::None;
    std::string slot_id;
    std::string target_slot_id; // for DragDrop
    glm::vec2   screen_pos{};   // for RightClick context menu origin
};

// One entry in the body-layout slot grid
struct SlotLayoutEntry {
    const char* slot_id;
    glm::vec2   offset;          // from panel_origin (top-left of body region)
    glm::vec2   size;            // usually {SLOT_SIZE, SLOT_SIZE}
    const char* icon_label;      // short label painted when empty (e.g. "HEAD")
};

// Translucent inventory overlay drawn in Alt-mode.
// Replicates the Stationeers-style body-silhouette equipment layout.
class InventoryPanel {
public:
    InventoryPanel(UIRenderer& ui);

    // Draw and handle input; returns any interaction that happened.
    // shift_held enables stack-splitting on drop.
    // rmb_pressed fires a RightClick interaction on the hovered slot.
    PanelInteraction draw(Inventory& inv, glm::vec2 cursor,
                          bool lmb_down, bool lmb_released,
                          bool shift_held, bool rmb_pressed, float alpha);

    // Notify the panel that a world-face item is being dragged to it
    void begin_world_drag(ItemStack item, glm::vec2 start_pos);
    void cancel_drag();
    bool is_dragging() const { return m_drag.active; }

private:
    // Draw one slot at absolute screen pos; returns true if hovered
    bool draw_slot(InventorySlot* slot, const char* fallback_label,
                   glm::vec2 pos, glm::vec2 size, bool greyed,
                   glm::vec2 cursor);

    void draw_tooltip(const ItemStack& item, glm::vec2 cursor);
    void draw_drag_ghost(glm::vec2 cursor);
    void draw_silhouette(glm::vec2 origin, float alpha);
    void draw_section_label(glm::vec2 pos, const char* text, float alpha);
    void draw_stats(Inventory& inv, glm::vec2 pos, float alpha);

    // Container contents panel (shown to the left of the main panel when open)
    void draw_container_panel(Inventory& inv, InventorySlot& con_slot,
                              glm::vec2 panel_tl, float panel_h,
                              glm::vec2 cursor,
                              float alpha);

    UIRenderer& m_ui;
    DragState   m_drag{};
    std::string m_hovered_slot; // updated each frame

public:
    static constexpr float PANEL_WIDTH  = 320.f;
    static constexpr float SLOT_SIZE    = 46.f;     // standard slot square
    static constexpr float SLOT_PAD     = 5.f;
    static constexpr float CON_PANEL_W  = 280.f;    // container contents panel
};

