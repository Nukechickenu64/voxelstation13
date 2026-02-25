#pragma once
#include "render/ui_renderer.h"
#include "core/world.h"
#include "data/voxel_registry.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// MapEditor — full-screen top-down voxel map editing overlay.
// Toggle with F7.  The caller must free the mouse cursor while the editor
// is open and recapture it on close.
//
// Controls:
//   LMB drag        — paint selected voxel type
//   RMB drag        — erase voxels (set to air)
//   Scroll wheel    — zoom in / out
//   Middle-drag     — pan the view
//   Page Up / Down  — move edit Y-layer up / down
//   Ctrl+S          — save map to maps/current.json
//   Ctrl+L          — load map from maps/current.json
//   Escape / F7     — close editor
// ─────────────────────────────────────────────────────────────────────────────

struct MapEditorResult {
    bool world_modified      = false; // true → caller should remesh dirty chunks
    bool needs_atmos_rebuild = false; // true → caller should rebuild atmos (map loaded)
    bool request_close       = false; // true → caller should close and recapture cursor
};

class MapEditor {
public:
    MapEditor(UIRenderer& ui, World& world, VoxelRegistry& voxels);

    // Open the editor centred on player_pos.
    void open(glm::vec3 player_pos);
    void close();
    bool is_open() const { return m_open; }

    // Called exactly once per render frame while the editor is open.
    // All boolean inputs are raw state for this frame:
    //   lmb_held / rmb_held / mmb_held — SDL mouse button states
    //   scroll_y  — signed wheel ticks (positive = zoom in)
    //   pgup / pgdn — edge-triggered key (use SDL_SCANCODE_PAGEUP/DOWN)
    //   ctrl_s / ctrl_l — Ctrl+S/L pressed this frame
    //   escape    — Escape key pressed this frame
    MapEditorResult draw(glm::vec2 cursor,
                         bool lmb_held, bool rmb_held, bool mmb_held,
                         float scroll_y,
                         bool pgup_pressed, bool pgdn_pressed,
                         bool ctrl_s, bool ctrl_l,
                         bool escape_pressed);

private:
    // ── Internal draw helpers ─────────────────────────────────────────────────
    void draw_top_bar();
    void draw_bottom_bar(glm::vec2 cursor_world);
    void draw_palette(glm::vec2 cursor, bool lmb_pressed);
    // Returns true if any voxel was modified.
    bool draw_grid(glm::vec2 cursor, bool lmb_held, bool rmb_held);

    // ── Coordinate conversion ─────────────────────────────────────────────────
    // World (X,Z) → screen pixel (top-left of cell rect)
    glm::vec2 world_to_screen(float wx, float wz) const;
    // Screen pixel → world (X,Z) float
    glm::vec2 screen_to_world(glm::vec2 screen) const;
    // Hovered world cell (X, Z integers) under the given screen pos, or
    // {INT_MIN, INT_MIN} when the cursor is outside the grid area.
    glm::ivec2 hovered_cell(glm::vec2 cursor) const;

    // ── Colour helpers ────────────────────────────────────────────────────────
    glm::vec4 voxel_color(uint16_t type_id) const;

    // ── Members ───────────────────────────────────────────────────────────────
    UIRenderer&    m_ui;
    World&         m_world;
    VoxelRegistry& m_voxel_reg;
    bool           m_open = false;

    // View state
    int       m_layer = 0;       // current edit Y layer
    glm::vec2 m_pan   = {0,0};   // world (X,Z) at the centre of the grid view
    float     m_zoom  = 20.f;    // pixels per voxel cell

    // Selection
    uint16_t m_selected_id = 1;

    // Palette state
    std::vector<std::pair<uint16_t, const VoxelTypeDef*>> m_palette;
    float m_palette_scroll = 0.f;

    // Middle-mouse pan state
    bool      m_panning          = false;
    glm::vec2 m_pan_cursor_start = {};
    glm::vec2 m_pan_world_start  = {};

    // Rising-edge LMB tracking for palette clicks
    bool m_prev_lmb = false;

    // Status message
    std::string m_status_msg;
    float       m_status_timer = 0.f;

    // Saved map path
    std::string m_map_path = "maps/current.json";

    // ── Layout constants (pixels) ─────────────────────────────────────────────
    static constexpr float TOP_H    = 34.f;   // top bar height
    static constexpr float BOT_H    = 30.f;   // bottom bar height
    static constexpr float PAL_W    = 180.f;  // palette panel width
    static constexpr float ZOOM_MIN = 4.f;
    static constexpr float ZOOM_MAX = 64.f;
    static constexpr float PAL_ITEM_H = 26.f;
    static constexpr float PAL_MARGIN = 6.f;
};
