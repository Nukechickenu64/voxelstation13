#pragma once
#include "render/ui_renderer.h"
#include "inventory/inventory.h"
#include "simulation/atmos.h"
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <string>
#include <deque>

struct HUDState {
    float       health      = 100.f;
    float       health_max  = 100.f;
    float       oxy_sat     = 1.f;      // 0-1
    float       tox_level   = 0.f;
    float       suit_pressure_kpa = 101.325f;
    std::string suit_temp_str;
    std::string clock_str;
    std::string examine_label;          // label over targeted face
    bool        active_hand_is_left = false;
    float       cam_pitch = 0.f;        // degrees, negative = looking down
    std::deque<std::string> radio_log;  // capped at ~30 entries
};

// Draws the always-on HUD elements.
class HUD {
public:
    explicit HUD(UIRenderer& ui);

    void draw(const HUDState& state, const Inventory& inv, int hotbar_slot);

private:
    void draw_health_bar   (const HUDState& s);
    void draw_suit_sensors (const HUDState& s);
    void draw_hotbar       (const Inventory& inv, int active_slot);
    void draw_hands        (const Inventory& inv, bool left_active, float pitch);
    void draw_examine_label(const std::string& label);
    void draw_radio_log    (const std::deque<std::string>& log);
    void draw_clock        (const std::string& time_str);

    UIRenderer&       m_ui;
    SDL_GPUTexture*   m_hand_tex = nullptr;  // textures/worldui/hand.png
};
