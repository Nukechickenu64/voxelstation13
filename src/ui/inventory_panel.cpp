#include "ui/inventory_panel.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

// ────────────────────────────────────────────────────────────────────────────
// Layout: all offsets are from equipment-section origin (eq_origin).
// SZ = SLOT_SIZE = 46.  Step between column left-edges = 56.
// Column X (from eq_origin): col0=25  col1=81  col2=137  col3=193  col4=249
// Row Y   (from eq_origin):  row0=0   row1=56  row2=112  row3=168  row4=224
//
//  Grid                 col0   col1   col2   col3   col4
//  row0  y=  0                 EARS   HEAD   EYES
//  row1  y= 56                 MASK   SUIT   UNIF
//  row2  y=112          BACK
//  row3  y=168                 GLVS   BELT   ID     PDA
//  row4  y=224                        BOOT
// ────────────────────────────────────────────────────────────────────────────

static constexpr float SZ  = InventoryPanel::SLOT_SIZE;
static constexpr float GAP = InventoryPanel::SLOT_PAD;

// Equipment-section slot positions (offset from eq_origin = panel_tl + (10, 36))
// Format: { slot_id, {x_offset, y_offset}, {width, height}, empty_label }
static const SlotLayoutEntry k_equip_layout[] = {
    // Row 0 — head accessories
    { "ears",    { 81,   0}, {SZ, SZ}, "EARS" },
    { "head",    {137,   0}, {SZ, SZ}, "HEAD" },
    { "eyes",    {193,   0}, {SZ, SZ}, "EYES" },
    // Row 1 — face / torso layers
    { "mask",    { 81,  56}, {SZ, SZ}, "MASK" },
    { "suit",    {137,  56}, {SZ, SZ}, "SUIT" },
    { "uniform", {193,  56}, {SZ, SZ}, "UNIF" },
    // Row 2 — carried items (back on far-left column)
    { "back",    { 25, 112}, {SZ, SZ}, "BACK" },
    // Row 3 — belt accessories
    { "gloves",  { 81, 168}, {SZ, SZ}, "GLVS" },
    { "belt",    {137, 168}, {SZ, SZ}, "BELT" },
    { "id_card", {193, 168}, {SZ, SZ}, "ID"   },
    { "pda",     {249, 168}, {SZ, SZ}, "PDA"  },
    // Row 4 — boots
    { "boots",   {137, 224}, {SZ, SZ}, "BOOT" },
};

// ────────────────────────────────────────────────────────────────────────────
InventoryPanel::InventoryPanel(UIRenderer& ui)
    : m_ui(ui)
{}

// ────────────────────────────────────────────────────────────────────────────
PanelInteraction InventoryPanel::draw(Inventory& inv, glm::vec2 cursor,
                                       bool lmb_down, bool lmb_released,
                                       bool shift_held, bool rmb_pressed, float alpha)
{
    PanelInteraction result;
    if (alpha <= 0.01f) return result;

    m_hovered_slot.clear();

    const float fb_w  = static_cast<float>(m_ui.fb_width());
    const float fb_h  = static_cast<float>(m_ui.fb_height());

    // Panel anchored to right edge
    const glm::vec2 panel_tl = { fb_w - PANEL_WIDTH - 12.f, fb_h * 0.05f };
    const float     panel_h  = fb_h * 0.90f;

    // ── Accent colours (reused throughout this function) ─────────────────────
    // (values are inlined below for clarity)

    // ── Panel outer frame (1-px bright line) ─────────────────────────────────
    m_ui.rect(panel_tl - glm::vec2(1.f), {PANEL_WIDTH + 2.f, panel_h + 2.f},
              {0.20f, 0.28f, 0.42f, 0.55f * alpha}, 9.f);

    // Panel background
    m_ui.rect(panel_tl, {PANEL_WIDTH, panel_h},
              {0.07f, 0.08f, 0.115f, 0.94f * alpha}, 8.f);

    // ── Coloured top accent strip ─────────────────────────────────────────────
    m_ui.rect(panel_tl, {PANEL_WIDTH, 3.f},
              {0.30f, 0.55f, 1.00f, 0.9f * alpha}, 0.f);

    // ── Header area ───────────────────────────────────────────────────────────
    // Title text
    m_ui.text(panel_tl + glm::vec2(12.f, 7.f), "INVENTORY",
              {0.70f, 0.88f, 1.f, alpha}, 13.f);
    // Subtitle hint (right-aligned in header)
    m_ui.text(panel_tl + glm::vec2(PANEL_WIDTH - 90.f, 9.f),
              "Alt = close", {0.30f, 0.38f, 0.52f, 0.75f * alpha}, 9.f);

    // Thin divider below header
    m_ui.rect(panel_tl + glm::vec2(0.f, 26.f), {PANEL_WIDTH, 1.f},
              {0.22f, 0.30f, 0.48f, 0.5f * alpha}, 0.f);

    // ── Body silhouette ───────────────────────────────────────────────────────
    const glm::vec2 eq_origin = panel_tl + glm::vec2(10.f, 36.f);
    draw_silhouette(eq_origin, alpha);

    // ── Equipment slots ───────────────────────────────────────────────────────
    for (const auto& entry : k_equip_layout) {
        glm::vec2 pos  = eq_origin + entry.offset;
        auto* slot = inv.find_slot(entry.slot_id);
        // Show source slot as empty while its item is being dragged
        bool dragging_from = m_drag.active && slot && slot->id == m_drag.src_slot;
        bool hov = draw_slot(dragging_from ? nullptr : slot, entry.icon_label, pos, entry.size,
                             /*greyed=*/false, cursor);
        if (hov) m_hovered_slot = entry.slot_id;
    }

    // ── Divider ───────────────────────────────────────────────────────────────
    const float eq_section_h = 224.f + SZ + GAP;  // bottom of shoes row
    const float div_y = eq_origin.y + eq_section_h + 4.f;
    // Pip + full-width divider
    m_ui.rect({panel_tl.x,        div_y - 0.5f}, {4.f,              2.f},
              {0.30f, 0.55f, 1.00f, 0.85f * alpha}, 0.f);
    m_ui.rect({panel_tl.x + 4.f,  div_y},        {PANEL_WIDTH - 4.f, 1.f},
              {0.22f, 0.30f, 0.48f, 0.45f * alpha}, 0.f);

    // ── Hands & Pockets ───────────────────────────────────────────────────────
    const float hands_y = div_y + 8.f;
    draw_section_label({panel_tl.x + 10.f, hands_y}, "HANDS / POCKETS", alpha);

    const float hand_row_y = hands_y + 18.f;

    // Columns: l_pocket(25), l_hand(81), r_hand(193), r_pocket(249)
    struct HandEntry { const char* id; float x; const char* lbl; const char* sublabel; };
    HandEntry hand_slots[] = {
        { "l_pocket", 25.f,  "L.PKT", "PKT" },
        { "l_hand",   81.f,  "L.HND", "L"   },
        { "r_hand",  193.f,  "R.HND", "R"   },
        { "r_pocket",249.f,  "R.PKT", "PKT" },
    };

    const std::string& active      = inv.active_hand_id();
    const std::string  grip_hand   = inv.gripped_hand_id();   // "" if no two-hander
    const std::string  th_hand     = inv.two_handed_hand_id(); // holding slot
    const bool two_hander = !th_hand.empty();

    for (const auto& hs : hand_slots) {
        glm::vec2 pos = { eq_origin.x + hs.x, hand_row_y };
        bool is_active_hand = (std::string(hs.id) == active);
        bool is_grip_hand   = two_hander && (std::string(hs.id) == grip_hand);
        auto* slot = inv.find_slot(hs.id);

        // Highlight active hand (blue ring)
        if (is_active_hand) {
            m_ui.rect(pos - glm::vec2(2.f), {SZ + 4.f, SZ + 4.f},
                      {0.3f, 0.5f, 0.9f, 0.5f * alpha}, 5.f);
        }

        // Gripped hand: amber ring + no item (visually gripping the two-handed item)
        if (is_grip_hand) {
            m_ui.rect(pos - glm::vec2(2.f), {SZ + 4.f, SZ + 4.f},
                      {0.7f, 0.45f, 0.05f, 0.55f * alpha}, 5.f);
        }

        bool dragging_from = m_drag.active && slot && slot->id == m_drag.src_slot;

        if (is_grip_hand) {
            // Show the gripped hand as an occupied-but-locked slot with amber tint.
            // Do NOT draw the item icon here — that icon is already visible in the
            // holding hand, so repeating it made the item look duplicated.
            m_ui.rect(pos, {SZ, SZ}, {0.35f, 0.22f, 0.03f, 0.8f * alpha}, 4.f);
            m_ui.text(pos + glm::vec2(4.f, SZ * 0.5f - 6.f), "GRIP",
                      {1.f, 0.75f, 0.1f, alpha}, 9.f);
            // Still make it hoverable so tooltips/RMB work
            if (cursor.x >= pos.x && cursor.x < pos.x + SZ &&
                cursor.y >= pos.y && cursor.y < pos.y + SZ)
                m_hovered_slot = hs.id;
        } else {
            bool hov = draw_slot(dragging_from ? nullptr : slot, hs.lbl, pos, {SZ, SZ},
                                 /*greyed=*/false, cursor);
            if (hov) m_hovered_slot = hs.id;
        }

        // Sub-label below slot box
        bool is_hand = (std::string(hs.id) == "l_hand" || std::string(hs.id) == "r_hand");
        glm::vec4 sub_col = is_active_hand
            ? glm::vec4{0.50f, 0.75f, 1.f, 0.95f * alpha}
            : glm::vec4{0.32f, 0.40f, 0.55f, 0.65f * alpha};
        // Center the 3-char label under the slot (approx 6px/char)
        float sub_x = pos.x + SZ * 0.5f - (is_hand ? 3.5f : 9.f);
        m_ui.text({sub_x, pos.y + SZ + 2.f}, hs.sublabel, sub_col,
                  is_hand ? 10.f : 8.f);
        // Active hand underline pip
        if (is_active_hand) {
            m_ui.rect({pos.x + 4.f, pos.y + SZ + 14.f}, {SZ - 8.f, 2.f},
                      {0.35f, 0.60f, 1.f, 0.7f * alpha}, 1.f);
        }
    }

    // ── Stats ─────────────────────────────────────────────────────────────────
    const float stats_y = panel_tl.y + panel_h - 34.f;
    draw_stats(inv, {panel_tl.x, stats_y}, alpha);

    // ── Open container panel (shown to the left of main panel) ────────────────
    m_con_close_hovered = false; // reset each frame before drawing container panel
    if (InventorySlot* con = inv.first_open_container()) {
        draw_container_panel(inv, *con, panel_tl, panel_h,
                             cursor, alpha);
    }

    // ── Container close (×) button handling ────────────────────────────────────
    if (m_con_close_hovered && lmb_released) {
        if (InventorySlot* con = inv.first_open_container()) {
            result.type    = PanelInteraction::Type::ContainerClose;
            result.slot_id = con->id;
        }
        m_drag = {};
        return result;
    }

    // ── Drag & drop logic ─────────────────────────────────────────────────────
    if (m_drag.active) {
        m_drag.current = cursor;
        draw_drag_ghost(cursor);

        if (lmb_released) {
            if (!m_hovered_slot.empty()) {
                // Distinguish a short click (≤6 px movement, same slot) from a real drag
                const bool is_click =
                    !m_drag.src_slot.empty() &&
                    (m_drag.src_slot == m_hovered_slot) &&
                    (glm::distance(m_drag.origin, m_drag.current) < 6.f);

                if (is_click) {
                    // Emit SlotClicked so the caller can handle click-to-swap-with-hand
                    result.type    = PanelInteraction::Type::SlotClicked;
                    result.slot_id = m_drag.src_slot;
                } else {
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
            } else if (!m_drag.src_slot.empty()) {
                // Released outside the panel with an inventory item → drop to world
                result.type    = PanelInteraction::Type::DropToWorld;
                result.slot_id = m_drag.src_slot;
            }
            m_drag = {};
        }
    } else if (lmb_down && !m_con_close_hovered && !m_hovered_slot.empty()) {
        // Begin drag from slot (suppress when hovering the container close button)
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

    // ── Right-click: open context menu for hovered slot ─────────────────────
    if (rmb_pressed && !m_hovered_slot.empty() && !m_drag.active) {
        result.type       = PanelInteraction::Type::RightClick;
        result.slot_id    = m_hovered_slot;
        result.screen_pos = cursor;
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
                                glm::vec2 cursor)
{
    bool hovering = cursor.x >= pos.x && cursor.x <= pos.x + size.x &&
                    cursor.y >= pos.y && cursor.y <= pos.y + size.y;
    bool has_item = slot && slot->item && slot->item->def && !greyed;

    // ── Slot border (outer rect, 1 px inset on each side) ────────────────────
    // Draw a border-coloured background slightly larger, then the fill on top.
    glm::vec4 bdr_col;
    if (greyed)
        bdr_col = {0.14f, 0.14f, 0.18f, 0.50f};
    else if (hovering)
        bdr_col = {0.50f, 0.72f, 1.00f, 0.90f};
    else if (has_item)
        bdr_col = {0.28f, 0.36f, 0.52f, 0.85f};
    else
        bdr_col = {0.18f, 0.22f, 0.32f, 0.75f};

    m_ui.rect(pos - glm::vec2(1.f), size + glm::vec2(2.f), bdr_col, 4.f);

    // Inner fill
    glm::vec4 bg;
    if (greyed)
        bg = {0.07f, 0.07f, 0.09f, 0.45f};
    else if (hovering && has_item)
        bg = {0.18f, 0.26f, 0.42f, 0.95f};
    else if (hovering)
        bg = {0.14f, 0.20f, 0.32f, 0.88f};
    else
        bg = {0.10f, 0.12f, 0.175f, 0.88f};

    m_ui.rect(pos, size, bg, 3.f);

    if (has_item) {
        const ItemDef& def = *slot->item->def;
        // Sprite icon (full slot area with small padding)
        SDL_GPUTexture* icon_tex = m_ui.item_icon(def.id);
        if (icon_tex) {
            m_ui.image(pos + glm::vec2(3.f, 3.f), size - glm::vec2(6.f, 6.f),
                       icon_tex, 1.f);
        } else {
            // Fallback: tinted rectangle + name text
            m_ui.rect(pos + glm::vec2(4.f, 4.f), size - glm::vec2(8.f, 8.f),
                      {0.18f, 0.28f, 0.46f, 0.70f}, 2.f);
            std::string n = def.name;
            if (n.size() > 7) n = n.substr(0, 6) + ".";
            m_ui.text(pos + glm::vec2(3.f, size.y - 13.f), n, {0.85f, 0.90f, 1.f, 0.9f}, 9.f);
        }

        // Container open-indicator: small green dot top-left when item is a container
        if (def.is_container) {
            glm::vec4 dot_col = slot->item->container_open
                                ? glm::vec4{0.20f, 0.90f, 0.40f, 0.90f}
                                : glm::vec4{0.25f, 0.55f, 0.30f, 0.70f};
            m_ui.rect(pos + glm::vec2(2.f, 2.f), {6.f, 6.f}, dot_col, 3.f);
        }

        // Stack count badge (top-right)
        if (slot->item->count > 1) {
            std::string cnt = std::to_string(slot->item->count);
            float badge_w = static_cast<float>(cnt.size()) * 6.f + 4.f;
            m_ui.rect(pos + glm::vec2(size.x - badge_w - 1.f, 1.f),
                      {badge_w, 13.f}, {0.08f, 0.08f, 0.14f, 0.90f}, 2.f);
            m_ui.text(pos + glm::vec2(size.x - badge_w,  2.f), cnt,
                      {0.90f, 1.f, 0.55f, 1.f}, 9.f);
        }

        // Locked badge (bottom-left corner, amber) — shown for lockable containers
        if (slot->item->locked) {
            m_ui.rect(pos + glm::vec2(2.f, size.y - 15.f), {24.f, 13.f},
                      {0.55f, 0.38f, 0.0f, 0.92f}, 2.f);
            m_ui.text(pos + glm::vec2(4.f, size.y - 14.f), "LOCK",
                      {1.f, 0.9f, 0.1f, 1.f}, 7.f);
        }

        // Integrity bar at bottom edge of slot (3 px tall, full width with padding)
        if (slot->item->integrity < 1.f) {
            float bar_inner = size.x - 6.f;
            float fil = std::max(0.f, slot->item->integrity) * bar_inner;
            float bary = pos.y + size.y - 4.f;
            m_ui.rect({pos.x + 3.f, bary}, {bar_inner, 3.f},
                      {0.15f, 0.15f, 0.18f, 0.80f}, 1.f);
            glm::vec4 ic = (slot->item->integrity > 0.5f)
                            ? glm::vec4{0.15f, 0.85f, 0.28f, 0.95f}
                            : (slot->item->integrity > 0.25f)
                                ? glm::vec4{0.95f, 0.65f, 0.05f, 0.95f}
                                : glm::vec4{0.95f, 0.20f, 0.10f, 0.95f};
            m_ui.rect({pos.x + 3.f, bary}, {fil, 3.f}, ic, 1.f);
        }
    } else if (!greyed) {
        // Empty slot: faint centered label
        // Approximate x-centre: 6 px per character
        float label_len = static_cast<float>(strlen(fallback_label)) * 5.f;
        float lx = pos.x + (size.x - label_len) * 0.5f;
        float ly = pos.y + size.y * 0.5f - 4.f;
        m_ui.text({lx, ly}, fallback_label,
                  {0.28f, 0.34f, 0.46f, hovering ? 0.80f : 0.50f}, 8.f);
    }

    return hovering;
}

// ────────────────────────────────────────────────────────────────────────────
void InventoryPanel::draw_silhouette(glm::vec2 origin, float alpha)
{
    const glm::vec4 base  = {0.14f, 0.175f, 0.24f, 0.50f * alpha};
    const glm::vec4 hi    = {0.19f, 0.24f,  0.34f, 0.35f * alpha}; // lighter overlay on key shapes

    // Head (large corner radius → circle-ish)
    m_ui.rect(origin + glm::vec2(147.f,  4.f),  {26.f, 26.f},  base, 13.f);
    m_ui.rect(origin + glm::vec2(148.f,  5.f),  {24.f, 13.f},  hi,   11.f);

    // Ear wings — bridge head to the EARS (left) and EYES (right) slots in row 0
    m_ui.rect(origin + glm::vec2(120.f, 12.f),  {27.f,  8.f},  base,  2.f); // left ear
    m_ui.rect(origin + glm::vec2(173.f, 12.f),  {27.f,  8.f},  base,  2.f); // right ear

    // Neck
    m_ui.rect(origin + glm::vec2(157.f, 30.f),  { 7.f, 14.f},  base,  2.f);

    // Shoulders — wide enough to hint at row-1 side slots (MASK left, UNIFORM right)
    m_ui.rect(origin + glm::vec2( 76.f, 42.f),  {52.f,  8.f},  base,  3.f); // left shoulder
    m_ui.rect(origin + glm::vec2(192.f, 42.f),  {52.f,  8.f},  base,  3.f); // right shoulder

    // Torso (starts at row-1 y to align with SUIT slot)
    m_ui.rect(origin + glm::vec2(120.f, 48.f),  {80.f, 70.f},  base,  5.f);
    m_ui.rect(origin + glm::vec2(122.f, 50.f),  {76.f, 28.f},  hi,    4.f); // upper highlight

    // Arms
    m_ui.rect(origin + glm::vec2( 76.f, 48.f),  {40.f, 68.f},  base,  6.f); // left
    m_ui.rect(origin + glm::vec2(204.f, 48.f),  {40.f, 68.f},  base,  6.f); // right

    // Hips
    m_ui.rect(origin + glm::vec2(122.f, 118.f), {76.f, 34.f},  base,  4.f);

    // Legs (rounded bottom)
    m_ui.rect(origin + glm::vec2(120.f, 152.f), {34.f, 112.f}, base,  6.f); // left
    m_ui.rect(origin + glm::vec2(166.f, 152.f), {34.f, 112.f}, base,  6.f); // right
}

// ────────────────────────────────────────────────────────────────────────────
void InventoryPanel::draw_section_label(glm::vec2 pos, const char* text, float alpha)
{
    // Colored left pip
    m_ui.rect(pos, {3.f, 11.f}, {0.30f, 0.55f, 1.00f, 0.80f * alpha}, 1.f);
    m_ui.text(pos + glm::vec2(7.f, 0.f), text,
              {0.60f, 0.72f, 0.90f, 0.90f * alpha}, 10.f);
}

// ────────────────────────────────────────────────────────────────────────────
void InventoryPanel::draw_stats(Inventory& inv, glm::vec2 pos, float alpha)
{
    float w  = inv.total_weight_deep();
    float v  = inv.total_volume_deep();

    const float BAR_H  = 24.f;
    const float PAD    = 10.f;
    const float HALF_W = (PANEL_WIDTH - PAD * 3.f) * 0.5f;

    // Left pill: weight
    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << w << " kg";
        // Pill background
        m_ui.rect(pos + glm::vec2(PAD, 0.f), {HALF_W, BAR_H},
                  {0.07f, 0.09f, 0.14f, 0.88f * alpha}, 4.f);
        // Coloured left accent on pill
        m_ui.rect(pos + glm::vec2(PAD, 0.f), {3.f, BAR_H},
                  {0.35f, 0.60f, 1.00f, 0.75f * alpha}, 0.f);
        m_ui.text(pos + glm::vec2(PAD + 7.f, 5.f), ss.str(),
                  {0.65f, 0.80f, 1.0f, 0.92f * alpha}, 10.f);
    }
    // Right pill: volume
    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << v << " L";
        float rx = PAD * 2.f + HALF_W;
        m_ui.rect(pos + glm::vec2(rx, 0.f), {HALF_W, BAR_H},
                  {0.07f, 0.10f, 0.10f, 0.88f * alpha}, 4.f);
        m_ui.rect(pos + glm::vec2(rx, 0.f), {3.f, BAR_H},
                  {0.20f, 0.75f, 0.55f, 0.75f * alpha}, 0.f);
        m_ui.text(pos + glm::vec2(rx + 7.f, 5.f), ss.str(),
                  {0.50f, 0.90f, 0.75f, 0.92f * alpha}, 10.f);
    }
}

// ────────────────────────────────────────────────────────────────────────────
void InventoryPanel::draw_tooltip(const ItemStack& item, glm::vec2 cursor)
{
    if (!item.def) return;
    const float W = 200.f, PAD = 8.f, LINE = 15.f;

    // Clamp tooltip inside the framebuffer
    float fb_w = static_cast<float>(m_ui.fb_width());
    float fb_h = static_cast<float>(m_ui.fb_height());
    glm::vec2 tpos = cursor + glm::vec2(14.f, 14.f);

    int n_verbs  = static_cast<int>(item.def->verbs.size());
    // lines: name, optional custom_name, tags, weight/vol, condition, (sep+verbs)
    int n_lines  = 4 + (!item.custom_name.empty() ? 1 : 0)
                     + (item.def->tags.empty() ? 0 : 1)
                     + n_verbs
                     + (n_verbs > 0 ? 1 : 0); // separator
    float h = PAD * 2.f + static_cast<float>(n_lines) * LINE + 4.f;

    // Clamp position so tooltip stays on screen
    if (tpos.x + W > fb_w - 4.f) tpos.x = fb_w - W - 8.f;
    if (tpos.y + h > fb_h - 4.f) tpos.y = cursor.y - h - 8.f;

    // Background
    m_ui.rect(tpos, {W, h}, {0.05f, 0.05f, 0.09f, 0.97f}, 5.f);

    // Outer border (1px inset trick)
    m_ui.rect(tpos - glm::vec2(1.f), {W + 2.f, h + 2.f},
              {0.22f, 0.28f, 0.42f, 0.60f}, 6.f);
    m_ui.rect(tpos, {W, h}, {0.05f, 0.05f, 0.09f, 0.97f}, 5.f);

    // Left accent bar — colour = integrity health
    glm::vec4 integ_col;
    if (item.integrity >= 0.75f)
        integ_col = {0.20f, 0.90f, 0.38f, 0.95f};   // green
    else if (item.integrity >= 0.40f)
        integ_col = {0.95f, 0.70f, 0.08f, 0.95f};   // amber
    else
        integ_col = {0.95f, 0.22f, 0.10f, 0.95f};   // red
    m_ui.rect(tpos + glm::vec2(0.f, 5.f), {3.f, h - 10.f}, integ_col, 1.f);

    // Item icon preview (top-right)
    SDL_GPUTexture* tip_icon = m_ui.item_icon(item.def->id);
    constexpr float ICO = 42.f;
    if (tip_icon) {
        glm::vec2 ico_origin = {tpos.x + W - PAD - ICO, tpos.y + PAD};
        m_ui.rect(ico_origin - glm::vec2(2.f), {ICO + 4.f, ICO + 4.f},
                  {0.09f, 0.09f, 0.16f, 0.85f}, 4.f);
        m_ui.image(ico_origin, {ICO, ICO}, tip_icon, 0.92f);
    }

    const float text_max_x = tip_icon ? W - PAD - ICO - 8.f : W - PAD;

    float ty = tpos.y + PAD;
    // Item name (larger, white)
    m_ui.text({tpos.x + PAD + 4.f, ty}, item.def->name,
              {1.f, 1.f, 1.f, 1.f}, 12.f);
    ty += LINE + 1.f;

    // Custom name if set
    if (!item.custom_name.empty()) {
        m_ui.text({tpos.x + PAD + 4.f, ty},
                  "\"" + item.custom_name + "\"",
                  {0.75f, 0.88f, 1.f, 1.f}, 10.f);
        ty += LINE;
    }

    // Tags row
    if (!item.def->tags.empty()) {
        std::string tags_str;
        for (const auto& t : item.def->tags) tags_str += "[" + t + "]";
        m_ui.text({tpos.x + PAD + 4.f, ty}, tags_str,
                  {0.40f, 0.75f, 0.45f, 0.95f}, 9.f);
        ty += LINE;
    }

    // Weight / Volume
    {
        std::ostringstream wv;
        wv << std::fixed << std::setprecision(2)
           << item.def->weight << " kg  /  " << item.def->volume << " L";
        m_ui.text({tpos.x + PAD + 4.f, ty}, wv.str(),
                  {0.65f, 0.78f, 0.90f, 1.f}, 10.f);
        ty += LINE;
    }

    // Condition (using condition_label + raw %)
    {
        std::string cond = std::string(condition_label(item.integrity))
                         + "  ("
                         + std::to_string(static_cast<int>(item.integrity * 100.f))
                         + "%)";
        m_ui.text({tpos.x + PAD + 4.f, ty}, cond, integ_col, 10.f);
        ty += LINE + 2.f;
    }

    // Verbs — separated by a thin rule
    if (n_verbs > 0) {
        m_ui.rect({tpos.x + PAD + 4.f, ty}, {W - PAD * 2.f - 4.f, 1.f},
                  {0.22f, 0.28f, 0.42f, 0.55f}, 0.f);
        ty += 5.f;
        for (const auto& v : item.def->verbs) {
            m_ui.text({tpos.x + PAD + 4.f, ty},
                      v.name, {0.80f, 0.82f, 1.f, 0.88f}, 10.f);
            ty += LINE;
        }
    }

    (void)text_max_x;
}

// ────────────────────────────────────────────────────────────────────────────
void InventoryPanel::draw_drag_ghost(glm::vec2 cursor)
{
    if (!m_drag.active || !m_drag.dragged_item.def) return;
    const float GZ = SZ * 0.88f;
    glm::vec2 gpos = cursor - glm::vec2(GZ * 0.5f);

    // Shadow / border
    m_ui.rect(gpos - glm::vec2(2.f), {GZ + 4.f, GZ + 4.f},
              {0.f, 0.f, 0.f, 0.40f}, 6.f);
    // Bright border
    m_ui.rect(gpos - glm::vec2(1.f), {GZ + 2.f, GZ + 2.f},
              {0.45f, 0.65f, 0.95f, 0.70f}, 5.f);

    SDL_GPUTexture* icon_tex = m_ui.item_icon(m_drag.dragged_item.def->id);
    if (icon_tex) {
        m_ui.image(gpos, {GZ, GZ}, icon_tex, 0.82f);
    } else {
        m_ui.rect(gpos, {GZ, GZ}, {0.20f, 0.35f, 0.60f, 0.72f}, 4.f);
        std::string n = m_drag.dragged_item.def->name;
        if (n.size() > 7) n = n.substr(0, 6) + ".";
        m_ui.text(gpos + glm::vec2(3.f, GZ - 13.f), n, {1.f, 1.f, 1.f, 0.95f}, 9.f);
    }

    // Count badge (always shown when count > 1)
    if (m_drag.split_mode && m_drag.dragged_item.count > 1) {
        int half = m_drag.dragged_item.count / 2;
        std::string cnt = "x" + std::to_string(half);
        m_ui.rect(gpos - glm::vec2(0.f, 1.f), {float(cnt.size()) * 6.f + 4.f, 13.f},
                  {0.10f, 0.10f, 0.18f, 0.88f}, 2.f);
        m_ui.text(gpos + glm::vec2(2.f, 0.f), cnt,
                  {1.f, 1.f, 0.35f, 1.f}, 9.f);
    } else if (m_drag.dragged_item.count > 1) {
        std::string cnt = "x" + std::to_string(m_drag.dragged_item.count);
        m_ui.rect(gpos - glm::vec2(0.f, 1.f), {float(cnt.size()) * 6.f + 4.f, 13.f},
                  {0.10f, 0.10f, 0.18f, 0.88f}, 2.f);
        m_ui.text(gpos + glm::vec2(2.f, 0.f), cnt,
                  {0.88f, 1.f, 0.55f, 1.f}, 9.f);
    }
}

// ────────────────────────────────────────────────────────────────────────────
void InventoryPanel::draw_container_panel(Inventory& inv, InventorySlot& con_slot,
                                           glm::vec2 main_panel_tl, float panel_h,
                                           glm::vec2 cursor,
                                           float alpha)
{
    const float CON_W = CON_PANEL_W;
    glm::vec2 con_tl = { main_panel_tl.x - CON_W - 8.f, main_panel_tl.y };

    // Outer border frame
    m_ui.rect(con_tl - glm::vec2(1.f), {CON_W + 2.f, panel_h + 2.f},
              {0.14f, 0.30f, 0.20f, 0.50f * alpha}, 9.f);
    // Panel background
    m_ui.rect(con_tl, {CON_W, panel_h},
              {0.06f, 0.09f, 0.07f, 0.93f * alpha}, 8.f);

    // Left accent stripe (green — distinguishes container panel from main)
    m_ui.rect(con_tl, {3.f, panel_h}, {0.18f, 0.80f, 0.42f, 0.70f * alpha}, 0.f);

    // Top accent strip
    m_ui.rect(con_tl, {CON_W, 3.f}, {0.20f, 0.80f, 0.45f, 0.85f * alpha}, 0.f);

    // Header
    std::string header = "CONTENTS";
    if (con_slot.item && con_slot.item->def)
        header += ": " + con_slot.item->def->name;
    m_ui.text(con_tl + glm::vec2(10.f, 7.f), header,
              {0.50f, 1.f, 0.62f, alpha}, 12.f);

    // Close (×) button — top-right corner
    constexpr float BTN_SZ = 17.f;
    glm::vec2 btn_pos = con_tl + glm::vec2(CON_W - BTN_SZ - 6.f, 5.f);
    m_con_close_hovered = cursor.x >= btn_pos.x && cursor.x <= btn_pos.x + BTN_SZ &&
                          cursor.y >= btn_pos.y && cursor.y <= btn_pos.y + BTN_SZ;
    glm::vec4 btn_col = m_con_close_hovered
                        ? glm::vec4{0.85f, 0.22f, 0.18f, 0.97f}
                        : glm::vec4{0.30f, 0.16f, 0.14f, 0.88f};
    // Button border
    m_ui.rect(btn_pos - glm::vec2(1.f), {BTN_SZ + 2.f, BTN_SZ + 2.f},
              {0.55f, 0.20f, 0.18f, 0.60f}, 4.f);
    m_ui.rect(btn_pos, {BTN_SZ, BTN_SZ}, btn_col, 3.f);
    m_ui.text(btn_pos + glm::vec2(4.f, 2.f), "X", {1.f, 1.f, 1.f, alpha}, 10.f);

    // Header divider
    m_ui.rect(con_tl + glm::vec2(3.f, 26.f), {CON_W - 3.f, 1.f},
              {0.18f, 0.45f, 0.28f, 0.45f * alpha}, 0.f);

    // Volume bar
    if (con_slot.item && con_slot.item->def) {
        float used = 0.f;
        for (const auto& c : con_slot.children)
            if (c.item && c.item->def)
                used += c.item->def->volume * static_cast<float>(c.item->count);
        float cap   = con_slot.item->def->container_volume;
        float frac  = (cap > 0.f) ? std::min(used / cap, 1.f) : 0.f;
        float bar_w = CON_W - 20.f;

        // Volume bar label
        std::ostringstream vs;
        vs << std::fixed << std::setprecision(1) << used << " / " << cap << " L";
        m_ui.text(con_tl + glm::vec2(10.f, 30.f), vs.str(),
                  {0.55f, 0.80f, 0.65f, 0.88f * alpha}, 9.f);

        // Bar track + fill
        m_ui.rect(con_tl + glm::vec2(10.f, 42.f), {bar_w, 5.f},
                  {0.12f, 0.14f, 0.12f, 0.85f * alpha}, 2.f);
        glm::vec4 bar_col = frac < 0.85f
                            ? glm::vec4{0.18f, 0.82f, 0.38f, 0.92f}
                            : glm::vec4{0.92f, 0.42f, 0.12f, 0.92f};
        if (frac > 0.f)
            m_ui.rect(con_tl + glm::vec2(10.f, 42.f), {bar_w * frac, 5.f}, bar_col, 2.f);
    }

    // Slot grid — 4 columns (start below volume bar)
    const float START_Y = 54.f;
    const float STEP    = SZ + GAP;
    const int   COLS    = 4;
    int idx = 0;
    for (auto& child : con_slot.children) {
        if (idx >= static_cast<int>(con_slot.children.size())) break;
        int col = idx % COLS;
        int row = idx / COLS;
        glm::vec2 spos = con_tl + glm::vec2(10.f + col * STEP, START_Y + row * STEP);

        InventorySlot* cp = &child;
        std::string lbl = std::to_string(idx + 1);
        bool dragging_from = m_drag.active && cp->id == m_drag.src_slot;
        bool hov = draw_slot(dragging_from ? nullptr : cp, lbl.c_str(),
                             spos, {SZ, SZ}, false, cursor);
        if (hov) m_hovered_slot = child.id;
        ++idx;
    }

    // Footer: weight of contents
    if (con_slot.item && con_slot.item->def) {
        float item_w = 0.f;
        for (const auto& c : con_slot.children)
            if (c.item && c.item->def)
                item_w += c.item->def->weight * static_cast<float>(c.item->count);
        std::ostringstream ws;
        ws << std::fixed << std::setprecision(2)
           << "Contents: " << item_w << " kg";
        m_ui.rect(con_tl + glm::vec2(3.f, panel_h - 34.f), {CON_W - 3.f, 24.f},
                  {0.05f, 0.08f, 0.06f, 0.85f * alpha}, 0.f);
        // Green left pip on footer
        m_ui.rect(con_tl + glm::vec2(3.f, panel_h - 34.f), {3.f, 24.f},
                  {0.18f, 0.75f, 0.40f, 0.65f * alpha}, 0.f);
        m_ui.text(con_tl + glm::vec2(12.f, panel_h - 29.f), ws.str(),
                  {0.60f, 0.82f, 0.68f, 0.92f * alpha}, 10.f);
    }

    (void)inv;
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
