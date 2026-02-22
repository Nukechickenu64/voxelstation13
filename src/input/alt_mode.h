#pragma once
#include "core/types.h"
#include "input/input_manager.h"
#include <glm/glm.hpp>

// Manages the Alt-mode state transition:
//   - Releases OS cursor so the player can interact with the inventory overlay
//   - Freezes camera yaw/pitch during Alt-mode
//   - Re-captures cursor and restores camera on Alt release
class AltMode {
public:
    AltMode(InputManager& input, SDL_Window* window);

    // Call each logic tick; returns true if mode changed this tick
    bool update();

    bool active() const { return m_active; }

    // Fade alpha for UI overlay animation [0, 1]
    float overlay_alpha() const { return m_alpha; }

    // Frozen camera angles (only valid while active)
    float frozen_yaw()   const { return m_frozen_yaw; }
    float frozen_pitch() const { return m_frozen_pitch; }

    // Update the angles to freeze (call every frame outside alt-mode)
    void set_camera_angles(float yaw, float pitch);

    // Cursor position in screen space during alt-mode
    glm::vec2 cursor_pos() const;

private:
    InputManager& m_input;
    SDL_Window*   m_window;

    bool  m_active       = false;
    float m_alpha        = 0.f;    // fades in/out
    float m_frozen_yaw   = 0.f;
    float m_frozen_pitch = 0.f;

    static constexpr float FADE_SPEED = 8.f; // alpha units per second
};
