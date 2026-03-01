#include "input/input_manager.h"
#include <SDL3/SDL.h>

InputManager::InputManager()
{
    // Initialise all action states
    for (int i = 0; i < static_cast<int>(Action::COUNT); ++i)
        m_states[static_cast<Action>(i)] = {};
    build_default_bindings();
}

void InputManager::build_default_bindings()
{
    m_key_map[SDL_SCANCODE_W]       = Action::MoveForward;
    m_key_map[SDL_SCANCODE_S]       = Action::MoveBack;
    m_key_map[SDL_SCANCODE_A]       = Action::MoveLeft;
    m_key_map[SDL_SCANCODE_D]       = Action::MoveRight;
    m_key_map[SDL_SCANCODE_SPACE]   = Action::SwitchHand;
    m_key_map[SDL_SCANCODE_LCTRL]   = Action::GrabWall;
    m_key_map[SDL_SCANCODE_LSHIFT]  = Action::Sprint;
    m_key_map[SDL_SCANCODE_E]       = Action::PickUp;
    m_key_map[SDL_SCANCODE_F]       = Action::ThrowItem;
    m_key_map[SDL_SCANCODE_Q]       = Action::DropItem;
    m_key_map[SDL_SCANCODE_TAB]     = Action::ExamineMode;
    m_key_map[SDL_SCANCODE_LALT]    = Action::AltMode;
    m_key_map[SDL_SCANCODE_ESCAPE]  = Action::Escape;
    m_key_map[SDL_SCANCODE_T]       = Action::ChatOpen;
    m_key_map[SDL_SCANCODE_SLASH]   = Action::ChatOpen;
    m_key_map[SDL_SCANCODE_C]       = Action::ToggleCamera;
    m_key_map[SDL_SCANCODE_Z]       = Action::Rest;

    m_mouse_map[SDL_BUTTON_LEFT]    = Action::PrimaryInteract;
    m_mouse_map[SDL_BUTTON_RIGHT]   = Action::SecondaryInteract;
}

void InputManager::begin_frame()
{
    m_mouse_delta  = {};
    m_scroll_delta = 0.f;
    // Always poll the real absolute cursor position so that hit-tests work
    // immediately after the cursor is uncaptured (without waiting for a
    // motion event to arrive).
    float mx = 0.f, my = 0.f;
    SDL_GetMouseState(&mx, &my);
    m_mouse_pos = {mx, my};
    for (auto& [a, s] : m_states) {
        s.pressed  = false;
        s.released = false;
    }
}

void InputManager::process_event(const SDL_Event& e)
{
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        auto it = m_key_map.find(e.key.scancode);
        if (it != m_key_map.end())
            update_action(it->second, e.type == SDL_EVENT_KEY_DOWN);
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        auto it = m_mouse_map.find(e.button.button);
        if (it != m_mouse_map.end())
            update_action(it->second, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        m_mouse_delta += glm::vec2(e.motion.xrel, e.motion.yrel);
        m_mouse_pos    = glm::vec2(e.motion.x,    e.motion.y);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        m_scroll_delta += e.wheel.y;
        break;
    default: break;
    }
}

void InputManager::update_action(Action a, bool down)
{
    auto& s = m_states[a];
    if (down) {
        if (!s.held) s.pressed = true;
        s.held = true;
    } else {
        if (s.held) s.released = true;
        s.held = false;
    }
}

bool InputManager::is_held    (Action a) const { return m_states.at(a).held;     }
bool InputManager::is_pressed (Action a) const { return m_states.at(a).pressed;  }
bool InputManager::is_released(Action a) const { return m_states.at(a).released; }
bool InputManager::consume_press(Action a) { auto& s = m_states.at(a); bool v = s.pressed; s.pressed = false; return v; }

void InputManager::capture_cursor(SDL_Window* window, bool capture)
{
    SDL_SetWindowRelativeMouseMode(window, capture);
    m_captured = capture;
}
