#include "ui/inventory_panel.h"
#include <algorithm>
#include <cmath>

InventoryPanel::InventoryPanel(UIRenderer& ui)
    : m_ui(ui)
{}

PanelInteraction InventoryPanel::draw(Inventory& inv, glm::vec2 cursor,
                                       bool lmb_down, bool lmb_released,
                                       float alpha)
{
    PanelInteraction result;
    if (alpha <= 0.01f) return result;

    float fb_w = static_cast<float>(m_ui.fb_width());
    float fb_h = static_cast<float>(m_ui.fb_height());

    // Panel anchor: right side
    glm::vec2 panel_pos = {fb_w - PANEL_WIDTH - 12.f,
                            fb_h * 0.1f};
    float panel_h = fb_h * 0.8f;

    // Background
    m_ui.rect(panel_pos, {PANEL_WIDTH, panel_h},
              {0.08f, 0.08f, 0.12f, 0.88f * alpha}, 6.f);
    m_ui.text(panel_pos + glm::vec2(10, 8), "INVENTORY",
              {0.7f, 0.8f, 1.f, alpha}, 13.f);

    float cx = panel_pos.x + SLOT_PADDING;
    float cy = panel_pos.y + 30.f;

    std::string hovered_slot;

    for (auto& slot : inv.slots()) {
        glm::vec2 spos = {cx, cy};
        bool hovered = false;
        draw_slot(const_cast<InventorySlot&>(slot), spos,
                  {SLOT_SIZE, SLOT_SIZE}, cursor, hovered, result);
        if (hovered) hovered_slot = slot.id;

        cy += SLOT_SIZE + SLOT_PADDING;

        // Open container sub-panel if slot is open container
        if (slot.item && slot.item->def && slot.item->def->is_container && slot.open) {
            for (auto& child : slot.children) {
                glm::vec2 cspos = {cx + 12.f, cy};
                bool ch = false;
                draw_slot(const_cast<InventorySlot&>(child), cspos,
                          {SLOT_SIZE, SLOT_SIZE}, cursor, ch, result);
                cy += SLOT_SIZE + SLOT_PADDING;
            }
        }

        if (cy + SLOT_SIZE > panel_pos.y + panel_h) break; // overflow
    }

    // Handle drag-and-drop
    if (m_drag.active) {
        // Update drag position
        m_drag.current = cursor;
        draw_drag_ghost(cursor);

        if (lmb_released) {
            // Drop onto hovered slot
            if (!hovered_slot.empty()) {
                result.type            = PanelInteraction::Type::DragDrop;
                result.slot_id         = m_drag.src_slot;
                result.target_slot_id  = hovered_slot;
                // Execute the move
                if (m_drag.src_slot.empty()) {
                    // World drag: place into slot
                    inv.put(hovered_slot, m_drag.dragged_item);
                } else {
                    inv.swap(m_drag.src_slot, hovered_slot);
                }
            }
            m_drag = {};
        }
    } else if (lmb_down && !hovered_slot.empty()) {
        // Begin drag
        auto* slot = inv.find_slot(hovered_slot);
        if (slot && slot->item) {
            m_drag.active     = true;
            m_drag.src_slot   = hovered_slot;
            m_drag.origin     = cursor;
            m_drag.current    = cursor;
            m_drag.dragged_item = *slot->item;
        }
    }

    // Tooltip
    if (!m_drag.active && !hovered_slot.empty()) {
        const auto* slot = inv.find_slot(hovered_slot);
        if (slot && slot->item) draw_tooltip(*slot->item, cursor);
    }

    return result;
}

void InventoryPanel::draw_slot(InventorySlot& slot, glm::vec2 pos,
                                glm::vec2 size, glm::vec2 cursor,
                                bool& hovered, PanelInteraction& out)
{
    bool hovering = cursor.x >= pos.x && cursor.x <= pos.x + size.x &&
                    cursor.y >= pos.y && cursor.y <= pos.y + size.y;
    hovered = hovering;

    glm::vec4 bg = hovering ? glm::vec4{0.25f,0.3f,0.45f,0.9f}
                             : glm::vec4{0.12f,0.12f,0.18f,0.85f};
    m_ui.rect(pos, size, bg, 3.f);

    // Slot label
    m_ui.text(pos + glm::vec2(2, size.y - 12), slot.id,
              {0.5f,0.5f,0.6f,0.8f}, 9.f);

    if (slot.item && slot.item->def) {
        // Icon placeholder
        m_ui.rect(pos + glm::vec2(4,4), size - glm::vec2(8,8),
                  {0.3f,0.4f,0.55f,0.7f}, 2.f);
        m_ui.text(pos + glm::vec2(4,16), slot.item->def->name.substr(0,8),
                  {1,1,1,0.9f}, 10.f);
    }
    (void)out;
}

void InventoryPanel::draw_tooltip(const ItemStack& item, glm::vec2 cursor)
{
    if (!item.def) return;
    const float W = 160.f, PAD = 6.f, LINE = 14.f;
    glm::vec2 tpos = cursor + glm::vec2(12, 12);
    float h = 4 * LINE + PAD * 2;
    m_ui.rect(tpos, {W, h}, {0.05f,0.05f,0.1f,0.92f}, 4.f);
    m_ui.text(tpos + glm::vec2(PAD, PAD),               item.def->name,                {1,1,1,1},     12.f);
    m_ui.text(tpos + glm::vec2(PAD, PAD + LINE),         "Weight: "+std::to_string(item.def->weight), {0.8f,0.8f,0.8f,1}, 10.f);
    std::string tags_str;
    for (const auto& t : item.def->tags) tags_str += t + " ";
    m_ui.text(tpos + glm::vec2(PAD, PAD + LINE*2), tags_str, {0.6f,0.8f,0.6f,1}, 9.f);
    m_ui.text(tpos + glm::vec2(PAD, PAD + LINE*3),
              "Integrity: "+std::to_string(static_cast<int>(item.integrity*100))+"%",
              {0.9f,0.7f,0.3f,1}, 9.f);
}

void InventoryPanel::draw_drag_ghost(glm::vec2 cursor)
{
    if (!m_drag.active || !m_drag.dragged_item.def) return;
    const float SZ = InventoryPanel::SLOT_SIZE * 0.85f;
    m_ui.rect(cursor - glm::vec2(SZ*0.5f), {SZ,SZ}, {0.3f,0.5f,0.8f,0.6f}, 4.f);
    m_ui.text(cursor - glm::vec2(SZ*0.5f, 0), m_drag.dragged_item.def->name.substr(0,6),
              {1,1,1,0.9f}, 9.f);
}

void InventoryPanel::begin_world_drag(ItemStack item, glm::vec2 start_pos)
{
    m_drag         = {};
    m_drag.active  = true;
    m_drag.src_slot= "";
    m_drag.origin  = start_pos;
    m_drag.current = start_pos;
    m_drag.dragged_item = std::move(item);
}

void InventoryPanel::cancel_drag()
{
    m_drag = {};
}
