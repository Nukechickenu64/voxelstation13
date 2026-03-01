#pragma once
#include "render/ui_renderer.h"
#include <glm/glm.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// GameSettings — all player-configurable settings in one struct.
// main.cpp owns an instance; each frame the values are applied to the
// relevant subsystems (renderer, input, etc.).
// ─────────────────────────────────────────────────────────────────────────────
struct GameSettings {
    // ── Graphics ──────────────────────────────────────────────────────────────
    float fov               = 90.f;    // degrees [60, 120]
    bool  ambient_occlusion = true;
    bool  fullbright        = false;
    bool  wireframe         = false;
    bool  psx_wobble        = false;
    float psx_snap_res      = 160.f;  // grid cells per half-NDC axis [20, 320]; lower = more drastic
    bool  psx_yshear        = false;
    bool  affine_enabled    = false;
    float affine_mix        = 1.0f;   // blend [0..1]; 1 = fully affine (PS1-style)

    // ── Audio ──────────────────────────────────────────────────────────────────
    float master_volume     = 1.0f;    // [0, 1]
    float sfx_volume        = 1.0f;    // [0, 1]
    float music_volume      = 0.5f;    // [0, 1] — placeholder

    // ── Controls ──────────────────────────────────────────────────────────────
    float mouse_sensitivity = 0.15f;   // [0.02, 0.50]
    bool  invert_mouse_y    = false;

    // ── HUD / Gameplay ─────────────────────────────────────────────────────────
    bool  show_debug_overlay = false;
    bool  show_gas_overlay   = false;
    bool  show_player_stats  = false;
    bool  show_crosshair     = true;
    bool  show_radio_log     = true;
    bool  third_person       = false;
    bool  freeze_sim         = false;
};

// ─────────────────────────────────────────────────────────────────────────────
struct PauseMenuResult {
    bool resume_clicked       = false;
    bool exit_to_main_clicked = false;
    bool exit_game_clicked    = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// PauseMenu — in-game pause overlay with a full tabbed settings screen.
//
// Press Escape in-game to open; Escape/Resume to close.
// ─────────────────────────────────────────────────────────────────────────────
class PauseMenu {
public:
    explicit PauseMenu(UIRenderer& ui);

    void open();
    void close();
    bool is_open() const { return m_open; }

    // Draw the pause menu overlay for one render frame.
    //   cursor      — current (un-captured) mouse position
    //   lmb_pressed — true on the single frame the left button is first pressed
    //   lmb_held    — true while left button is held (used for slider dragging)
    //   esc_pressed — true on the single frame Escape is pressed
    //   settings    — modified in-place by any slider or toggle interaction
    PauseMenuResult draw(glm::vec2 cursor, bool lmb_pressed, bool lmb_held,
                         bool esc_pressed, GameSettings& settings);

private:
    // ── Widget helpers ─────────────────────────────────────────────────────────

    // Draw a standard button; returns true on the click frame.
    bool draw_button(glm::vec2 pos, float w, float h,
                     const char* label, glm::vec2 cursor, bool lmb,
                     bool warn_style = false);

    // Draw a tab selector button at pos; returns true if it was clicked.
    bool draw_tab_btn(glm::vec2 pos, float w, float h,
                      const char* label, bool active,
                      glm::vec2 cursor, bool lmb);

    // Draw a labelled slider row; returns the (possibly updated) value.
    // slider_id must be unique across all sliders drawn in one frame.
    float draw_slider(glm::vec2 row_pos, float row_w,
                      const char* label, float value,
                      float vmin, float vmax, const char* fmt,
                      glm::vec2 cursor, bool lmb_pressed, bool lmb_held,
                      int slider_id);

    // Draw a labelled ON/OFF toggle row; returns true if it was clicked.
    bool draw_toggle(glm::vec2 row_pos, float row_w,
                     const char* label, bool value,
                     glm::vec2 cursor, bool lmb);

    // Draw a thin decorative section separator line.
    void draw_separator(glm::vec2 pos, float w);

    // ── State ──────────────────────────────────────────────────────────────────
    UIRenderer& m_ui;

    bool m_open        = false;
    bool m_in_settings = false;

    enum class SettingsTab { Graphics = 0, Audio = 1, Controls = 2, Gameplay = 3 };
    SettingsTab m_tab = SettingsTab::Graphics;

    int m_drag_slider = -1;   // ID of the slider currently being dragged; -1 = none

    // ── Layout constants ───────────────────────────────────────────────────────
    static constexpr float PANEL_W          = 480.f;
    static constexpr float PANEL_H_PAUSE    = 355.f;
    static constexpr float PANEL_H_SETTINGS = 510.f;
    static constexpr float BTN_W            = 300.f;
    static constexpr float BTN_H            = 46.f;
    static constexpr float BTN_GAP          = 12.f;
    static constexpr float ROW_H            = 38.f;
};
