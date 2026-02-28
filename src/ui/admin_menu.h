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
    bool noclip        = false;
    bool build_mode    = false;
    bool gas_overlay   = false;
    bool debug_overlay = false;
    bool player_stats  = false;
    bool verbose_log   = false;
};

// Returned by draw() — each flag is true for exactly one frame when the
// corresponding button is clicked.
struct AdminMenuResult {
    bool toggle_noclip        = false;
    bool toggle_build_mode    = false;
    bool toggle_gas_overlay   = false;
    bool toggle_debug_overlay = false;
    bool toggle_player_stats  = false;
    bool toggle_verbose_log   = false;
    bool close_requested      = false;
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
    // Returns true if this button was clicked this frame.
    bool draw_toggle_button(glm::vec2 pos, float w, float h,
                            const char* label, bool active,
                            glm::vec2 cursor, bool lmb_just_pressed);

    UIRenderer& m_ui;
    bool        m_open = false;

    static constexpr float PANEL_W   = 260.f;
    static constexpr float BTN_H     = 34.f;
    static constexpr float BTN_GAP   = 6.f;
    static constexpr float HEADER_H  = 38.f;
    static constexpr float PAD       = 10.f;
};
