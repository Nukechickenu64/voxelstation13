#include "ui/creative_menu.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
static constexpr float PANEL_MARGIN = 60.f;   // from screen edges
static constexpr float TAB_H        = 32.f;
static constexpr float HEADER_H     = 44.f;   // title + search bar placeholder

// ─────────────────────────────────────────────────────────────────────────────
CreativeMenu::CreativeMenu(UIRenderer& ui)
    : m_ui(ui)
{}

// ─────────────────────────────────────────────────────────────────────────────
void CreativeMenu::open(const ItemRegistry& items, const VoxelRegistry& voxels)
{
    // Build sorted item list
    m_items.clear();
    for (const auto& [id, def] : items.all())
        m_items.push_back(&def);
    std::sort(m_items.begin(), m_items.end(),
              [](const ItemDef* a, const ItemDef* b){ return a->name < b->name; });

    // Build sorted voxel list (skip air)
    m_voxels.clear();
    for (const auto& [id, def] : voxels.all()) {
        if (def.id == "air") continue;
        m_voxels.emplace_back(id, &def);
    }
    std::sort(m_voxels.begin(), m_voxels.end(),
              [](const auto& a, const auto& b){ return a.second->name < b.second->name; });

    m_open         = true;
    m_scroll_offset= 0.f;
    m_tab          = 0;
}

void CreativeMenu::close()
{
    m_open = false;
}

// ─────────────────────────────────────────────────────────────────────────────
void CreativeMenu::draw_tab_bar(glm::vec2 origin, float width, float alpha)
{
    const char* tabs[] = { "  ITEMS  ", "  STRUCTURES  " };
    float x = origin.x;
    for (int i = 0; i < 2; ++i) {
        float tw = (i == 0) ? 100.f : 140.f;
        glm::vec4 bg = (m_tab == i)
                       ? glm::vec4{0.2f, 0.4f, 0.7f, 0.95f * alpha}
                       : glm::vec4{0.1f, 0.12f, 0.18f, 0.8f * alpha};
        m_ui.rect({x, origin.y}, {tw, TAB_H}, bg, 4.f);
        m_ui.text({x + 8.f, origin.y + 9.f}, tabs[i],
                  {0.9f, 0.95f, 1.f, alpha}, 12.f);
        x += tw + 4.f;
    }
    (void)width;
}

// ─────────────────────────────────────────────────────────────────────────────
CreativeResult CreativeMenu::draw(glm::vec2 cursor, bool lmb_pressed,
                                   float scroll_y, bool escape_pressed)
{
    CreativeResult result;
    if (!m_open) return result;

    if (escape_pressed) { close(); return result; }

    const float fb_w = static_cast<float>(m_ui.fb_width());
    const float fb_h = static_cast<float>(m_ui.fb_height());

    const glm::vec2 panel_tl = { PANEL_MARGIN, PANEL_MARGIN };
    const float     panel_w  = fb_w - PANEL_MARGIN * 2.f;
    const float     panel_h  = fb_h - PANEL_MARGIN * 2.f;

    // Full dim backdrop
    m_ui.rect({0.f, 0.f}, {fb_w, fb_h}, {0.f, 0.f, 0.f, 0.55f}, 0.f);

    // Main panel
    m_ui.rect(panel_tl, {panel_w, panel_h}, {0.06f, 0.07f, 0.11f, 0.97f}, 10.f);

    // Title
    m_ui.text(panel_tl + glm::vec2(14.f, 10.f), "CREATIVE MENU   [F2 / ESC to close]",
              {0.6f, 0.75f, 1.f, 1.f}, 14.f);

    // Tabs
    const glm::vec2 tabs_origin = panel_tl + glm::vec2(14.f, HEADER_H);
    draw_tab_bar(tabs_origin, panel_w - 28.f, 1.f);

    // Tab switch on click
    {
        float tx = tabs_origin.x;
        float tw0 = 100.f, tw1 = 140.f;
        bool in_tab0 = cursor.x >= tx && cursor.x <= tx + tw0 &&
                       cursor.y >= tabs_origin.y && cursor.y <= tabs_origin.y + TAB_H;
        bool in_tab1 = cursor.x >= tx + tw0 + 4.f && cursor.x <= tx + tw0 + 4.f + tw1 &&
                       cursor.y >= tabs_origin.y && cursor.y <= tabs_origin.y + TAB_H;
        if (lmb_pressed) {
            if (in_tab0) { m_tab = 0; m_scroll_offset = 0.f; }
            if (in_tab1) { m_tab = 1; m_scroll_offset = 0.f; }
        }
    }

    // Grid area
    const float GRID_Y0 = panel_tl.y + HEADER_H + TAB_H + 8.f;
    const float GRID_H  = panel_tl.y + panel_h - GRID_Y0 - 10.f;
    const float STEP    = CELL_SIZE + CELL_PAD;
    const int   cols    = COLS;

    // Scissor (simulate by clamping drawing to grid bounds)
    // We don't have a real scissor API, so just skip cells outside range.

    // Scroll
    const int  count = (m_tab == 0) ? static_cast<int>(m_items.size())
                                    : static_cast<int>(m_voxels.size());
    const int  rows  = (count + cols - 1) / cols;
    const float total_h = static_cast<float>(rows) * STEP;

    m_scroll_offset -= scroll_y * (STEP * 1.5f);
    m_scroll_offset  = std::max(0.f, std::min(m_scroll_offset,
                                              std::max(0.f, total_h - GRID_H)));

    // Draw cells
    const float GX0 = panel_tl.x + 14.f;

    for (int i = 0; i < count; ++i) {
        int col  = i % cols;
        int row  = i / cols;
        float cx = GX0 + static_cast<float>(col) * STEP;
        float cy = GRID_Y0 + static_cast<float>(row) * STEP - m_scroll_offset;

        // Cull: fully outside grid
        if (cy + CELL_SIZE < GRID_Y0 || cy > GRID_Y0 + GRID_H) continue;

        glm::vec2 cpos = {cx, cy};
        bool hov = cursor.x >= cx && cursor.x <= cx + CELL_SIZE &&
                   cursor.y >= cy && cursor.y <= cy + CELL_SIZE;

        glm::vec4 bg = hov ? glm::vec4{0.25f, 0.4f, 0.65f, 0.95f}
                           : glm::vec4{0.10f, 0.12f, 0.18f, 0.85f};
        m_ui.rect(cpos, {CELL_SIZE, CELL_SIZE}, bg, 5.f);
        m_ui.rect(cpos, {CELL_SIZE, CELL_SIZE}, {0.2f, 0.25f, 0.4f, 0.4f}, 5.f);

        // Icon placeholder
        m_ui.rect(cpos + glm::vec2(8.f, 8.f), {CELL_SIZE - 16.f, CELL_SIZE - 26.f},
                  {0.18f, 0.28f, 0.45f, 0.75f}, 3.f);

        // Name label
        std::string name;
        if (m_tab == 0) {
            name = m_items[i]->name;
        } else {
            name = m_voxels[i].second->name;
        }
        if (name.size() > 9) name = name.substr(0, 8) + ".";
        m_ui.text(cpos + glm::vec2(4.f, CELL_SIZE - 16.f), name,
                  {0.9f, 0.95f, 1.f, 0.95f}, 9.f);

        // Click
        if (lmb_pressed && hov) {
            if (m_tab == 0) {
                result.give_item = m_items[i];
            } else {
                result.place_voxel = m_voxels[i].first;
            }
        }
    }

    // Scroll indicator
    if (total_h > GRID_H && total_h > 0.f) {
        float scroll_track_h = GRID_H - 20.f;
        float thumb_h        = std::max(20.f, scroll_track_h * (GRID_H / total_h));
        float thumb_y        = GRID_Y0 + (m_scroll_offset / (total_h - GRID_H))
                               * (scroll_track_h - thumb_h);
        float sx = panel_tl.x + panel_w - 14.f;
        m_ui.rect({sx, GRID_Y0}, {6.f, scroll_track_h},
                  {0.15f, 0.15f, 0.2f, 0.6f}, 3.f);
        m_ui.rect({sx, thumb_y}, {6.f, thumb_h},
                  {0.4f, 0.55f, 0.85f, 0.85f}, 3.f);
    }

    // Row/count indicator
    {
        std::ostringstream ss;
        ss << count << " entries";
        m_ui.text(panel_tl + glm::vec2(panel_w - 90.f, 12.f), ss.str(),
                  {0.5f, 0.6f, 0.75f, 0.8f}, 10.f);
    }

    return result;
}
