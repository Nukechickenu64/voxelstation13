#pragma once
#include "render/ui_renderer.h"
#include "core/world.h"
#include "core/entity_manager.h"
#include "data/voxel_registry.h"
#include "data/mob_species_registry.h"
#include "inventory/item_registry.h"
#include "simulation/world_items.h"
#include "simulation/mob_system.h"
#include "simulation/model_objects.h"
#include "simulation/physics.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <unordered_set>

// ─────────────────────────────────────────────────────────────────────────────
// Palette tabs
// ─────────────────────────────────────────────────────────────────────────────
enum class EditorTab  { Voxels, Items, Mobs, Objects };

// ─────────────────────────────────────────────────────────────────────────────
// Tool modes (active only in Voxels tab)
// ─────────────────────────────────────────────────────────────────────────────
enum class EditorTool { Brush, Fill, Rect };

// ─────────────────────────────────────────────────────────────────────────────
// Undo / redo record
// ─────────────────────────────────────────────────────────────────────────────
struct VoxelEdit {
    glm::ivec3 pos;
    Voxel      before;
    Voxel      after;
};

struct UndoOp {
    // Voxel paint/fill/rect edits
    std::vector<VoxelEdit> edits;
    // Item / mob spawns: undo destroys these entities (redo cannot replay)
    std::vector<EntityID>  spawned_entities;
    // Model-object placements: undo removes by ID, redo re-adds using stored def
    std::vector<std::pair<int, StaticModelObject>> placed_model_objs;
};

// ─────────────────────────────────────────────────────────────────────────────
// MapEditor — full-screen top-down voxel map editing overlay.
// Toggle with F7.  The caller must free the mouse cursor while the editor
// is open and recapture it on close.
//
// Palette tabs:
//   VOXELS  — paint/erase/fill voxel cells (original behaviour)
//   ITEMS   — click to place a world item; RMB to delete
//   MOBS    — click to place a mob entity;  RMB to delete
//   OBJECTS — click to place a model object; RMB to delete; Q/E rotate
//
// Common controls:
//   Scroll wheel    — zoom in / out (grid) or scroll palette list
//   Middle-drag     — pan the view
//   Page Up / Down  — move edit Y-layer up / down
//   Ctrl+Z / Ctrl+Y — undo / redo (voxels + model objects; entity spawns undo-only)
//   Ctrl+S          — save map (voxels+items+mobs+objects) to maps/current.json
//   Ctrl+L          — load map from maps/current.json
//   Escape / F7     — close editor
//
// Voxels tab:
//   LMB drag        — Brush paint
//   RMB drag        — Erase (air)
//   Shift+LMB drag  — Rectangle fill
//   B / F / R       — Brush / Fill / Rect tool
//
// Objects tab:
//   Q / E           — rotate placement yaw -90° / +90°
// ─────────────────────────────────────────────────────────────────────────────

struct MapEditorResult {
    bool world_modified      = false;
    bool needs_atmos_rebuild = false;
    bool map_reloaded        = false;  // true when a full map_load_full was performed
    bool request_close       = false;
};

class MapEditor {
public:
    MapEditor(UIRenderer&          ui,
              World&               world,
              VoxelRegistry&       voxels,
              EntityManager&       entities,
              WorldItemSystem&     world_items,
              ItemRegistry&        item_reg,
              MobSpeciesRegistry&  mob_reg,
              ModelObjectManager&  model_objs,
              std::vector<std::string> model_names);

    void open(glm::vec3 player_pos);
    void close();
    bool is_open() const { return m_open; }

    MapEditorResult draw(glm::vec2 cursor,
                         bool lmb_held, bool rmb_held, bool mmb_held,
                         float scroll_y,
                         bool pgup_pressed, bool pgdn_pressed,
                         bool ctrl_s, bool ctrl_l,
                         bool escape_pressed);

private:
    // ── Draw helpers ──────────────────────────────────────────────────────────
    void draw_top_bar();
    void draw_bottom_bar(glm::vec2 cursor_world);
    void draw_tab_bar(glm::vec2 cursor, bool lmb_pressed);
    void draw_palette(glm::vec2 cursor, bool lmb_pressed);
    void draw_palette_voxels(glm::vec2 cursor, bool lmb_pressed);
    void draw_palette_items (glm::vec2 cursor, bool lmb_pressed);
    void draw_palette_mobs  (glm::vec2 cursor, bool lmb_pressed);
    void draw_palette_objects(glm::vec2 cursor, bool lmb_pressed);

    // Renders voxel cells + handles voxel painting (returns true if modified).
    bool draw_grid(glm::vec2 cursor,
                   bool lmb_held, bool lmb_pressed, bool lmb_released,
                   bool rmb_held, bool rmb_pressed, bool rmb_released,
                   bool shift_held, bool alt_held);

    // Draws entity/object markers overlaid on the grid.
    void draw_entity_overlay();

    // Handles item/mob/object placement & removal on the grid.
    // Returns true if the world changed.
    bool handle_entity_placement(glm::vec2 cursor,
                                 bool lmb_pressed, bool rmb_pressed);

    // ── Coordinate helpers ────────────────────────────────────────────────────
    glm::vec2  world_to_screen(float wx, float wz) const;
    glm::vec2  screen_to_world(glm::vec2 screen) const;
    glm::ivec2 hovered_cell(glm::vec2 cursor) const;

    // ── Voxel colour ─────────────────────────────────────────────────────────
    glm::vec4 voxel_color(uint16_t type_id) const;

    // ── Undo / redo ───────────────────────────────────────────────────────────
    void push_undo(UndoOp op);
    bool do_undo();
    bool do_redo();
    bool do_fill(glm::ivec2 start_cell);

    // ── Core references ───────────────────────────────────────────────────────
    UIRenderer&         m_ui;
    World&              m_world;
    VoxelRegistry&      m_voxel_reg;
    EntityManager&      m_entities;
    WorldItemSystem&    m_world_items;
    ItemRegistry&       m_item_reg;
    MobSpeciesRegistry& m_mob_reg;
    ModelObjectManager& m_model_objs;
    std::vector<std::string> m_model_names;

    bool m_open = false;

    // ── View ─────────────────────────────────────────────────────────────────
    int       m_layer = 0;
    glm::vec2 m_pan   = {0,0};
    float     m_zoom  = 20.f;

    // ── Tab / tool ────────────────────────────────────────────────────────────
    EditorTab  m_tab  = EditorTab::Voxels;
    EditorTool m_tool = EditorTool::Brush;

    // ── Voxel palette ─────────────────────────────────────────────────────────
    uint16_t m_selected_voxel_id   = 1;
    uint8_t  m_paint_orientation   = 0;   // 0-3 applied when painting voxels
    std::vector<std::pair<uint16_t, const VoxelTypeDef*>> m_vox_palette;
    float    m_vox_scroll = 0.f;

    // ── Item palette ──────────────────────────────────────────────────────────
    std::string                  m_selected_item_id;
    std::vector<const ItemDef*>  m_item_palette;
    float                        m_item_scroll = 0.f;

    // ── Mob palette ───────────────────────────────────────────────────────────
    std::string                       m_selected_species;
    std::string                       m_selected_variant = "female";
    std::vector<const MobSpeciesDef*> m_mob_palette;
    float                             m_mob_scroll = 0.f;

    // ── Object palette ────────────────────────────────────────────────────────
    std::string m_selected_model;
    float       m_model_scroll = 0.f;
    float       m_place_yaw    = 0.f;   // 0 / 90 / 180 / 270 degrees

    // ── Middle-mouse pan ──────────────────────────────────────────────────────
    bool      m_panning          = false;
    glm::vec2 m_pan_cursor_start = {};
    glm::vec2 m_pan_world_start  = {};

    // ── LMB edge tracking ─────────────────────────────────────────────────────
    bool m_prev_lmb = false;
    bool m_prev_rmb = false;

    // ── Undo / redo stacks ────────────────────────────────────────────────────
    static constexpr int UNDO_MAX = 64;
    std::vector<UndoOp> m_undo_stack;
    std::vector<UndoOp> m_redo_stack;

    // Brush stroke accumulator (LMB paint + RMB erase, shared — only one active at a time)
    bool                         m_in_stroke        = false;
    bool                         m_erase_in_stroke  = false;  // non-Brush RMB erase
    std::unordered_set<uint64_t> m_stroke_cells;
    std::vector<VoxelEdit>       m_stroke_edits;

    // Rect tool
    bool       m_rect_active = false;
    glm::ivec2 m_rect_origin = {};

    // ── Internal key prev-state ───────────────────────────────────────────────
    bool m_prev_z_key = false;
    bool m_prev_y_key = false;
    bool m_prev_b_key = false;
    bool m_prev_f_key = false;
    bool m_prev_r_key = false;
    bool m_prev_q_key   = false;
    bool m_prev_e_key   = false;
    bool m_prev_alt_key = false;

    // ── Status ────────────────────────────────────────────────────────────────
    std::string m_status_msg;
    float       m_status_timer = 0.f;

    std::string m_map_path = "maps/current.json";

    // ── Layout constants ──────────────────────────────────────────────────────
    static constexpr float TOP_H      = 34.f;
    static constexpr float BOT_H      = 30.f;
    static constexpr float PAL_W      = 180.f;
    static constexpr float TAB_H      = 26.f;   // tab-bar height inside palette
    static constexpr float ZOOM_MIN   = 4.f;
    static constexpr float ZOOM_MAX   = 64.f;
    static constexpr float PAL_ITEM_H = 26.f;
    static constexpr float PAL_MARGIN = 6.f;
};

