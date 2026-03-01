#pragma once
#include "render/ui_renderer.h"
#include <glm/glm.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// MainMenu — full-screen pre-game menu with Play and Exit buttons.
// ─────────────────────────────────────────────────────────────────────────────

struct MainMenuResult {
    bool play_clicked         = false;
    bool char_create_clicked  = false;  // TG SS13-style "Setup Character" button
    bool exit_clicked         = false;
};

class MainMenu {
public:
    explicit MainMenu(UIRenderer& ui);

    // Call each frame.  cursor = screen-space mouse pos.
    // lmb = true only on the frame the left button goes down.
    MainMenuResult draw(glm::vec2 cursor, bool lmb);

    // Advance the shimmer/pulse animation time (seconds since menu opened).
    void tick(double dt) { m_time += dt; }

private:
    bool draw_button(glm::vec2 pos, float w, float h,
                     const char* label,
                     glm::vec2 cursor, bool lmb);

    UIRenderer& m_ui;
    double      m_time = 0.0;

    static constexpr float BTN_W   = 260.f;
    static constexpr float BTN_H   = 48.f;
    static constexpr float BTN_GAP  = 12.f;
};
