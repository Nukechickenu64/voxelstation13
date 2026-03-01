#pragma once
#include "render/ui_renderer.h"
#include <glm/glm.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// AdminMenu — F1 overlay that surfaces all debug/admin toggles as clickable
// buttons so you don't have to remember which function key does what.
// ─────────────────────────────────────────────────────────────────────────────

// Current state of every toggle, fed in each frame so the menu can render
// ON/OFF indicators.
struct AdminMenuState {
    // ── Visual / debug toggles ────────────────────────────────────────────
    bool noclip            = false;
    bool build_mode        = false;
    bool gas_overlay       = false;
    bool debug_overlay     = false;
    bool player_stats      = false;
    bool verbose_log       = false;
    bool fullbright        = false;
    bool ambient_occlusion = false;
    bool godmode           = false;
    bool wireframe         = false;

    // ── Simulation dev-tool toggles ───────────────────────────────────────
    bool freeze_sim      = false;   // pause server.tick()
    bool slow_motion     = false;   // 0.1× time scale
    bool auto_heal       = false;   // regen health to max each frame
    bool zerog_override  = false;   // force zero-G regardless of atmos zone
    bool infinite_oxy    = false;   // zero out oxy damage every frame
};

// Returned by draw() — each flag is true for exactly one frame when the
// corresponding button is clicked.
struct AdminMenuResult {
    // ── Visual / debug toggles ────────────────────────────────────────────
    bool toggle_noclip            = false;
    bool toggle_build_mode        = false;
    bool toggle_gas_overlay       = false;
    bool toggle_debug_overlay     = false;
    bool toggle_player_stats      = false;
    bool toggle_verbose_log       = false;
    bool toggle_fullbright        = false;
    bool toggle_ambient_occlusion = false;
    bool toggle_godmode           = false;
    bool toggle_wireframe         = false;
    bool close_requested          = false;

    // ── Simulation dev-tool toggles ───────────────────────────────────────
    bool toggle_freeze_sim      = false;
    bool toggle_slow_motion     = false;
    bool toggle_auto_heal       = false;
    bool toggle_zerog_override  = false;
    bool toggle_infinite_oxy    = false;

    // ── One-shot actions (fire once on the click frame) ───────────────────
    bool action_full_heal       = false;   // zero all damage buckets
    bool action_kill_player     = false;   // instant gib
    bool action_teleport_origin = false;   // warp to map origin
    bool action_force_atmos     = false;   // rebuild atmos zones
    bool action_spawn_items     = false;   // scatter test items at player
};

class AdminMenu {
public:
    explicit AdminMenu(UIRenderer& ui);

    void open();
    void close();
    bool is_open() const { return m_open; }

    // Call each frame while open.  cursor = screen-space mouse position,
    // lmb_just_pressed = true only on the frame the left button goes down.
    AdminMenuResult draw(glm::vec2 cursor,
                         bool lmb_just_pressed,
                         bool escape_pressed,
                         const AdminMenuState& state);

private:
    // Toggle button — shows ON/OFF tag.  Returns true on click frame.
    bool draw_toggle_button(glm::vec2 pos, float w, float h,
                            const char* label, bool active,
                            glm::vec2 cursor, bool lmb_just_pressed);

    // Action button — no ON/OFF tag, distinct blue colour scheme.
    // Returns true on click frame.
    bool draw_action_button(glm::vec2 pos, float w, float h,
                            const char* label,
                            glm::vec2 cursor, bool lmb_just_pressed);

    // Small section label between button groups.
    void draw_section_label(glm::vec2 pos, float w, const char* text);

    UIRenderer& m_ui;
    bool        m_open = false;

    // Two-column layout dimensions
    static constexpr float PANEL_W   = 540.f;  // full panel width
    static constexpr float COL_W     = 248.f;  // each column's button width
    static constexpr float COL_GAP   = 12.f;   // gap between left & right col
    static constexpr float BTN_H     = 34.f;
    static constexpr float BTN_GAP   = 5.f;
    static constexpr float HEADER_H  = 38.f;
    static constexpr float PAD       = 10.f;
    static constexpr float SECT_H    = 18.f;   // section label height
    static constexpr float SECT_GAP  = 6.f;    // gap after a section label
};
