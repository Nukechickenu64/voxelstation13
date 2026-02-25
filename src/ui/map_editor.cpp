#include "ui/map_editor.h"
#include "data/map_io.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
MapEditor::MapEditor(UIRenderer& ui, World& world, VoxelRegistry& voxels)
    : m_ui(ui), m_world(world), m_voxel_reg(voxels)
{}

// ─────────────────────────────────────────────────────────────────────────────
void MapEditor::open(glm::vec3 player_pos)
{
    // Centre the view on the player's X/Z position, at the floor layer.
    m_pan   = {player_pos.x, player_pos.z};
    m_layer = static_cast<int>(std::floor(player_pos.y));
    m_zoom  = 20.f;
    m_palette_scroll = 0.f;
    m_panning = false;
    m_status_msg.clear();
    m_status_timer = 0.f;

    // Build sorted palette (skip air type_id=0)
    m_palette.clear();
    for (const auto& [id, def] : m_voxel_reg.all()) {
        if (def.type_id == 0 || def.id == "air") continue;
        m_palette.emplace_back(id, &def);
    }
    std::sort(m_palette.begin(), m_palette.end(),
              [](const auto& a, const auto& b){ return a.second->name < b.second->name; });

    // Default selection: first entry
    if (!m_palette.empty())
        m_selected_id = m_palette.front().first;

    m_open = true;
}

void MapEditor::close()
{
    m_open    = false;
    m_panning = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Coordinate helpers
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int INT_MIN_SENTINEL = INT_MIN;

// Grid area: x=[PAL_W, fb_w], y=[TOP_H, fb_h-BOT_H]
// Centre of grid area on screen:
//   gcx = PAL_W + (fb_w - PAL_W) * 0.5f
//   gcy = TOP_H + (fb_h - TOP_H - BOT_H) * 0.5f
// Cell (wx, wz) top-left screen position:
//   sx = gcx + (wx - m_pan.x) * m_zoom
//   sy = gcy + (wz - m_pan.y) * m_zoom

glm::vec2 MapEditor::world_to_screen(float wx, float wz) const
{
    const float fb_w = static_cast<float>(m_ui.fb_width());
    const float fb_h = static_cast<float>(m_ui.fb_height());
    const float gcx  = PAL_W + (fb_w - PAL_W) * 0.5f;
    const float gcy  = TOP_H + (fb_h - TOP_H - BOT_H) * 0.5f;
    return { gcx + (wx - m_pan.x) * m_zoom,
             gcy + (wz - m_pan.y) * m_zoom };
}

glm::vec2 MapEditor::screen_to_world(glm::vec2 s) const
{
    const float fb_w = static_cast<float>(m_ui.fb_width());
    const float fb_h = static_cast<float>(m_ui.fb_height());
    const float gcx  = PAL_W + (fb_w - PAL_W) * 0.5f;
    const float gcy  = TOP_H + (fb_h - TOP_H - BOT_H) * 0.5f;
    return { (s.x - gcx) / m_zoom + m_pan.x,
             (s.y - gcy) / m_zoom + m_pan.y };
}

glm::ivec2 MapEditor::hovered_cell(glm::vec2 cursor) const
{
    const float fb_w = static_cast<float>(m_ui.fb_width());
    const float fb_h = static_cast<float>(m_ui.fb_height());
    if (cursor.x < PAL_W || cursor.x >= fb_w)              return {INT_MIN_SENTINEL, INT_MIN_SENTINEL};
    if (cursor.y < TOP_H  || cursor.y >= fb_h - BOT_H)     return {INT_MIN_SENTINEL, INT_MIN_SENTINEL};
    glm::vec2 w = screen_to_world(cursor);
    return { static_cast<int>(std::floor(w.x)),
             static_cast<int>(std::floor(w.y)) };
}

// ─────────────────────────────────────────────────────────────────────────────
// Voxel colour lookup
// ─────────────────────────────────────────────────────────────────────────────
glm::vec4 MapEditor::voxel_color(uint16_t type_id) const
{
    if (type_id == 0) return {0.06f, 0.07f, 0.09f, 1.f};  // air — very dark

    const VoxelTypeDef* def = m_voxel_reg.get(type_id);
    if (!def) return {0.55f, 0.0f, 0.55f, 1.f};  // unknown — purple

    const std::string& id = def->id;

    // ── Known-type colour map ─────────────────────────────────────────────────
    if (id == "floor")              return {0.50f, 0.50f, 0.53f, 1.f};
    if (id == "floor_plating")      return {0.40f, 0.40f, 0.45f, 1.f};
    if (id == "reinforced_wall"
     || id == "wall")               return {0.22f, 0.24f, 0.28f, 1.f};
    if (id.find("door") != id.npos) return {0.62f, 0.42f, 0.18f, 1.f};
    if (id.find("window") != id.npos
     || id.find("glass") != id.npos)return {0.40f, 0.65f, 0.82f, 0.9f};
    if (id == "catwalk")            return {0.68f, 0.58f, 0.22f, 1.f};
    if (id.find("grille") != id.npos) return {0.35f, 0.40f, 0.45f, 1.f};

    // ── Hash-derived colour for anything else ─────────────────────────────────
    std::size_t h = std::hash<std::string>{}(id);
    float r = 0.30f + 0.50f * static_cast<float>( h        & 0xFF) / 255.f;
    float g = 0.30f + 0.50f * static_cast<float>((h >>  8) & 0xFF) / 255.f;
    float b = 0.30f + 0.50f * static_cast<float>((h >> 16) & 0xFF) / 255.f;
    return {r, g, b, 1.f};
}

// ─────────────────────────────────────────────────────────────────────────────
// Top bar
// ─────────────────────────────────────────────────────────────────────────────
void MapEditor::draw_top_bar()
{
    const float fb_w = static_cast<float>(m_ui.fb_width());

    m_ui.rect({0.f, 0.f}, {fb_w, TOP_H},
              {0.07f, 0.09f, 0.14f, 0.97f});

    m_ui.text({10.f, 8.f}, "MAP EDITOR",
              {0.55f, 0.80f, 1.00f, 1.f}, 15.f);

    // Layer indicator
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "Layer Y = %d   [PgUp/PgDn]", m_layer);
        m_ui.text({160.f, 9.f}, buf, {0.85f, 0.92f, 1.f, 0.9f}, 13.f);
    }

    // Zoom indicator
    {
        char zbuf[32];
        std::snprintf(zbuf, sizeof(zbuf), "Zoom: %.0f px", m_zoom);
        m_ui.text({fb_w - 180.f, 9.f}, zbuf, {0.7f, 0.8f, 0.9f, 0.85f}, 12.f);
    }

    // Ctrl+S / Ctrl+L hints
    m_ui.text({fb_w - 370.f, 9.f}, "Ctrl+S Save  Ctrl+L Load  F7/ESC Close",
              {0.5f, 0.6f, 0.75f, 0.75f}, 11.f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Bottom bar
// ─────────────────────────────────────────────────────────────────────────────
void MapEditor::draw_bottom_bar(glm::vec2 cursor_world)
{
    const float fb_w = static_cast<float>(m_ui.fb_width());
    const float fb_h = static_cast<float>(m_ui.fb_height());
    const float by   = fb_h - BOT_H;

    m_ui.rect({0.f, by}, {fb_w, BOT_H},
              {0.07f, 0.09f, 0.14f, 0.97f});

    // Cursor world coords
    {
        char cbuf[64];
        std::snprintf(cbuf, sizeof(cbuf),
                      "Cursor: (%.1f, %d, %.1f)",
                      cursor_world.x, m_layer, cursor_world.y);
        m_ui.text({10.f, by + 7.f}, cbuf, {0.75f, 0.85f, 1.f, 0.9f}, 12.f);
    }

    // Status message (save/load feedback)
    if (m_status_timer > 0.f) {
        m_status_timer -= 1.f / 60.f;  // approximate 60 fps decay
        glm::vec4 col = (m_status_msg.rfind("[OK]", 0) == 0)
                        ? glm::vec4{0.3f, 0.9f, 0.4f, 1.f}
                        : glm::vec4{0.95f, 0.4f, 0.4f, 1.f};
        m_ui.text({fb_w * 0.4f, by + 7.f}, m_status_msg, col, 12.f);
    }

    // Map path
    m_ui.text({fb_w - 340.f, by + 7.f}, m_map_path,
              {0.45f, 0.55f, 0.7f, 0.75f}, 11.f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Palette panel (left side)
// ─────────────────────────────────────────────────────────────────────────────
void MapEditor::draw_palette(glm::vec2 cursor, bool lmb_pressed)
{
    const float fb_h      = static_cast<float>(m_ui.fb_height());
    const float panel_h   = fb_h - TOP_H - BOT_H;
    const float panel_y0  = TOP_H;

    // Background
    m_ui.rect({0.f, panel_y0}, {PAL_W, panel_h},
              {0.08f, 0.10f, 0.15f, 0.98f});
    // Right border
    m_ui.rect({PAL_W - 1.f, panel_y0}, {1.f, panel_h},
              {0.2f, 0.3f, 0.5f, 0.6f});

    // Title
    m_ui.text({PAL_MARGIN, panel_y0 + 6.f}, "PALETTE",
              {0.55f, 0.75f, 1.f, 0.9f}, 12.f);

    // "Erase" item always at top
    {
        const float iy = panel_y0 + 24.f - m_palette_scroll;
        if (iy >= panel_y0 && iy < panel_y0 + panel_h - PAL_ITEM_H) {
            bool sel = (m_selected_id == 0);
            bool hov = cursor.x >= 0.f && cursor.x < PAL_W
                    && cursor.y >= iy  && cursor.y < iy + PAL_ITEM_H;
            glm::vec4 bg = sel  ? glm::vec4{0.18f, 0.35f, 0.60f, 1.f}
                         : hov  ? glm::vec4{0.15f, 0.18f, 0.26f, 1.f}
                                : glm::vec4{0.09f, 0.11f, 0.17f, 0.85f};
            m_ui.rect({PAL_MARGIN, iy}, {PAL_W - PAL_MARGIN * 2.f, PAL_ITEM_H - 2.f}, bg, 3.f);
            // Eraser swatch: cross pattern represented as an 'X' tinted red
            m_ui.rect({PAL_MARGIN + 3.f, iy + 4.f}, {PAL_ITEM_H - 10.f, PAL_ITEM_H - 10.f},
                      {0.65f, 0.25f, 0.25f, 0.75f}, 2.f);
            m_ui.text({PAL_MARGIN + PAL_ITEM_H, iy + 5.f}, "Erase (Air)",
                      {0.9f, 0.6f, 0.6f, 1.f}, 11.f);
            if (hov && lmb_pressed) m_selected_id = 0;
        }
    }

    // Voxel type entries
    const float item_y0 = panel_y0 + 24.f + PAL_ITEM_H + 2.f;
    const int count = static_cast<int>(m_palette.size());
    for (int i = 0; i < count; ++i) {
        const float iy = item_y0 + static_cast<float>(i) * (PAL_ITEM_H + 2.f) - m_palette_scroll;
        if (iy + PAL_ITEM_H < panel_y0) continue;
        if (iy > panel_y0 + panel_h)    break;

        uint16_t  tid  = m_palette[i].first;
        const auto* def = m_palette[i].second;

        bool sel = (m_selected_id == tid);
        bool hov = cursor.x >= 0.f && cursor.x < PAL_W
                && cursor.y >= iy  && cursor.y < iy + PAL_ITEM_H;

        glm::vec4 bg = sel  ? glm::vec4{0.18f, 0.35f, 0.60f, 1.f}
                     : hov  ? glm::vec4{0.15f, 0.18f, 0.26f, 1.f}
                            : glm::vec4{0.09f, 0.11f, 0.17f, 0.85f};

        m_ui.rect({PAL_MARGIN, iy}, {PAL_W - PAL_MARGIN * 2.f, PAL_ITEM_H - 2.f}, bg, 3.f);

        // Colour swatch
        m_ui.rect({PAL_MARGIN + 3.f, iy + 4.f},
                  {PAL_ITEM_H - 10.f, PAL_ITEM_H - 10.f},
                  voxel_color(tid), 2.f);

        // Name label
        std::string name = def->name;
        if (name.size() > 14) name = name.substr(0, 13) + ".";
        m_ui.text({PAL_MARGIN + PAL_ITEM_H, iy + 5.f}, name,
                  sel ? glm::vec4{1.f, 1.f, 1.f, 1.f} : glm::vec4{0.82f, 0.88f, 0.98f, 0.9f},
                  11.f);

        if (hov && lmb_pressed)
            m_selected_id = tid;
    }

    // Scroll bar if needed
    const float total_items_h = static_cast<float>(count + 1) * (PAL_ITEM_H + 2.f) + 4.f;
    const float visible_h     = panel_h - 24.f;
    if (total_items_h > visible_h) {
        float track_h = visible_h - 4.f;
        float thumb_h = std::max(16.f, track_h * (visible_h / total_items_h));
        float max_scroll = total_items_h - visible_h;
        float ratio = (max_scroll > 0.f) ? (m_palette_scroll / max_scroll) : 0.f;
        float thumb_y = panel_y0 + 24.f + ratio * (track_h - thumb_h);
        m_ui.rect({PAL_W - 7.f, panel_y0 + 24.f}, {4.f, track_h},
                  {0.14f, 0.16f, 0.22f, 0.8f}, 2.f);
        m_ui.rect({PAL_W - 7.f, thumb_y}, {4.f, thumb_h},
                  {0.35f, 0.50f, 0.80f, 0.85f}, 2.f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Main grid
// ─────────────────────────────────────────────────────────────────────────────
bool MapEditor::draw_grid(glm::vec2 cursor, bool lmb_held, bool rmb_held)
{
    const float fb_w = static_cast<float>(m_ui.fb_width());
    const float fb_h = static_cast<float>(m_ui.fb_height());

    const float gx0  = PAL_W;
    const float gy0  = TOP_H;
    const float gx1  = fb_w;
    const float gy1  = fb_h - BOT_H;

    // Grid background
    m_ui.rect({gx0, gy0}, {gx1 - gx0, gy1 - gy0},
              {0.05f, 0.06f, 0.08f, 1.f});

    // Compute visible world cell range
    glm::vec2 top_left  = screen_to_world({gx0, gy0});
    glm::vec2 bot_right = screen_to_world({gx1, gy1});

    int wx_min = static_cast<int>(std::floor(top_left.x))  - 1;
    int wx_max = static_cast<int>(std::ceil (bot_right.x)) + 1;
    int wz_min = static_cast<int>(std::floor(top_left.y))  - 1;
    int wz_max = static_cast<int>(std::ceil (bot_right.y)) + 1;

    const float gap  = (m_zoom >= 10.f) ? 1.f : 0.f;
    const float cdsz = m_zoom - gap;  // cell draw size

    // Hover cell
    glm::ivec2 hcell = hovered_cell(cursor);
    bool cursor_in_grid = (hcell.x != INT_MIN_SENTINEL);

    bool world_modified = false;

    // ── Draw cells ─────────────────────────────────────────────────────────
    int draw_budget = 14000;  // stay safely under k_max_verts/4=16384
    for (int wz = wz_min; wz <= wz_max && draw_budget > 0; ++wz) {
        for (int wx = wx_min; wx <= wx_max && draw_budget > 0; ++wx, --draw_budget) {
            glm::vec2 sc = world_to_screen(static_cast<float>(wx),
                                           static_cast<float>(wz));
            // Clip to grid area
            if (sc.x + cdsz < gx0 || sc.x > gx1) continue;
            if (sc.y + cdsz < gy0 || sc.y > gy1) continue;

            Voxel v      = m_world.get_voxel({wx, m_layer, wz});
            glm::vec4 col = voxel_color(v.type_id);

            bool hover = cursor_in_grid && (wx == hcell.x) && (wz == hcell.y);
            if (hover) {
                col.r = std::min(1.f, col.r + 0.18f);
                col.g = std::min(1.f, col.g + 0.18f);
                col.b = std::min(1.f, col.b + 0.18f);
            }

            m_ui.rect(sc, {cdsz, cdsz}, col);
        }
    }

    // ── Origin cross-hair (world 0,0) ───────────────────────────────────────
    {
        glm::vec2 orig = world_to_screen(0.f, 0.f);
        const float arm = 8.f;
        m_ui.line(orig + glm::vec2{-arm, 0.f}, orig + glm::vec2{ arm, 0.f},
                  {0.9f, 0.2f, 0.2f, 0.7f}, 1.5f);
        m_ui.line(orig + glm::vec2{0.f, -arm}, orig + glm::vec2{0.f,  arm},
                  {0.9f, 0.2f, 0.2f, 0.7f}, 1.5f);
    }

    // ── Paint / erase on click ──────────────────────────────────────────────
    if (cursor_in_grid) {
        if (lmb_held) {
            const VoxelTypeDef* def = (m_selected_id != 0)
                                      ? m_voxel_reg.get(m_selected_id)
                                      : nullptr;
            Voxel nv;
            nv.type_id = m_selected_id;
            nv.flags   = def ? def->default_flags
                             : static_cast<uint16_t>(VFLAG_SOLID | VFLAG_OPAQUE);
            Voxel cur_v = m_world.get_voxel({hcell.x, m_layer, hcell.y});
            if (cur_v.type_id != m_selected_id) {
                m_world.set_voxel({hcell.x, m_layer, hcell.y}, nv);
                world_modified = true;
            }
        }
        if (rmb_held) {
            Voxel cur_v = m_world.get_voxel({hcell.x, m_layer, hcell.y});
            if (cur_v.type_id != 0) {
                m_world.set_voxel({hcell.x, m_layer, hcell.y}, Voxel{});
                world_modified = true;
            }
        }
    }

    // ── Hover tooltip (voxel type name) ────────────────────────────────────
    if (cursor_in_grid) {
        Voxel hv = m_world.get_voxel({hcell.x, m_layer, hcell.y});
        const VoxelTypeDef* hvd = m_voxel_reg.get(hv.type_id);
        std::string tip = hvd ? hvd->name : (hv.type_id == 0 ? "Air" : "?");
        char tipbuf[64];
        std::snprintf(tipbuf, sizeof(tipbuf), "(%d,%d,%d)  %s",
                      hcell.x, m_layer, hcell.y, tip.c_str());
        float tw = static_cast<float>(strlen(tipbuf)) * 7.f + 10.f;
        glm::vec2 tp = cursor + glm::vec2(14.f, 6.f);
        // Clamp to screen
        if (tp.x + tw > fb_w) tp.x = fb_w - tw - 4.f;
        if (tp.y + 18.f > fb_h - BOT_H) tp.y -= 22.f;
        m_ui.rect(tp - glm::vec2(4.f, 2.f), {tw, 18.f},
                  {0.04f, 0.04f, 0.08f, 0.85f}, 3.f);
        m_ui.text(tp, tipbuf, {0.9f, 1.f, 0.7f, 0.95f}, 12.f);
    }

    return world_modified;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main draw entry point
// ─────────────────────────────────────────────────────────────────────────────
MapEditorResult MapEditor::draw(
    glm::vec2 cursor,
    bool lmb_held, bool rmb_held, bool mmb_held,
    float scroll_y,
    bool pgup_pressed, bool pgdn_pressed,
    bool ctrl_s, bool ctrl_l,
    bool escape_pressed)
{
    MapEditorResult result;
    if (!m_open) return result;

    const float fb_w = static_cast<float>(m_ui.fb_width());
    const float fb_h = static_cast<float>(m_ui.fb_height());

    // ── Close check ──────────────────────────────────────────────────────────
    if (escape_pressed) {
        result.request_close = true;
        return result;
    }

    // ── Layer navigation (Page Up/Down) ──────────────────────────────────────
    if (pgup_pressed) ++m_layer;
    if (pgdn_pressed) --m_layer;

    // ── Zoom (scroll wheel, only when cursor is in the grid area) ────────────
    bool cursor_in_grid_x = (cursor.x > PAL_W && cursor.x < fb_w);
    bool cursor_in_grid_y = (cursor.y > TOP_H  && cursor.y < fb_h - BOT_H);
    if (cursor_in_grid_x && cursor_in_grid_y && scroll_y != 0.f) {
        float old_zoom = m_zoom;
        m_zoom *= (scroll_y > 0.f) ? 1.15f : (1.f / 1.15f);
        m_zoom  = std::clamp(m_zoom, ZOOM_MIN, ZOOM_MAX);

        // Zoom toward cursor: keep world point under cursor stationary.
        if (m_zoom != old_zoom) {
            glm::vec2 wc_before = screen_to_world(cursor);
            // After zoom change, recompute where that world point lands and
            // adjust pan so it stays under cursor.
            // screen_to_world uses m_zoom already updated, so:
            // wc_before = (cursor - gcenter) / old_zoom + pan_before
            // We want: (cursor - gcenter) / new_zoom + pan_new = wc_before
            // => pan_new = wc_before - (cursor - gcenter) / new_zoom
            const float gcx = PAL_W + (fb_w - PAL_W) * 0.5f;
            const float gcy = TOP_H + (fb_h - TOP_H - BOT_H) * 0.5f;
            m_pan.x = wc_before.x - (cursor.x - gcx) / m_zoom;
            m_pan.y = wc_before.y - (cursor.y - gcy) / m_zoom;
        }
    }

    // Palette scroll (cursor over palette area)
    if (cursor.x < PAL_W && cursor.y >= TOP_H && cursor.y < fb_h - BOT_H && scroll_y != 0.f) {
        const int count = static_cast<int>(m_palette.size());
        float total_h = static_cast<float>(count + 1) * (PAL_ITEM_H + 2.f) + 4.f;
        float visible_h = fb_h - TOP_H - BOT_H - 24.f;
        float max_scroll = std::max(0.f, total_h - visible_h);
        m_palette_scroll -= scroll_y * (PAL_ITEM_H + 2.f) * 2.f;
        m_palette_scroll  = std::clamp(m_palette_scroll, 0.f, max_scroll);
    }

    // ── Middle-mouse pan ─────────────────────────────────────────────────────
    if (mmb_held) {
        if (!m_panning) {
            m_panning           = true;
            m_pan_cursor_start  = cursor;
            m_pan_world_start   = m_pan;
        } else {
            glm::vec2 delta = cursor - m_pan_cursor_start;
            m_pan = m_pan_world_start - delta / m_zoom;
        }
    } else {
        m_panning = false;
    }

    // ── Save / load ──────────────────────────────────────────────────────────
    if (ctrl_s) {
        bool ok = map_save(m_world, m_voxel_reg, m_map_path);
        m_status_msg   = ok ? "[OK] Saved to " + m_map_path
                            : "[ERR] Save failed: " + m_map_path;
        m_status_timer = 3.5f;
    }
    if (ctrl_l) {
        bool ok = map_load(m_world, m_voxel_reg, m_map_path);
        if (ok) {
            result.world_modified      = true;
            result.needs_atmos_rebuild = true;
            m_status_msg   = "[OK] Loaded from " + m_map_path;
        } else {
            m_status_msg = "[ERR] Load failed: " + m_map_path;
        }
        m_status_timer = 3.5f;
    }

    // ── Detect LMB-press (rising edge) for palette single-click ─────────────
    bool lmb_pressed = lmb_held && !m_prev_lmb;
    m_prev_lmb = lmb_held;

    // ── Cursor world position for bottom bar ─────────────────────────────────
    glm::vec2 cursor_world = screen_to_world(cursor);

    // ── Draw layers ──────────────────────────────────────────────────────────
    // Grid first (bottom of the z-stack for UI)
    bool grid_modified = draw_grid(cursor, lmb_held && cursor.x > PAL_W,
                                            rmb_held && cursor.x > PAL_W);
    if (grid_modified) result.world_modified = true;

    // Palette (on top, clip to left strip)
    draw_palette(cursor, lmb_pressed && cursor.x < PAL_W);

    // Top and bottom bars (always on top)
    draw_top_bar();
    draw_bottom_bar(cursor_world);

    return result;
}
