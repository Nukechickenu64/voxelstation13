#include "input/alt_mode.h"
#include <algorithm>

AltMode::AltMode(InputManager& input, SDL_Window* window)
    : m_input(input)
    , m_window(window)
{}

bool AltMode::update()
{
    bool was_active = m_active;
    bool want_active = m_input.is_held(Action::AltMode);

    if (want_active && !m_active) {
        m_active = true;
        m_input.capture_cursor(m_window, false); // release cursor
    } else if (!want_active && m_active) {
        m_active = false;
        m_input.capture_cursor(m_window, true);  // re-capture
    }

    // Smooth fade
    float target = m_active ? 1.f : 0.f;
    float step   = FADE_SPEED * (1.f / 60.f); // assume ~60 Hz
    if (m_alpha < target) m_alpha = std::min(m_alpha + step, target);
    else                   m_alpha = std::max(m_alpha - step, target);

    return m_active != was_active;
}

void AltMode::set_camera_angles(float yaw, float pitch)
{
    if (!m_active) {
        m_frozen_yaw   = yaw;
        m_frozen_pitch = pitch;
    }
}

glm::vec2 AltMode::cursor_pos() const
{
    return m_input.mouse_pos();
}
