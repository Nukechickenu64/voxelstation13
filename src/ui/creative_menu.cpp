#include "ui/creative_menu.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <string_view>

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

    m_open          = true;
    m_scroll_offset = 0.f;
    m_tab           = 0;
    m_item_category = ItemCategory::All;
}

// ─────────────────────────────────────────────────────────────────────────────
// Returns the subset of m_items that matches the active ItemCategory filter,
// using the type_path classification system (istype checks).
std::vector<const ItemDef*> CreativeMenu::filtered_items() const
{
    if (m_item_category == ItemCategory::All)
        return m_items;

    std::vector<const ItemDef*> out;
    out.reserve(m_items.size());

    for (const ItemDef* def : m_items) {
        bool match = false;
        switch (m_item_category) {
            case ItemCategory::Tools:
                match = istype(*def, "/obj/item/tool");
                break;
            case ItemCategory::Equipment:
                match = istype(*def, "/obj/item/clothing");
                break;
            case ItemCategory::Storage:
                match = istype(*def, "/obj/item/storage");
                break;
            case ItemCategory::Medical:
                match = istype(*def, "/obj/item/medical");
                break;
            case ItemCategory::Weapons:
                match = istype(*def, "/obj/item/weapon");
                break;
            case ItemCategory::Misc:
                // Misc = stack types + anything that isn't one of the above named categories
                match = istype(*def, "/obj/item/stack") ||
                        (!istype(*def, "/obj/item/tool")     &&
                         !istype(*def, "/obj/item/clothing") &&
                         !istype(*def, "/obj/item/storage")  &&
                         !istype(*def, "/obj/item/medical")  &&
                         !istype(*def, "/obj/item/weapon"));
                break;
            default:
                match = true;
                break;
        }
        if (match) out.push_back(def);
    }
    return out;
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
// Category filter bar — rendered below the main tab bar when in the Items tab.
void CreativeMenu::draw_category_bar(glm::vec2 origin, float width, float alpha)
{
    struct CatEntry { ItemCategory cat; const char* label; float w; };
    static const CatEntry cats[] = {
        { ItemCategory::All,       "  All  ",       60.f },
        { ItemCategory::Tools,     " Tools ",       64.f },
        { ItemCategory::Equipment, " Equipment ",  100.f },
        { ItemCategory::Storage,   " Storage ",     78.f },
        { ItemCategory::Medical,   " Medical ",     78.f },
        { ItemCategory::Weapons,   " Weapons ",     78.f },
        { ItemCategory::Misc,      " Misc ",        56.f },
    };

    float x = origin.x;
    for (const auto& entry : cats) {
        bool active = (m_item_category == entry.cat);
        glm::vec4 bg = active
            ? glm::vec4{0.25f, 0.45f, 0.30f, 0.95f * alpha}
            : glm::vec4{0.08f, 0.10f, 0.15f, 0.75f * alpha};
        m_ui.rect({x, origin.y}, {entry.w, CATEGORY_H}, bg, 4.f);
        m_ui.text({x + 5.f, origin.y + 7.f}, entry.label,
                  {0.8f, 0.95f, 0.85f, alpha}, 10.f);
        x += entry.w + 4.f;
        if (x > origin.x + width) break;
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

    // Category filter bar (Items tab only)
    float cat_bar_extra = 0.f;
    if (m_tab == 0) {
        const glm::vec2 cat_origin = panel_tl + glm::vec2(14.f, HEADER_H + TAB_H + 6.f);
        draw_category_bar(cat_origin, panel_w - 28.f, 1.f);
        cat_bar_extra = CATEGORY_H + 6.f;

        // Handle category click
        if (lmb_pressed) {
            struct CatEntry { ItemCategory cat; float w; };
            static const CatEntry cats[] = {
                { ItemCategory::All,       60.f },
                { ItemCategory::Tools,     64.f },
                { ItemCategory::Equipment,100.f },
                { ItemCategory::Storage,   78.f },
                { ItemCategory::Medical,   78.f },
                { ItemCategory::Weapons,   78.f },
                { ItemCategory::Misc,      56.f },
            };
            float cx = cat_origin.x;
            for (const auto& entry : cats) {
                bool hit = cursor.x >= cx && cursor.x <= cx + entry.w &&
                           cursor.y >= cat_origin.y && cursor.y <= cat_origin.y + CATEGORY_H;
                if (hit) {
                    m_item_category = entry.cat;
                    m_scroll_offset = 0.f;
                    break;
                }
                cx += entry.w + 4.f;
                if (cx > cat_origin.x + panel_w - 28.f) break;
            }
        }
    }

    // Grid area — starts below tab bar + optional category bar
    const float GRID_Y0 = panel_tl.y + HEADER_H + TAB_H + 8.f + cat_bar_extra;
    const float GRID_H  = panel_tl.y + panel_h - GRID_Y0 - 10.f;
    const float STEP    = CELL_SIZE + CELL_PAD;
    const int   cols    = COLS;

    // Scissor (simulate by clamping drawing to grid bounds)
    // We don't have a real scissor API, so just skip cells outside range.

    // Build the active list for this frame
    std::vector<const ItemDef*> active_items;
    if (m_tab == 0) active_items = filtered_items();

    // Scroll
    const int  count = (m_tab == 0) ? static_cast<int>(active_items.size())
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

        // Icon: use loaded item icon when available; fall back to a tinted rect.
        if (m_tab == 0) {
            SDL_GPUTexture* icon_tex = m_ui.item_icon(active_items[i]->id);
            if (icon_tex) {
                m_ui.image(cpos + glm::vec2(8.f, 8.f),
                           {CELL_SIZE - 16.f, CELL_SIZE - 26.f},
                           icon_tex, 1.f);
            } else {
                m_ui.rect(cpos + glm::vec2(8.f, 8.f), {CELL_SIZE - 16.f, CELL_SIZE - 26.f},
                          {0.18f, 0.28f, 0.45f, 0.75f}, 3.f);
            }
        } else {
            // Voxel types: no per-type icon atlas yet — placeholder rect.
            m_ui.rect(cpos + glm::vec2(8.f, 8.f), {CELL_SIZE - 16.f, CELL_SIZE - 26.f},
                      {0.18f, 0.28f, 0.45f, 0.75f}, 3.f);
        }

        // Name label
        std::string name;
        if (m_tab == 0) {
            name = active_items[i]->name;
        } else {
            name = m_voxels[i].second->name;
        }
        if (name.size() > 9) name = name.substr(0, 8) + ".";
        m_ui.text(cpos + glm::vec2(4.f, CELL_SIZE - 16.f), name,
                  {0.9f, 0.95f, 1.f, 0.95f}, 9.f);

        // Click
        if (lmb_pressed && hov) {
            if (m_tab == 0) {
                result.give_item = active_items[i];
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
        if (m_tab == 0 && m_item_category != ItemCategory::All) {
            ss << count << " / " << static_cast<int>(m_items.size()) << " items";
        } else {
            ss << count << " entries";
        }
        m_ui.text(panel_tl + glm::vec2(panel_w - 100.f, 12.f), ss.str(),
                  {0.5f, 0.6f, 0.75f, 0.8f}, 10.f);
    }

    return result;
}
