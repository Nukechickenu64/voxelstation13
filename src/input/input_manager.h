#pragma once
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <unordered_map>
#include <string>
#include <functional>

// Named actions mapped to SDL keys / mouse buttons
enum class Action : uint32_t {
    MoveForward, MoveBack, MoveLeft, MoveRight,
    Crouch, Sprint,
    PrimaryInteract, SecondaryInteract,
    PickUp, ThrowItem, DropItem,
    SwitchHand,
    ExamineMode,
    AltMode,
    ChatOpen,
    Escape,
    COUNT
};

struct InputState {
    bool held    = false;
    bool pressed = false;  // true only on first frame
    bool released= false;
};

class InputManager {
public:
    InputManager();

    // Call once per frame before game logic
    void begin_frame();

    // Feed raw SDL events from the event pump
    void process_event(const SDL_Event& e);

    // Action queries
    bool is_held    (Action a) const;
    bool is_pressed (Action a) const;
    bool is_released(Action a) const;

    // Mouse state (only meaningful when cursor is captured)
    glm::vec2 mouse_delta() const { return m_mouse_delta; }
    glm::vec2 mouse_pos()   const { return m_mouse_pos; }
    float     scroll_delta()const { return m_scroll_delta; }

    // Cursor capture control
    void capture_cursor(SDL_Window* window, bool capture);
    bool cursor_captured() const { return m_captured; }

private:
    std::unordered_map<Action, InputState> m_states;
    std::unordered_map<SDL_Scancode, Action> m_key_map;
    std::unordered_map<uint8_t, Action>      m_mouse_map;

    glm::vec2 m_mouse_delta{};
    glm::vec2 m_mouse_pos{};
    float     m_scroll_delta = 0.f;
    bool      m_captured     = false;

    void build_default_bindings();
    void update_action(Action a, bool down);
};
