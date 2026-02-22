#include "ui/inventory_panel.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

// ────────────────────────────────────────────────────────────────────────────
// Layout: all offsets are from equipment-section origin (eq_origin).
// SZ = SLOT_SIZE = 46.  GAP between columns = 10.
// Column X centres in a 320-wide panel (panel-relative, not screen-relative):
//   col0=25  col1=81  col2=137  col3=193  col4=249
//   (each column is 46 wide; 56 step between left-edges)
// Row Y starts (from eq_origin):
//   row0=0   row1=56   row2=112   row3=168   row4=224
// ────────────────────────────────────────────────────────────────────────────

static constexpr float SZ  = InventoryPanel::SLOT_SIZE;
static constexpr float GAP = InventoryPanel::SLOT_PAD;

// Equipment-section slot positions (offset from eq_origin = panel_tl + (10, 36))
// Format: { slot_id, {x_offset, y_offset}, {width, height}, empty_label }
static const SlotLayoutEntry k_equip_layout[] = {
    { "ears",     { 81,   0}, {SZ, SZ}, "EARS"    },
    { "head",     {137,   0}, {SZ, SZ}, "HEAD"    },
    { "glasses",  {193,   0}, {SZ, SZ}, "EYES"    },
    { "mask",     {137,  56}, {SZ, SZ}, "MASK"    },
    { "back",     { 25, 112}, {SZ, SZ}, "BACK"    },
    { "suit",     {118, 100}, {60, 68}, "SUIT"    },   // slightly larger
    { "gloves",   { 81, 168}, {SZ, SZ}, "GLVS"    },
    { "belt",     {137, 168}, {SZ, SZ}, "BELT"    },
    { "id_slot",  {193, 168}, {SZ, SZ}, "ID"      },
    { "shoes",    {137, 224}, {SZ, SZ}, "SHOES"   },
};

// ────────────────────────────────────────────────────────────────────────────
InventoryPanel::InventoryPanel(UIRenderer& ui)
    : m_ui(ui)
{}

// ────────────────────────────────────────────────────────────────────────────
PanelInteraction InventoryPanel::draw(Inventory& inv, glm::vec2 cursor,
                                       bool lmb_down, bool lmb_released,
                                       bool shift_held, float alpha)
{
    PanelInteraction result;
    if (alpha <= 0.01f) return result;

    m_hovered_slot.clear();

    const float fb_w  = static_cast<float>(m_ui.fb_width());
    const float fb_h  = static_cast<float>(m_ui.fb_height());

    // Panel anchored to right edge
    const glm::vec2 panel_tl = { fb_w - PANEL_WIDTH - 12.f, fb_h * 0.05f };
    const float     panel_h  = fb_h * 0.90f;

    // Panel background
    m_ui.rect(panel_tl, {PANEL_WIDTH, panel_h},
              {0.06f, 0.07f, 0.10f, 0.92f * alpha}, 8.f);

    // Title
    m_ui.text(panel_tl + glm::vec2(10.f, 8.f), "INVENTORY",
              {0.55f, 0.75f, 1.f, alpha}, 13.f);

    // ── Body silhouette ───────────────────────────────────────────────────────
    const glm::vec2 eq_origin = panel_tl + glm::vec2(10.f, 36.f);
    draw_silhouette(eq_origin, alpha);

    // ── Equipment slots ───────────────────────────────────────────────────────
    for (const auto& entry : k_equip_layout) {
        glm::vec2 pos  = eq_origin + entry.offset;
        auto* slot = inv.find_slot(entry.slot_id);
        bool hov = draw_slot(slot, entry.icon_label, pos, entry.size,
                             /*greyed=*/false, cursor, result);
        if (hov) m_hovered_slot = entry.slot_id;
    }

    // ── Divider ───────────────────────────────────────────────────────────────
    const float eq_section_h = 224.f + SZ + GAP;  // bottom of shoes row
    const float div_y = eq_origin.y + eq_section_h + 4.f;
    m_ui.rect({panel_tl.x + 10.f, div_y}, {PANEL_WIDTH - 20.f, 1.f},
              {0.3f, 0.4f, 0.6f, 0.5f * alpha}, 0.f);

    // ── Hands & Pockets ───────────────────────────────────────────────────────
    const float hands_y = div_y + 8.f;
    draw_section_label({panel_tl.x + 10.f, hands_y}, "HANDS  /  POCKETS", alpha);

    const float hand_row_y = hands_y + 18.f;

    // Columns: l_pocket(25), l_hand(81), r_hand(193), r_pocket(249)
    struct { const char* id; float x; const char* lbl; } hand_slots[] = {
        { "l_pocket", 25.f,  "L.PKT" },
        { "l_hand",   81.f,  "L.HND" },
        { "r_hand",  193.f,  "R.HND" },
        { "r_pocket",249.f,  "R.PKT" },
    };

    const std::string& active = inv.active_hand_id();
    for (const auto& hs : hand_slots) {
        glm::vec2 pos = { eq_origin.x + hs.x, hand_row_y };
        bool is_active_hand = (std::string(hs.id) == active);
        auto* slot = inv.find_slot(hs.id);

        // Highlight active hand
        if (is_active_hand) {
            m_ui.rect(pos - glm::vec2(2.f), {SZ + 4.f, SZ + 4.f},
                      {0.3f, 0.5f, 0.9f, 0.5f * alpha}, 5.f);
        }

        bool hov = draw_slot(slot, hs.lbl, pos, {SZ, SZ},
                             /*greyed=*/false, cursor, result);
        if (hov) m_hovered_slot = hs.id;
    }

    // ── Belt tool sub-slots ───────────────────────────────────────────────────
    const float belt_section_y = hand_row_y + SZ + GAP + 10.f;
    const auto* belt_slot = inv.find_slot("belt");
    bool belt_has_toolbelt = belt_slot && belt_slot->item.has_value();

    draw_section_label({panel_tl.x + 10.f, belt_section_y}, "BELT SLOTS", alpha);

    if (belt_slot && !belt_slot->children.empty()) {
        const float bs_row_y = belt_section_y + 18.f;
        // Draw 8 sub-slots in 2 rows of 4
        for (int i = 0; i < static_cast<int>(belt_slot->children.size()) && i < 8; ++i) {
            int col = i % 4;
            int row = i / 4;
            float bx = eq_origin.x + 25.f + static_cast<float>(col) * (SZ + GAP);
            float by = bs_row_y + static_cast<float>(row) * (SZ + GAP);
            // Use a slightly smaller slot for belt tools
            const float BSZ = SZ - 4.f;
            glm::vec2 bpos  = {bx, by};

            // Children are stored on belt_slot but we need a mutable pointer
            InventorySlot* child = const_cast<InventorySlot*>(&belt_slot->children[i]);
            bool hov = draw_slot(child, std::to_string(i + 1).c_str(),
                                 bpos, {BSZ, BSZ},
                                 /*greyed=*/!belt_has_toolbelt, cursor, result);
            if (hov) m_hovered_slot = child->id;
        }
    }

    // ── Stats ─────────────────────────────────────────────────────────────────
    const float stats_y = panel_tl.y + panel_h - 34.f;
    draw_stats(inv, {panel_tl.x, stats_y}, alpha);

    // ── Drag & drop logic ─────────────────────────────────────────────────────
    if (m_drag.active) {
        m_drag.current = cursor;
        draw_drag_ghost(cursor);

        if (lmb_released) {
            if (!m_hovered_slot.empty()) {
                result.type           = PanelInteraction::Type::DragDrop;
                result.slot_id        = m_drag.src_slot;
                result.target_slot_id = m_hovered_slot;

                if (shift_held && m_drag.dragged_item.def &&
                    m_drag.dragged_item.def->stack_max > 1 &&
                    m_drag.dragged_item.count > 1 &&
                    !m_drag.src_slot.empty())
                {
                    // Split half of the stack into target
                    int half = m_drag.dragged_item.count / 2;
                    inv.split(m_drag.src_slot, m_hovered_slot, half);
                } else if (m_drag.src_slot.empty()) {
                    // World drag: place into slot
                    inv.put(m_hovered_slot, m_drag.dragged_item);
                } else {
                    inv.swap(m_drag.src_slot, m_hovered_slot);
                }
            }
            m_drag = {};
        }
    } else if (lmb_down && !m_hovered_slot.empty()) {
        // Begin drag from slot
        auto* slot = inv.find_slot_deep(m_hovered_slot);
        if (slot && slot->item) {
            m_drag.active       = true;
            m_drag.src_slot     = m_hovered_slot;
            m_drag.origin       = cursor;
            m_drag.current      = cursor;
            m_drag.dragged_item = *slot->item;
            m_drag.split_mode   = shift_held;
        }
    }

    // ── Tooltip ───────────────────────────────────────────────────────────────
    if (!m_drag.active && !m_hovered_slot.empty()) {
        const auto* slot = inv.find_slot_deep(m_hovered_slot);
        if (slot && slot->item) draw_tooltip(*slot->item, cursor);
    }

    return result;
}

// ────────────────────────────────────────────────────────────────────────────
bool InventoryPanel::draw_slot(InventorySlot* slot, const char* fallback_label,
                                glm::vec2 pos, glm::vec2 size, bool greyed,
                                glm::vec2 cursor, PanelInteraction& out)
{
    bool hovering = cursor.x >= pos.x && cursor.x <= pos.x + size.x &&
                    cursor.y >= pos.y && cursor.y <= pos.y + size.y;

    glm::vec4 bg;
    if (greyed)
        bg = {0.08f, 0.08f, 0.10f, 0.45f};
    else if (hovering)
        bg = {0.25f, 0.35f, 0.52f, 0.92f};
    else
        bg = {0.11f, 0.13f, 0.18f, 0.82f};

    m_ui.rect(pos, size, bg, 3.f);

    // Slot border
    m_ui.rect(pos, size, {0.2f, 0.25f, 0.4f, 0.4f}, 3.f);

    if (slot && slot->item && slot->item->def && !greyed) {
        const ItemDef& def = *slot->item->def;
        // Icon placeholder (tinted rectangle)
        m_ui.rect(pos + glm::vec2(4.f, 4.f), size - glm::vec2(8.f, 8.f),
                  {0.25f, 0.38f, 0.58f, 0.75f}, 2.f);
        // Item name (truncated)
        std::string n = def.name;
        if (n.size() > 7) n = n.substr(0, 6) + ".";
        m_ui.text(pos + glm::vec2(3.f, size.y - 13.f), n, {1.f, 1.f, 1.f, 0.9f}, 9.f);
        // Stack count badge
        if (slot->item->count > 1) {
            std::string cnt = std::to_string(slot->item->count);
            m_ui.rect(pos + glm::vec2(size.x - 16.f, 2.f), {14.f, 13.f},
                      {0.15f, 0.15f, 0.2f, 0.88f}, 2.f);
            m_ui.text(pos + glm::vec2(size.x - 15.f, 3.f), cnt,
                      {0.9f, 1.f, 0.6f, 1.f}, 9.f);
        }
        // Integrity bar at bottom edge of slot
        if (slot->item->integrity < 1.f) {
            float fil = std::max(0.f, slot->item->integrity) * (size.x - 4.f);
            float bary = pos.y + size.y - 3.f;
            m_ui.rect({pos.x + 2.f, bary}, {size.x - 4.f, 2.f},
                      {0.25f, 0.25f, 0.25f, 0.7f}, 0.f);
            glm::vec4 ic = (slot->item->integrity > 0.5f)
                            ? glm::vec4{0.2f, 0.9f, 0.3f, 0.9f}
                            : glm::vec4{0.9f, 0.35f, 0.1f, 0.9f};
            m_ui.rect({pos.x + 2.f, bary}, {fil, 2.f}, ic, 0.f);
        }
    } else if (!greyed) {
        // Empty slot: show faint label
        m_ui.text(pos + glm::vec2(4.f, size.y * 0.5f - 4.f), fallback_label,
                  {0.3f, 0.35f, 0.45f, 0.6f}, 8.f);
    }

    (void)out;
    return hovering;
}

// ────────────────────────────────────────────────────────────────────────────
void InventoryPanel::draw_silhouette(glm::vec2 origin, float alpha)
{
    const glm::vec4 col = {0.15f, 0.18f, 0.25f, 0.55f * alpha};
    const float R = 3.f;
    // Head
    m_ui.rect(origin + glm::vec2(147.f, 4.f),  {26.f, 26.f}, col, 12.f);
    // Neck
    m_ui.rect(origin + glm::vec2(156.f, 30.f), { 8.f, 12.f}, col, 0.f);
    // Torso
    m_ui.rect(origin + glm::vec2(118.f, 42.f), {84.f, 80.f}, col, R);
    // Left arm
    m_ui.rect(origin + glm::vec2(68.f,  42.f), {46.f, 74.f}, col, R);
    // Right arm
    m_ui.rect(origin + glm::vec2(206.f, 42.f), {46.f, 74.f}, col, R);
    // Hips
    m_ui.rect(origin + glm::vec2(120.f, 122.f),{80.f, 36.f}, col, R);
    // Left leg
    m_ui.rect(origin + glm::vec2(118.f, 158.f),{36.f, 110.f}, col, R);
    // Right leg
    m_ui.rect(origin + glm::vec2(166.f, 158.f),{36.f, 110.f}, col, R);
}

// ────────────────────────────────────────────────────────────────────────────
void InventoryPanel::draw_section_label(glm::vec2 pos, const char* text, float alpha)
{
    m_ui.text(pos, text, {0.45f, 0.55f, 0.75f, 0.9f * alpha}, 10.f);
}

// ────────────────────────────────────────────────────────────────────────────
void InventoryPanel::draw_stats(Inventory& inv, glm::vec2 pos, float alpha)
{
    float w  = inv.total_weight();
    float v  = inv.total_volume();

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1)
       << "Weight: " << w << " kg   Vol: " << v << " L";

    m_ui.rect(pos + glm::vec2(10.f, 0.f), {PANEL_WIDTH - 20.f, 24.f},
              {0.05f, 0.06f, 0.09f, 0.8f * alpha}, 4.f);
    m_ui.text(pos + glm::vec2(16.f, 5.f), ss.str(),
              {0.65f, 0.75f, 0.9f, 0.9f * alpha}, 10.f);
}

// ────────────────────────────────────────────────────────────────────────────
void InventoryPanel::draw_tooltip(const ItemStack& item, glm::vec2 cursor)
{
    if (!item.def) return;
    const float W = 192.f, PAD = 7.f, LINE = 15.f;
    glm::vec2 tpos = cursor + glm::vec2(14.f, 14.f);

    // Count verbs + lines
    int n_verbs     = static_cast<int>(item.def->verbs.size());
    int n_lines     = 5 + n_verbs;
    float h         = PAD * 2.f + static_cast<float>(n_lines) * LINE;

    m_ui.rect(tpos, {W, h}, {0.04f, 0.04f, 0.08f, 0.95f}, 5.f);

    float ty = tpos.y + PAD;
    m_ui.text({tpos.x + PAD, ty}, item.def->name, {1.f, 1.f, 1.f, 1.f}, 12.f);
    ty += LINE;

    if (!item.custom_name.empty()) {
        m_ui.text({tpos.x + PAD, ty}, "\"" + item.custom_name + "\"",
                  {0.8f, 0.9f, 1.f, 1.f}, 10.f);
        ty += LINE;
    }

    // Tags
    std::string tags_str;
    for (const auto& t : item.def->tags) tags_str += "[" + t + "] ";
    if (!tags_str.empty()) {
        m_ui.text({tpos.x + PAD, ty}, tags_str, {0.5f, 0.8f, 0.5f, 1.f}, 9.f);
        ty += LINE;
    }

    std::ostringstream wv;
    wv << std::fixed << std::setprecision(2)
       << item.def->weight << " kg  /  " << item.def->volume << " L";
    m_ui.text({tpos.x + PAD, ty}, wv.str(), {0.7f, 0.8f, 0.9f, 1.f}, 10.f);
    ty += LINE;

    std::string integ = "Integrity: " +
                        std::to_string(static_cast<int>(item.integrity * 100.f)) + "%";
    glm::vec4 ic = (item.integrity > 0.5f)
                   ? glm::vec4{0.3f, 1.f, 0.4f, 1.f}
                   : glm::vec4{1.f, 0.4f, 0.15f, 1.f};
    m_ui.text({tpos.x + PAD, ty}, integ, ic, 10.f);
    ty += LINE + 2.f;

    if (n_verbs > 0) {
        m_ui.rect({tpos.x + PAD, ty}, {W - PAD * 2.f, 1.f},
                  {0.25f, 0.3f, 0.45f, 0.6f}, 0.f);
        ty += 4.f;
        for (const auto& v : item.def->verbs) {
            m_ui.text({tpos.x + PAD, ty}, v.name, {0.85f, 0.85f, 1.f, 0.9f}, 10.f);
            ty += LINE;
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
void InventoryPanel::draw_drag_ghost(glm::vec2 cursor)
{
    if (!m_drag.active || !m_drag.dragged_item.def) return;
    const float GZ = SZ * 0.9f;
    glm::vec2 gpos = cursor - glm::vec2(GZ * 0.5f);
    m_ui.rect(gpos, {GZ, GZ}, {0.3f, 0.5f, 0.85f, 0.7f}, 4.f);
    std::string n = m_drag.dragged_item.def->name;
    if (n.size() > 7) n = n.substr(0, 6) + ".";
    m_ui.text(gpos + glm::vec2(3.f, GZ - 13.f), n, {1.f, 1.f, 1.f, 0.95f}, 9.f);
    if (m_drag.split_mode) {
        m_ui.text(gpos,  "x" + std::to_string(m_drag.dragged_item.count / 2),
                  {1.f, 1.f, 0.4f, 1.f}, 9.f);
    }
}

// ────────────────────────────────────────────────────────────────────────────
void InventoryPanel::begin_world_drag(ItemStack item, glm::vec2 start_pos)
{
    m_drag        = {};
    m_drag.active       = true;
    m_drag.src_slot     = ""; // world
    m_drag.origin       = start_pos;
    m_drag.current      = start_pos;
    m_drag.dragged_item = std::move(item);
}

void InventoryPanel::cancel_drag()
{
    m_drag = {};
}
