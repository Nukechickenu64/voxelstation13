#include "ui/map_editor.h"
#include "data/map_io.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <functional>
#include <queue>

// ─────────────────────────────────────────────────────────────────────────────
MapEditor::MapEditor(UIRenderer&          ui,
                     World&               world,
                     VoxelRegistry&       voxels,
                     EntityManager&       entities,
                     WorldItemSystem&     world_items,
                     ItemRegistry&        item_reg,
                     MobSpeciesRegistry&  mob_reg,
                     ModelObjectManager&  model_objs,
                     std::vector<std::string> model_names)
    : m_ui(ui), m_world(world), m_voxel_reg(voxels),
      m_entities(entities), m_world_items(world_items),
      m_item_reg(item_reg), m_mob_reg(mob_reg),
      m_model_objs(model_objs), m_model_names(std::move(model_names))
{}

// ─────────────────────────────────────────────────────────────────────────────
void MapEditor::open(glm::vec3 player_pos)
{
    m_pan   = {player_pos.x, player_pos.z};
    m_layer = static_cast<int>(std::floor(player_pos.y));
    m_zoom  = 20.f;
    m_panning     = false;
    m_status_msg.clear();
    m_status_timer = 0.f;

    // Reset tool / undo state
    m_tab        = EditorTab::Voxels;
    m_tool       = EditorTool::Brush;
    m_in_stroke  = false;
    m_stroke_cells.clear();
    m_stroke_edits.clear();
    m_rect_active = false;
    m_undo_stack.clear();
    m_redo_stack.clear();
    m_place_yaw = 0.f;

    // ── Build voxel palette ───────────────────────────────────────────────────
    m_vox_palette.clear();
    m_vox_scroll = 0.f;
    for (const auto& [id, def] : m_voxel_reg.all()) {
        if (def.type_id == 0 || def.id == "air") continue;
        m_vox_palette.emplace_back(id, &def);
    }
    std::sort(m_vox_palette.begin(), m_vox_palette.end(),
              [](const auto& a, const auto& b){ return a.second->name < b.second->name; });
    m_selected_voxel_id = m_vox_palette.empty() ? 0 : m_vox_palette.front().first;

    // ── Build item palette ────────────────────────────────────────────────────
    m_item_palette.clear();
    m_item_scroll = 0.f;
    for (const auto& [id, def] : m_item_reg.all())
        m_item_palette.push_back(&def);
    std::sort(m_item_palette.begin(), m_item_palette.end(),
              [](const ItemDef* a, const ItemDef* b){ return a->name < b->name; });
    m_selected_item_id = m_item_palette.empty() ? "" : m_item_palette.front()->id;

    // ── Build mob palette ─────────────────────────────────────────────────────
    m_mob_palette.clear();
    m_mob_scroll = 0.f;
    for (const auto& [id, def] : m_mob_reg.all())
        m_mob_palette.push_back(&def);
    std::sort(m_mob_palette.begin(), m_mob_palette.end(),
              [](const MobSpeciesDef* a, const MobSpeciesDef* b){ return a->name < b->name; });
    m_selected_species = m_mob_palette.empty() ? "" : m_mob_palette.front()->species;

    // ── Model object palette ──────────────────────────────────────────────────
    m_model_scroll = 0.f;
    m_selected_model = m_model_names.empty() ? "" : m_model_names.front();

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
// Encode helpers for undo deduplication
// ─────────────────────────────────────────────────────────────────────────────

// Pack an ivec3 into 64 bits (21 bits per axis, offset for signed range).
static uint64_t encode_pos3(glm::ivec3 p) {
    constexpr uint64_t OFF = 1u << 20;
    uint64_t xu = (static_cast<uint64_t>(p.x) + OFF) & 0x1FFFFFu;
    uint64_t yu = (static_cast<uint64_t>(p.y) + OFF) & 0x1FFFFFu;
    uint64_t zu = (static_cast<uint64_t>(p.z) + OFF) & 0x1FFFFFu;
    return (xu << 42) | (yu << 21) | zu;
}

// Pack a 2-D cell (x,z) into 64 bits for flood-fill visited tracking.
static uint64_t encode_cell2(int x, int z) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32)
         |  static_cast<uint64_t>(static_cast<uint32_t>(z));
}

// ─────────────────────────────────────────────────────────────────────────────
// Undo / redo
// ─────────────────────────────────────────────────────────────────────────────

void MapEditor::push_undo(UndoOp op) {
    if (op.edits.empty()) return;
    m_redo_stack.clear();
    m_undo_stack.push_back(std::move(op));
    if (static_cast<int>(m_undo_stack.size()) > UNDO_MAX)
        m_undo_stack.erase(m_undo_stack.begin());
}

bool MapEditor::do_undo() {
    if (m_undo_stack.empty()) {
        m_status_msg   = "Nothing to undo.";
        m_status_timer = 1.5f;
        return false;
    }
    UndoOp op = std::move(m_undo_stack.back());
    m_undo_stack.pop_back();
    for (auto& e : op.edits)
        m_world.set_voxel(e.pos, e.before);
    // Undo entity spawns (destroy)
    for (EntityID id : op.spawned_entities)
        if (m_entities.alive(id)) m_entities.destroy(id);
    op.spawned_entities.clear();  // cannot redo entity spawns
    // Undo model object placements (remove by ID)
    for (auto& [id, def] : op.placed_model_objs)
        m_model_objs.remove(id);
    m_status_msg   = "[OK] Undo";
    m_status_timer = 1.5f;
    m_redo_stack.push_back(std::move(op));
    return true;
}

bool MapEditor::do_redo() {
    if (m_redo_stack.empty()) {
        m_status_msg   = "Nothing to redo.";
        m_status_timer = 1.5f;
        return false;
    }
    UndoOp op = std::move(m_redo_stack.back());
    m_redo_stack.pop_back();
    for (auto& e : op.edits)
        m_world.set_voxel(e.pos, e.after);
    // Redo model object placements (re-add, update stored IDs for next undo)
    for (auto& [stored_id, def] : op.placed_model_objs)
        stored_id = m_model_objs.add(def);
    // Entity spawns cannot be redone — spawned_entities already cleared by do_undo
    m_status_msg   = "[OK] Redo";
    m_status_timer = 1.5f;
    m_undo_stack.push_back(std::move(op));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Flood fill
// ─────────────────────────────────────────────────────────────────────────────

bool MapEditor::do_fill(glm::ivec2 start_cell) {
    Voxel origin      = m_world.get_voxel({start_cell.x, m_layer, start_cell.y});
    uint16_t tgt_type = origin.type_id;
    if (tgt_type == m_selected_voxel_id) return false;

    const VoxelTypeDef* def = (m_selected_voxel_id != 0) ? m_voxel_reg.get(m_selected_voxel_id) : nullptr;
    Voxel paint_v;
    paint_v.type_id = m_selected_voxel_id;
    paint_v.flags   = def ? def->default_flags
                          : static_cast<uint16_t>(VFLAG_SOLID | VFLAG_OPAQUE);

    constexpr int FILL_LIMIT = 4096;
    UndoOp op;
    std::queue<glm::ivec2> bfsq;
    std::unordered_set<uint64_t> visited;

    bfsq.push(start_cell);
    visited.insert(encode_cell2(start_cell.x, start_cell.y));

    const int dxdz[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    while (!bfsq.empty() && static_cast<int>(op.edits.size()) < FILL_LIMIT) {
        auto cell = bfsq.front(); bfsq.pop();
        glm::ivec3 pos{cell.x, m_layer, cell.y};
        Voxel cur = m_world.get_voxel(pos);
        if (cur.type_id != tgt_type) continue;

        op.edits.push_back({pos, cur, paint_v});
        m_world.set_voxel(pos, paint_v);

        for (auto& d : dxdz) {
            glm::ivec2 nb{cell.x + d[0], cell.y + d[1]};
            auto key = encode_cell2(nb.x, nb.y);
            if (!visited.count(key)) {
                visited.insert(key);
                bfsq.push(nb);
            }
        }
    }

    if (!op.edits.empty()) {
        const char* was_capped = (static_cast<int>(op.edits.size()) >= FILL_LIMIT) ? " (capped)" : "";
        m_status_msg   = "[OK] Fill – " + std::to_string(op.edits.size()) + " cells" + was_capped;
        m_status_timer = 1.5f;
        push_undo(std::move(op));
        return true;
    }
    return false;
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

    // Tool indicator
    {
        const char* tname = (m_tool == EditorTool::Brush) ? "BRUSH [B]"
                          : (m_tool == EditorTool::Fill)  ? "FILL  [F]"
                                                          : "RECT  [R]";
        glm::vec4 tcol = (m_tool == EditorTool::Brush) ? glm::vec4{0.55f, 1.0f, 0.65f, 1.f}
                       : (m_tool == EditorTool::Fill)  ? glm::vec4{1.0f, 0.75f, 0.30f, 1.f}
                                                       : glm::vec4{0.45f, 0.85f, 1.0f, 1.f};
        m_ui.text({390.f, 9.f}, tname, tcol, 13.f);
    }

    // Undo/redo depth indicator
    {
        char ubuf[32];
        std::snprintf(ubuf, sizeof(ubuf), "U:%d R:%d",
                      static_cast<int>(m_undo_stack.size()),
                      static_cast<int>(m_redo_stack.size()));
        m_ui.text({510.f, 9.f}, ubuf, {0.5f, 0.6f, 0.75f, 0.7f}, 11.f);
    }

    // Zoom indicator
    {
        char zbuf[32];
        std::snprintf(zbuf, sizeof(zbuf), "Zoom: %.0f px", m_zoom);
        m_ui.text({fb_w - 180.f, 9.f}, zbuf, {0.7f, 0.8f, 0.9f, 0.85f}, 12.f);
    }

    // Ctrl+S / Ctrl+L hints
    m_ui.text({fb_w - 370.f, 9.f}, "^S Save  ^L Load  ^Z Undo  ^Y Redo  F7/ESC Close",
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
// Shared palette scrollbar helper (draws a thin scrollbar on the right edge)
// ─────────────────────────────────────────────────────────────────────────────
static void draw_scrollbar(UIRenderer& ui, float x, float y0, float visible_h,
                            float total_h, float scroll)
{
    if (total_h <= visible_h) return;
    float track_h    = visible_h - 4.f;
    float thumb_h    = std::max(16.f, track_h * (visible_h / total_h));
    float max_scroll = total_h - visible_h;
    float ratio      = (max_scroll > 0.f) ? scroll / max_scroll : 0.f;
    float thumb_y    = y0 + 2.f + ratio * (track_h - thumb_h);
    ui.rect({x, y0 + 2.f}, {4.f, track_h},  {0.14f,0.16f,0.22f,0.8f}, 2.f);
    ui.rect({x, thumb_y},  {4.f, thumb_h},  {0.35f,0.50f,0.80f,0.85f}, 2.f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Tab bar
// ─────────────────────────────────────────────────────────────────────────────
void MapEditor::draw_tab_bar(glm::vec2 cursor, bool lmb_pressed)
{
    const float panel_y0 = TOP_H;
    const float tab_w    = (PAL_W - 2.f) / 4.f;
    const char* labels[4] = { "VOX", "ITM", "MOB", "OBJ" };
    const EditorTab tabs[4] = { EditorTab::Voxels, EditorTab::Items,
                                 EditorTab::Mobs,   EditorTab::Objects };
    for (int i = 0; i < 4; ++i) {
        float tx = 1.f + tab_w * i;
        float ty = panel_y0 + 1.f;
        bool active = (m_tab == tabs[i]);
        bool hov    = cursor.x >= tx && cursor.x < tx + tab_w
                   && cursor.y >= ty && cursor.y < ty + TAB_H - 2.f;
        glm::vec4 bg = active ? glm::vec4{0.18f,0.35f,0.60f,1.f}
                     : hov    ? glm::vec4{0.15f,0.20f,0.30f,1.f}
                              : glm::vec4{0.10f,0.13f,0.20f,0.9f};
        m_ui.rect({tx, ty}, {tab_w - 1.f, TAB_H - 2.f}, bg, 3.f);
        m_ui.text({tx + (tab_w - 22.f) * 0.5f, ty + 5.f}, labels[i],
                  active ? glm::vec4{1.f,1.f,1.f,1.f} : glm::vec4{0.6f,0.7f,0.9f,0.85f},
                  11.f);
        if (hov && lmb_pressed) m_tab = tabs[i];
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Outer palette panel — background + tab bar + dispatch to active sub-palette
// ─────────────────────────────────────────────────────────────────────────────
void MapEditor::draw_palette(glm::vec2 cursor, bool lmb_pressed)
{
    const float fb_h     = static_cast<float>(m_ui.fb_height());
    const float panel_h  = fb_h - TOP_H - BOT_H;
    const float panel_y0 = TOP_H;

    // Background
    m_ui.rect({0.f, panel_y0}, {PAL_W, panel_h},
              {0.08f, 0.10f, 0.15f, 0.98f});
    m_ui.rect({PAL_W - 1.f, panel_y0}, {1.f, panel_h},
              {0.2f, 0.3f, 0.5f, 0.6f});

    draw_tab_bar(cursor, lmb_pressed);

    switch (m_tab) {
        case EditorTab::Voxels:  draw_palette_voxels (cursor, lmb_pressed); break;
        case EditorTab::Items:   draw_palette_items  (cursor, lmb_pressed); break;
        case EditorTab::Mobs:    draw_palette_mobs   (cursor, lmb_pressed); break;
        case EditorTab::Objects: draw_palette_objects(cursor, lmb_pressed); break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Voxels sub-palette
// ─────────────────────────────────────────────────────────────────────────────
void MapEditor::draw_palette_voxels(glm::vec2 cursor, bool lmb_pressed)
{
    const float fb_h     = static_cast<float>(m_ui.fb_height());
    const float panel_y0 = TOP_H + TAB_H;
    const float avail_h  = fb_h - TOP_H - TAB_H - BOT_H;

    // "Erase (Air)" entry
    {
        const float iy = panel_y0 + 4.f - m_vox_scroll;
        if (iy >= panel_y0 && iy < panel_y0 + avail_h - PAL_ITEM_H) {
            bool sel = (m_selected_voxel_id == 0);
            bool hov = cursor.x >= 0.f && cursor.x < PAL_W
                    && cursor.y >= iy  && cursor.y < iy + PAL_ITEM_H;
            glm::vec4 bg = sel  ? glm::vec4{0.18f,0.35f,0.60f,1.f}
                         : hov  ? glm::vec4{0.15f,0.18f,0.26f,1.f}
                                : glm::vec4{0.09f,0.11f,0.17f,0.85f};
            m_ui.rect({PAL_MARGIN, iy}, {PAL_W - PAL_MARGIN * 2.f, PAL_ITEM_H - 2.f}, bg, 3.f);
            m_ui.rect({PAL_MARGIN + 3.f, iy + 4.f}, {PAL_ITEM_H - 10.f, PAL_ITEM_H - 10.f},
                      {0.65f,0.25f,0.25f,0.75f}, 2.f);
            m_ui.text({PAL_MARGIN + PAL_ITEM_H, iy + 5.f}, "Erase (Air)",
                      {0.9f,0.6f,0.6f,1.f}, 11.f);
            if (hov && lmb_pressed) m_selected_voxel_id = 0;
        }
    }

    const float item_y0 = panel_y0 + 4.f + PAL_ITEM_H + 2.f;
    const int count = static_cast<int>(m_vox_palette.size());
    for (int i = 0; i < count; ++i) {
        const float iy = item_y0 + static_cast<float>(i) * (PAL_ITEM_H + 2.f) - m_vox_scroll;
        if (iy + PAL_ITEM_H < panel_y0) continue;
        if (iy > panel_y0 + avail_h)    break;

        uint16_t     tid = m_vox_palette[i].first;
        const auto*  def = m_vox_palette[i].second;
        bool sel = (m_selected_voxel_id == tid);
        bool hov = cursor.x >= 0.f && cursor.x < PAL_W
                && cursor.y >= iy  && cursor.y < iy + PAL_ITEM_H;

        glm::vec4 bg = sel  ? glm::vec4{0.18f,0.35f,0.60f,1.f}
                     : hov  ? glm::vec4{0.15f,0.18f,0.26f,1.f}
                            : glm::vec4{0.09f,0.11f,0.17f,0.85f};
        m_ui.rect({PAL_MARGIN, iy}, {PAL_W - PAL_MARGIN * 2.f, PAL_ITEM_H - 2.f}, bg, 3.f);
        m_ui.rect({PAL_MARGIN + 3.f, iy + 4.f}, {PAL_ITEM_H - 10.f, PAL_ITEM_H - 10.f},
                  voxel_color(tid), 2.f);
        std::string name = def->name;
        if (name.size() > 14) name = name.substr(0, 13) + ".";
        m_ui.text({PAL_MARGIN + PAL_ITEM_H, iy + 5.f}, name,
                  sel ? glm::vec4{1.f,1.f,1.f,1.f} : glm::vec4{0.82f,0.88f,0.98f,0.9f}, 11.f);
        if (hov && lmb_pressed) m_selected_voxel_id = tid;
    }

    float total_h = static_cast<float>(count + 1) * (PAL_ITEM_H + 2.f) + 4.f;
    draw_scrollbar(m_ui, PAL_W - 7.f, panel_y0, avail_h, total_h, m_vox_scroll);
}

// ─────────────────────────────────────────────────────────────────────────────
// Items sub-palette
// ─────────────────────────────────────────────────────────────────────────────
void MapEditor::draw_palette_items(glm::vec2 cursor, bool lmb_pressed)
{
    const float fb_h    = static_cast<float>(m_ui.fb_height());
    const float pal_y0  = TOP_H + TAB_H;
    const float avail_h = fb_h - TOP_H - TAB_H - BOT_H;
    const int   count   = static_cast<int>(m_item_palette.size());

    if (count == 0) {
        m_ui.text({PAL_MARGIN, pal_y0 + 10.f}, "No items loaded",
                  {0.5f,0.5f,0.6f,0.8f}, 11.f);
        return;
    }

    const float item_y0 = pal_y0 + 4.f;
    for (int i = 0; i < count; ++i) {
        const float iy = item_y0 + static_cast<float>(i) * (PAL_ITEM_H + 2.f) - m_item_scroll;
        if (iy + PAL_ITEM_H < pal_y0) continue;
        if (iy > pal_y0 + avail_h)    break;

        const ItemDef* def = m_item_palette[i];
        bool sel = (m_selected_item_id == def->id);
        bool hov = cursor.x >= 0.f && cursor.x < PAL_W
                && cursor.y >= iy   && cursor.y < iy + PAL_ITEM_H;

        glm::vec4 bg = sel  ? glm::vec4{0.22f,0.45f,0.18f,1.f}
                     : hov  ? glm::vec4{0.15f,0.18f,0.26f,1.f}
                            : glm::vec4{0.09f,0.11f,0.17f,0.85f};
        m_ui.rect({PAL_MARGIN, iy}, {PAL_W - PAL_MARGIN * 2.f, PAL_ITEM_H - 2.f}, bg, 3.f);
        // Gold swatch for items
        m_ui.rect({PAL_MARGIN + 3.f, iy + 4.f}, {PAL_ITEM_H - 10.f, PAL_ITEM_H - 10.f},
                  {0.90f,0.75f,0.25f,0.85f}, 2.f);
        std::string name = def->name;
        if (name.size() > 14) name = name.substr(0, 13) + ".";
        m_ui.text({PAL_MARGIN + PAL_ITEM_H, iy + 5.f}, name,
                  sel ? glm::vec4{1.f,1.f,0.8f,1.f} : glm::vec4{0.82f,0.88f,0.98f,0.9f}, 11.f);
        if (hov && lmb_pressed) m_selected_item_id = def->id;
    }

    float total_h = static_cast<float>(count) * (PAL_ITEM_H + 2.f) + 4.f;
    draw_scrollbar(m_ui, PAL_W - 7.f, pal_y0, avail_h, total_h, m_item_scroll);
}

// ─────────────────────────────────────────────────────────────────────────────
// Mobs sub-palette
// ─────────────────────────────────────────────────────────────────────────────
void MapEditor::draw_palette_mobs(glm::vec2 cursor, bool lmb_pressed)
{
    const float fb_h    = static_cast<float>(m_ui.fb_height());
    const float pal_y0  = TOP_H + TAB_H;
    const float avail_h = fb_h - TOP_H - TAB_H - BOT_H;
    const int   count   = static_cast<int>(m_mob_palette.size());

    if (count == 0) {
        m_ui.text({PAL_MARGIN, pal_y0 + 10.f}, "No species loaded",
                  {0.5f,0.5f,0.6f,0.8f}, 11.f);
        return;
    }

    // Variant toggle (female / male) at top of list
    {
        const float vy = pal_y0 + 2.f;
        bool hov_f = cursor.x < PAL_W * 0.5f && cursor.y >= vy && cursor.y < vy + 18.f;
        bool hov_m = cursor.x >= PAL_W * 0.5f && cursor.x < PAL_W && cursor.y >= vy && cursor.y < vy + 18.f;
        bool is_f  = (m_selected_variant == "female");
        m_ui.rect({PAL_MARGIN,          vy}, {PAL_W * 0.5f - PAL_MARGIN - 1.f, 18.f},
                  is_f  ? glm::vec4{0.2f,0.4f,0.6f,1.f}
                        : hov_f ? glm::vec4{0.15f,0.2f,0.3f,1.f}
                                : glm::vec4{0.09f,0.11f,0.17f,0.8f}, 3.f);
        m_ui.rect({PAL_W * 0.5f + 1.f, vy}, {PAL_W * 0.5f - PAL_MARGIN - 2.f, 18.f},
                  !is_f ? glm::vec4{0.2f,0.4f,0.6f,1.f}
                        : hov_m ? glm::vec4{0.15f,0.2f,0.3f,1.f}
                                : glm::vec4{0.09f,0.11f,0.17f,0.8f}, 3.f);
        m_ui.text({PAL_MARGIN + 4.f,    vy + 3.f}, "Female", {0.9f,0.6f,0.8f,1.f}, 10.f);
        m_ui.text({PAL_W * 0.5f + 5.f,  vy + 3.f}, "Male",   {0.6f,0.8f,0.9f,1.f}, 10.f);
        if (hov_f && lmb_pressed) m_selected_variant = "female";
        if (hov_m && lmb_pressed) m_selected_variant = "male";
    }

    const float item_y0 = pal_y0 + 22.f;
    const float adj_avail = avail_h - 22.f;
    for (int i = 0; i < count; ++i) {
        const float iy = item_y0 + static_cast<float>(i) * (PAL_ITEM_H + 2.f) - m_mob_scroll;
        if (iy + PAL_ITEM_H < item_y0) continue;
        if (iy > item_y0 + adj_avail)  break;

        const MobSpeciesDef* def = m_mob_palette[i];
        bool sel = (m_selected_species == def->species);
        bool hov = cursor.x >= 0.f && cursor.x < PAL_W
                && cursor.y >= iy   && cursor.y < iy + PAL_ITEM_H;

        glm::vec4 bg = sel  ? glm::vec4{0.40f,0.20f,0.55f,1.f}
                     : hov  ? glm::vec4{0.15f,0.18f,0.26f,1.f}
                            : glm::vec4{0.09f,0.11f,0.17f,0.85f};
        m_ui.rect({PAL_MARGIN, iy}, {PAL_W - PAL_MARGIN * 2.f, PAL_ITEM_H - 2.f}, bg, 3.f);
        // Cyan swatch for mobs
        m_ui.rect({PAL_MARGIN + 3.f, iy + 4.f}, {PAL_ITEM_H - 10.f, PAL_ITEM_H - 10.f},
                  {0.35f,0.90f,0.85f,0.85f}, 2.f);
        std::string name = def->name;
        if (name.size() > 14) name = name.substr(0, 13) + ".";
        m_ui.text({PAL_MARGIN + PAL_ITEM_H, iy + 5.f}, name,
                  sel ? glm::vec4{1.f,0.9f,1.f,1.f} : glm::vec4{0.82f,0.88f,0.98f,0.9f}, 11.f);
        if (hov && lmb_pressed) m_selected_species = def->species;
    }

    float total_h = static_cast<float>(count) * (PAL_ITEM_H + 2.f) + 4.f;
    draw_scrollbar(m_ui, PAL_W - 7.f, item_y0, adj_avail, total_h, m_mob_scroll);
}

// ─────────────────────────────────────────────────────────────────────────────
// Objects sub-palette
// ─────────────────────────────────────────────────────────────────────────────
void MapEditor::draw_palette_objects(glm::vec2 cursor, bool lmb_pressed)
{
    const float fb_h    = static_cast<float>(m_ui.fb_height());
    const float pal_y0  = TOP_H + TAB_H;
    const float avail_h = fb_h - TOP_H - TAB_H - BOT_H;
    const int   count   = static_cast<int>(m_model_names.size());

    if (count == 0) {
        m_ui.text({PAL_MARGIN, pal_y0 + 10.f}, "No models loaded",
                  {0.5f,0.5f,0.6f,0.8f}, 11.f);
        return;
    }

    // Rotation indicator at top
    {
        char rbuf[32];
        std::snprintf(rbuf, sizeof(rbuf), "Yaw: %.0f\xc2\xb0  [Q/E]", m_place_yaw);
        m_ui.text({PAL_MARGIN, pal_y0 + 4.f}, rbuf, {0.8f,0.9f,0.6f,0.9f}, 11.f);
    }

    const float item_y0 = pal_y0 + 20.f;
    const float adj_avail = avail_h - 20.f;
    for (int i = 0; i < count; ++i) {
        const float iy = item_y0 + static_cast<float>(i) * (PAL_ITEM_H + 2.f) - m_model_scroll;
        if (iy + PAL_ITEM_H < item_y0) continue;
        if (iy > item_y0 + adj_avail)  break;

        const std::string& mname = m_model_names[i];
        bool sel = (m_selected_model == mname);
        bool hov = cursor.x >= 0.f && cursor.x < PAL_W
                && cursor.y >= iy   && cursor.y < iy + PAL_ITEM_H;

        glm::vec4 bg = sel  ? glm::vec4{0.18f,0.45f,0.25f,1.f}
                     : hov  ? glm::vec4{0.15f,0.18f,0.26f,1.f}
                            : glm::vec4{0.09f,0.11f,0.17f,0.85f};
        m_ui.rect({PAL_MARGIN, iy}, {PAL_W - PAL_MARGIN * 2.f, PAL_ITEM_H - 2.f}, bg, 3.f);
        // Green swatch for model objects
        m_ui.rect({PAL_MARGIN + 3.f, iy + 4.f}, {PAL_ITEM_H - 10.f, PAL_ITEM_H - 10.f},
                  {0.30f,0.85f,0.45f,0.85f}, 2.f);
        std::string label = mname;
        if (label.size() > 14) label = label.substr(0, 13) + ".";
        m_ui.text({PAL_MARGIN + PAL_ITEM_H, iy + 5.f}, label,
                  sel ? glm::vec4{0.8f,1.f,0.8f,1.f} : glm::vec4{0.82f,0.88f,0.98f,0.9f}, 11.f);
        if (hov && lmb_pressed) m_selected_model = mname;
    }

    float total_h = static_cast<float>(count) * (PAL_ITEM_H + 2.f) + 4.f;
    draw_scrollbar(m_ui, PAL_W - 7.f, item_y0, adj_avail, total_h, m_model_scroll);
}

// ─────────────────────────────────────────────────────────────────────────────
// Entity overlay — draws markers for placed items / mobs / model objects
// ─────────────────────────────────────────────────────────────────────────────
void MapEditor::draw_entity_overlay()
{
    // World items — gold dot at the cell
    m_entities.each<WorldItemComponent>([&](EntityID /*id*/, WorldItemComponent& wic) {
        glm::vec3 pos = WorldItemSystem::item_world_pos(wic);
        if (static_cast<int>(std::floor(pos.y + 0.5f)) != m_layer) return;
        glm::vec2 sc = world_to_screen(std::floor(pos.x) + 0.5f, std::floor(pos.z) + 0.5f);
        const float r = std::max(2.f, m_zoom * 0.18f);
        m_ui.rect(sc - glm::vec2(r), {r * 2.f, r * 2.f},
                  {0.95f, 0.80f, 0.20f, 0.85f}, r);
    });

    // Mobs — cyan circle marker
    m_entities.each<MobComponent>([&](EntityID id, MobComponent& /*mob*/) {
        auto* tr = m_entities.get_component<TransformComponent>(id);
        if (!tr) return;
        if (static_cast<int>(std::floor(tr->pos.y)) != m_layer
            && static_cast<int>(std::floor(tr->pos.y - 0.5f)) != m_layer) return;
        glm::vec2 sc = world_to_screen(std::floor(tr->pos.x) + 0.5f, std::floor(tr->pos.z) + 0.5f);
        const float r = std::max(3.f, m_zoom * 0.22f);
        m_ui.rect(sc - glm::vec2(r), {r * 2.f, r * 2.f},
                  {0.35f, 0.90f, 0.85f, 0.85f}, r);
    });

    // Model objects — green AABB outline at the layer
    for (const auto& obj : m_model_objs.objects()) {
        if (m_layer < obj.voxel_min.y || m_layer > obj.voxel_max.y) continue;
        glm::vec2 sl = world_to_screen(static_cast<float>(obj.voxel_min.x),
                                       static_cast<float>(obj.voxel_min.z));
        glm::vec2 sr = world_to_screen(static_cast<float>(obj.voxel_max.x + 1),
                                       static_cast<float>(obj.voxel_max.z + 1));
        glm::vec4 col{0.30f, 0.85f, 0.45f, 0.75f};
        m_ui.line({sl.x, sl.y}, {sr.x, sl.y}, col, 1.5f);
        m_ui.line({sr.x, sl.y}, {sr.x, sr.y}, col, 1.5f);
        m_ui.line({sr.x, sr.y}, {sl.x, sr.y}, col, 1.5f);
        m_ui.line({sl.x, sr.y}, {sl.x, sl.y}, col, 1.5f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Entity placement / removal on the grid
// ─────────────────────────────────────────────────────────────────────────────
bool MapEditor::handle_entity_placement(glm::vec2 cursor,
                                         bool lmb_pressed, bool rmb_pressed)
{
    glm::ivec2 hcell = hovered_cell(cursor);
    if (hcell.x == INT_MIN_SENTINEL) return false;

    bool modified = false;

    // ── Items ────────────────────────────────────────────────────────────────
    if (m_tab == EditorTab::Items) {
        if (lmb_pressed && !m_selected_item_id.empty()) {
            const ItemDef* def = m_item_reg.get(m_selected_item_id);
            if (def) {
                ItemStack st; st.def = def; st.count = 1; st.integrity = 1.f;
                glm::vec3 pos{ hcell.x + 0.5f, static_cast<float>(m_layer) + 0.5f, hcell.y + 0.5f };
                EntityID eid = m_world_items.spawn_floating(pos, std::move(st));
                UndoOp op;
                op.spawned_entities.push_back(eid);
                push_undo(std::move(op));
                m_status_msg   = "[OK] Placed " + def->name;
                m_status_timer = 1.f;
                modified = true;
            }
        }
        if (rmb_pressed) {
            // Destroy world-item entities resting at / near hovered cell
            std::vector<EntityID> to_kill;
            m_entities.each<WorldItemComponent>([&](EntityID id, WorldItemComponent& wic) {
                glm::vec3 pos = WorldItemSystem::item_world_pos(wic);
                if (static_cast<int>(std::floor(pos.x)) == hcell.x
                 && static_cast<int>(std::floor(pos.z)) == hcell.y
                 && static_cast<int>(std::floor(pos.y + 0.5f)) == m_layer)
                    to_kill.push_back(id);
            });
            for (EntityID id : to_kill) { m_entities.destroy(id); modified = true; }
            if (modified) { m_status_msg = "[OK] Removed item(s)"; m_status_timer = 1.f; }
        }
    }

    // ── Mobs ─────────────────────────────────────────────────────────────────
    else if (m_tab == EditorTab::Mobs) {
        if (lmb_pressed && !m_selected_species.empty()) {
            const MobSpeciesDef* sdef = m_mob_reg.get(m_selected_species);
            EntityID eid = m_entities.create();
            TransformComponent tr{};
            tr.pos = { hcell.x + 0.5f, static_cast<float>(m_layer) + 1.f, hcell.y + 0.5f };
            tr.yaw = 0.f;
            m_entities.add_component<TransformComponent>(eid, tr);
            MobComponent mob{};
            mob.species = m_selected_species;
            mob.variant = m_selected_variant;
            m_entities.add_component<MobComponent>(eid, mob);
            HealthComponent hp{};
            hp.health_max = sdef ? sdef->health_max : 100.f;
            m_entities.add_component<HealthComponent>(eid, hp);
            UndoOp op;
            op.spawned_entities.push_back(eid);
            push_undo(std::move(op));
            m_status_msg   = "[OK] Placed " + m_selected_species + " (" + m_selected_variant + ")";
            m_status_timer = 1.f;
            modified = true;
        }
        if (rmb_pressed) {
            std::vector<EntityID> to_kill;
            m_entities.each<MobComponent>([&](EntityID id, MobComponent&) {
                auto* tr = m_entities.get_component<TransformComponent>(id);
                if (!tr) return;
                if (static_cast<int>(std::floor(tr->pos.x)) == hcell.x
                 && static_cast<int>(std::floor(tr->pos.z)) == hcell.y
                 && (static_cast<int>(std::floor(tr->pos.y)) == m_layer
                  || static_cast<int>(std::floor(tr->pos.y - 0.5f)) == m_layer))
                    to_kill.push_back(id);
            });
            for (EntityID id : to_kill) { m_entities.destroy(id); modified = true; }
            if (modified) { m_status_msg = "[OK] Removed mob(s)"; m_status_timer = 1.f; }
        }
    }

    // ── Model objects ─────────────────────────────────────────────────────────
    else if (m_tab == EditorTab::Objects) {
        if (lmb_pressed && !m_selected_model.empty()) {
            StaticModelObject def;
            def.name       = m_selected_model;
            def.cell       = { hcell.x, m_layer, hcell.y };
            def.yaw        = m_place_yaw;
            def.scale      = 1.f;
            def.blocks_mobs = true;
            def.blocks_gas  = false;
            int obj_id = m_model_objs.add(def);
            // Store {new_id, def} for undo (remove) and redo (re-add)
            UndoOp op;
            op.placed_model_objs.push_back({ obj_id, def });
            push_undo(std::move(op));
            m_status_msg   = "[OK] Placed " + m_selected_model;
            m_status_timer = 1.f;
            modified = true;
        }
        if (rmb_pressed) {
            const auto& objs = m_model_objs.objects();
            int remove_id = -1;
            for (std::size_t i = 0; i < objs.size(); ++i) {
                const auto& obj = objs[i];
                if (hcell.x >= obj.voxel_min.x && hcell.x <= obj.voxel_max.x
                 && m_layer  >= obj.voxel_min.y && m_layer  <= obj.voxel_max.y
                 && hcell.y >= obj.voxel_min.z && hcell.y <= obj.voxel_max.z) {
                    remove_id = m_model_objs.id_at(i);
                    break;
                }
            }
            if (remove_id >= 0) {
                m_model_objs.remove(remove_id);
                // Removal is not undoable (we don't store the removed def here;
                // a future improvement could push a redo-able remove op)
                m_status_msg   = "[OK] Removed model object";
                m_status_timer = 1.f;
                modified = true;
            }
        }
    }

    return modified;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main grid
// ─────────────────────────────────────────────────────────────────────────────
bool MapEditor::draw_grid(glm::vec2 cursor,
                          bool lmb_held, bool lmb_pressed, bool lmb_released,
                          bool rmb_held, bool shift_held)
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

            // Rect-tool preview highlight
            bool in_rect_preview = false;
            if (m_rect_active && m_tool == EditorTool::Rect) {
                int rx0 = std::min(m_rect_origin.x, hcell.x);
                int rx1 = std::max(m_rect_origin.x, hcell.x);
                int rz0 = std::min(m_rect_origin.y, hcell.y);
                int rz1 = std::max(m_rect_origin.y, hcell.y);
                if (wx >= rx0 && wx <= rx1 && wz >= rz0 && wz <= rz1)
                    in_rect_preview = true;
            }

            if (in_rect_preview) {
                // blend selection colour with cell colour
                glm::vec4 sel_col = voxel_color(m_selected_voxel_id);
                col.r = col.r * 0.4f + sel_col.r * 0.6f;
                col.g = col.g * 0.4f + sel_col.g * 0.6f;
                col.b = col.b * 0.4f + sel_col.b * 0.6f;
            } else if (hover) {
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

    // ── Rect-tool border outline ─────────────────────────────────────────────
    if (m_rect_active && m_tool == EditorTool::Rect && cursor_in_grid) {
        int rx0 = std::min(m_rect_origin.x, hcell.x);
        int rx1 = std::max(m_rect_origin.x, hcell.x) + 1;
        int rz0 = std::min(m_rect_origin.y, hcell.y);
        int rz1 = std::max(m_rect_origin.y, hcell.y) + 1;
        glm::vec2 sl = world_to_screen(static_cast<float>(rx0), static_cast<float>(rz0));
        glm::vec2 sr = world_to_screen(static_cast<float>(rx1), static_cast<float>(rz1));
        glm::vec4 border{0.9f, 0.85f, 0.3f, 0.9f};
        const float lw = 1.5f;
        m_ui.line({sl.x, sl.y}, {sr.x, sl.y}, border, lw);
        m_ui.line({sr.x, sl.y}, {sr.x, sr.y}, border, lw);
        m_ui.line({sr.x, sr.y}, {sl.x, sr.y}, border, lw);
        m_ui.line({sl.x, sr.y}, {sl.x, sl.y}, border, lw);

        // Dimension label
        char dim[32];
        std::snprintf(dim, sizeof(dim), "%dx%d", rx1-rx0, rz1-rz0);
        m_ui.text(sl + glm::vec2(2.f, -14.f), dim, {1.f, 1.f, 0.5f, 0.9f}, 11.f);
    }

    // ── Paint / erase / rect apply ──────────────────────────────────────────
    if (cursor_in_grid && m_tab == EditorTab::Voxels) {
        const VoxelTypeDef* def = (m_selected_voxel_id != 0) ? m_voxel_reg.get(m_selected_voxel_id) : nullptr;
        Voxel paint_v;
        paint_v.type_id = m_selected_voxel_id;
        paint_v.flags   = def ? def->default_flags
                              : static_cast<uint16_t>(VFLAG_SOLID | VFLAG_OPAQUE);

        // ── Brush tool ──────────────────────────────────────────────────────
        if (m_tool == EditorTool::Brush && !shift_held) {
            // Begin stroke on pen-down
            if (lmb_pressed || (lmb_pressed && rmb_held)) {
                m_in_stroke = true;
                m_stroke_cells.clear();
                m_stroke_edits.clear();
            }
            if (lmb_held) {
                uint64_t key = encode_pos3({hcell.x, m_layer, hcell.y});
                if (!m_stroke_cells.count(key)) {
                    Voxel cur_v = m_world.get_voxel({hcell.x, m_layer, hcell.y});
                    if (cur_v.type_id != m_selected_voxel_id) {
                        m_stroke_cells.insert(key);
                        m_stroke_edits.push_back({{hcell.x, m_layer, hcell.y}, cur_v, paint_v});
                        m_world.set_voxel({hcell.x, m_layer, hcell.y}, paint_v);
                        world_modified = true;
                    }
                }
            }
            // Commit stroke on pen-up
            if (lmb_released && m_in_stroke) {
                if (!m_stroke_edits.empty()) {
                    UndoOp op;
                    op.edits = std::move(m_stroke_edits);
                    push_undo(std::move(op));
                }
                m_in_stroke = false;
                m_stroke_cells.clear();
                m_stroke_edits.clear();
            }

            // RMB erase (own stroke)
            if (lmb_released || lmb_pressed) { /* handled above */ }
            if (rmb_held) {
                uint64_t key = encode_pos3({hcell.x, m_layer, hcell.y});
                if (!m_stroke_cells.count(key)) {
                    Voxel cur_v = m_world.get_voxel({hcell.x, m_layer, hcell.y});
                    if (cur_v.type_id != 0) {
                        m_stroke_cells.insert(key);
                        m_stroke_edits.push_back({{hcell.x, m_layer, hcell.y}, cur_v, Voxel{}});
                        m_world.set_voxel({hcell.x, m_layer, hcell.y}, Voxel{});
                        world_modified = true;
                    }
                }
            }
        }

        // ── Rect tool ───────────────────────────────────────────────────────
        if (m_tool == EditorTool::Rect || (m_tool == EditorTool::Brush && shift_held)) {
            // Override: if Shift is held on any tool, use rect mode temporarily
            const bool using_rect = (m_tool == EditorTool::Rect)
                                 || (m_tool == EditorTool::Brush && shift_held);
            if (using_rect && lmb_pressed && !m_rect_active) {
                m_rect_active = true;
                m_rect_origin = hcell;
            }
            if (using_rect && lmb_released && m_rect_active) {
                // Apply the rectangle
                int rx0 = std::min(m_rect_origin.x, hcell.x);
                int rx1 = std::max(m_rect_origin.x, hcell.x);
                int rz0 = std::min(m_rect_origin.y, hcell.y);
                int rz1 = std::max(m_rect_origin.y, hcell.y);
                UndoOp op;
                for (int iz = rz0; iz <= rz1; ++iz) {
                    for (int ix = rx0; ix <= rx1; ++ix) {
                        glm::ivec3 pos{ix, m_layer, iz};
                        Voxel cur = m_world.get_voxel(pos);
                        if (cur.type_id != m_selected_voxel_id) {
                            op.edits.push_back({pos, cur, paint_v});
                            m_world.set_voxel(pos, paint_v);
                            world_modified = true;
                        }
                    }
                }
                if (!op.edits.empty()) {
                    m_status_msg   = "[OK] Rect – " + std::to_string(op.edits.size()) + " cells";
                    m_status_timer = 1.5f;
                    push_undo(std::move(op));
                }
                m_rect_active = false;
            }
            if (!lmb_held) m_rect_active = false;
        }

        // ── RMB erase (non-Brush, or always for non-Brush tools) ───────────
        if (m_tool != EditorTool::Brush && rmb_held) {
            Voxel cur_v = m_world.get_voxel({hcell.x, m_layer, hcell.y});
            if (cur_v.type_id != 0) {
                // Single-cell erase without stroke batching for non-brush tools
                UndoOp op;
                op.edits.push_back({{hcell.x, m_layer, hcell.y}, cur_v, Voxel{}});
                m_world.set_voxel({hcell.x, m_layer, hcell.y}, Voxel{});
                push_undo(std::move(op));
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
        bool ok = map_save_full(m_world, m_voxel_reg, m_entities,
                                m_item_reg, m_model_objs, m_map_path);
        m_status_msg   = ok ? "[OK] Saved to " + m_map_path
                            : "[ERR] Save failed: " + m_map_path;
        m_status_timer = 3.5f;
    }
    if (ctrl_l) {
        bool ok = map_load_full(m_world, m_voxel_reg, m_entities,
                                m_world_items, m_item_reg, m_mob_reg,
                                m_model_objs, m_map_path);
        if (ok) {
            result.world_modified      = true;
            result.needs_atmos_rebuild = true;
            result.map_reloaded        = true;
            m_status_msg   = "[OK] Loaded from " + m_map_path;
        } else {
            m_status_msg = "[ERR] Load failed: " + m_map_path;
        }
        m_status_timer = 3.5f;
    }

    // ── Detect LMB/RMB-press / release (rising + falling edge) ────────────────
    bool lmb_pressed  = lmb_held && !m_prev_lmb;
    bool lmb_released = !lmb_held && m_prev_lmb;
    m_prev_lmb = lmb_held;
    bool rmb_pressed  = rmb_held && !m_prev_rmb;
    m_prev_rmb = rmb_held;

    // ── Internal key state: tool switching and undo/redo ─────────────────────
    {
        const bool* ks_i = SDL_GetKeyboardState(nullptr);
        bool ctrl_i  = ks_i[SDL_SCANCODE_LCTRL] || ks_i[SDL_SCANCODE_RCTRL];
        bool z_now   = ctrl_i && ks_i[SDL_SCANCODE_Z];
        bool y_now   = ctrl_i && ks_i[SDL_SCANCODE_Y];
        bool b_now   = ks_i[SDL_SCANCODE_B];
        bool f_now   = ks_i[SDL_SCANCODE_F];
        bool r_now   = ks_i[SDL_SCANCODE_R];

        bool q_now = ks_i[SDL_SCANCODE_Q];
        bool e_now = ks_i[SDL_SCANCODE_E];

        if (z_now && !m_prev_z_key) { if (do_undo()) result.world_modified = true; }
        if (y_now && !m_prev_y_key) { if (do_redo()) result.world_modified = true; }
        if (b_now && !m_prev_b_key) { m_tool = EditorTool::Brush; m_status_msg = "Tool: Brush";  m_status_timer = 1.f; }
        if (f_now && !m_prev_f_key) { m_tool = EditorTool::Fill;  m_status_msg = "Tool: Fill";   m_status_timer = 1.f; }
        if (r_now && !m_prev_r_key) { m_tool = EditorTool::Rect;  m_status_msg = "Tool: Rect";   m_status_timer = 1.f; }
        // Q/E rotate model yaw (Objects tab)
        if (q_now && !m_prev_q_key && m_tab == EditorTab::Objects)
            m_place_yaw = std::fmod(m_place_yaw - 90.f + 360.f, 360.f);
        if (e_now && !m_prev_e_key && m_tab == EditorTab::Objects)
            m_place_yaw = std::fmod(m_place_yaw + 90.f, 360.f);

        m_prev_z_key = z_now;
        m_prev_y_key = y_now;
        m_prev_b_key = b_now;
        m_prev_f_key = f_now;
        m_prev_r_key = r_now;
        m_prev_q_key = q_now;
        m_prev_e_key = e_now;
    }

    // ── Shift state (for Shift+LMB rect override on any tool) ────────────────
    const bool* ks_shift = SDL_GetKeyboardState(nullptr);
    bool shift_held = ks_shift[SDL_SCANCODE_LSHIFT] || ks_shift[SDL_SCANCODE_RSHIFT];

    // ── Cursor world position for bottom bar ─────────────────────────────────
    glm::vec2 cursor_world = screen_to_world(cursor);

    // ── Draw layers ──────────────────────────────────────────────────────────
    // Grid first (bottom of the z-stack for UI)

    // Fill-tool dispatch: on LMB press over the grid area, trigger flood fill.
    if (m_tool == EditorTool::Fill && lmb_pressed && cursor.x > PAL_W) {
        glm::ivec2 fc = hovered_cell(cursor);
        if (fc.x != INT_MIN_SENTINEL) {
            if (do_fill(fc)) result.world_modified = true;
        }
    }

    bool grid_modified = draw_grid(cursor,
                                   lmb_held      && cursor.x > PAL_W,
                                   lmb_pressed   && cursor.x > PAL_W,
                                   lmb_released  && cursor.x > PAL_W,
                                   rmb_held      && cursor.x > PAL_W,
                                   shift_held);
    if (grid_modified) result.world_modified = true;

    // Entity placement (Items / Mobs / Objects tabs)
    if (cursor.x > PAL_W && cursor.y > TOP_H && cursor.y < fb_h - BOT_H) {
        if (handle_entity_placement(cursor,
                                    lmb_pressed  && cursor.x > PAL_W,
                                    rmb_pressed  && cursor.x > PAL_W))
            result.world_modified = true;
    }

    // ── Palette scroll (process before draw_palette so current frame sees it) ────
    if (cursor.x < PAL_W && cursor.y > TOP_H && cursor.y < fb_h - BOT_H
        && scroll_y != 0.f) {
        const float avail_h = fb_h - TOP_H - TAB_H - BOT_H;
        auto clamp_scroll = [&](float& sv, int count) {
            float total_h    = static_cast<float>(count) * (PAL_ITEM_H + 2.f) + 4.f;
            float max_scroll = std::max(0.f, total_h - avail_h);
            sv -= scroll_y * (PAL_ITEM_H + 2.f) * 2.f;
            sv  = std::clamp(sv, 0.f, max_scroll);
        };
        switch (m_tab) {
            case EditorTab::Voxels:  clamp_scroll(m_vox_scroll,   (int)m_vox_palette.size() + 1); break;
            case EditorTab::Items:   clamp_scroll(m_item_scroll,  (int)m_item_palette.size());     break;
            case EditorTab::Mobs:    clamp_scroll(m_mob_scroll,   (int)m_mob_palette.size());      break;
            case EditorTab::Objects: clamp_scroll(m_model_scroll, (int)m_model_names.size());      break;
        }
    }

    // Entity overlay (items / mobs / model objects on the grid)
    draw_entity_overlay();

    // Palette (on top, clip to left strip)
    draw_palette(cursor, lmb_pressed && cursor.x < PAL_W);

    // Top and bottom bars (always on top)
    draw_top_bar();
    draw_bottom_bar(cursor_world);

    return result;
}
