#pragma once
#include "render/ui_renderer.h"
#include <glm/glm.hpp>
#include <string>

// ── PlayerStatsOverlayState ──────────────────────────────────────────────────
// Data fed to the player-stats debug screen (F6).
// Populated each frame by main.cpp from HealthComponent,
// CharacterControllerComponent, HUDState, and the active inventory slot.
struct PlayerStatsOverlayState {
    // ── Identity
    std::string species    = "human";
    std::string variant    = "female";

    // ── Health (four SS13 damage buckets)
    float health_max  = 100.f;
    float dmg_brute   = 0.f;
    float dmg_burn    = 0.f;
    float dmg_tox     = 0.f;
    float dmg_oxy     = 0.f;
    bool  dead        = false;

    // ── Suit sensors (mirrors HUDState)
    float oxy_sat           = 1.f;    // 0-1
    float suit_pressure_kpa = 101.325f;
    float tox_level         = 0.f;    // kPa of combined toxins
    std::string suit_temp_str;

    // ── Controller / movement
    float move_speed  = 4.5f;
    float sprint_mult = 1.8f;
    float jump_vel    = 6.f;
    float height      = 0.9f;
    float radius      = 0.3f;
    bool  on_ground         = false;
    bool  sprinting         = false;
    bool  zero_g            = false;
    bool  noclip            = false;
    bool  jetpack_equipped  = false;
    bool  grab_wall         = false;

    // ── Active hand item
    std::string active_hand_name;  // empty = nothing held
    std::string active_hand_id;    // "l_hand" / "r_hand"
};

// ── PlayerStatsOverlay ───────────────────────────────────────────────────────
// F6 debug screen: human (player) mob stats.
// Toggle visibility in main.cpp; call draw() every frame while visible.
class PlayerStatsOverlay {
public:
    explicit PlayerStatsOverlay(UIRenderer& ui);

    void draw(const PlayerStatsOverlayState& state);

private:
    // Emit one text pill (same helper style as DebugOverlay).
    void emit(glm::vec2 pos, const std::string& str, glm::vec4 col);

    // Draw a horizontal damage bar and return the next Y position.
    float draw_bar(float x, float y,
                   const std::string& label, float value, float max_value,
                   glm::vec4 fill_col);

    UIRenderer& m_ui;

    static constexpr float k_font_size  = 12.f;
    static constexpr float k_line_h     = 14.f;
    static constexpr float k_margin     = 4.f;
    static constexpr float k_text_pad_x = 3.f;
    static constexpr float k_text_pad_y = 1.f;
    static constexpr float k_bar_w      = 140.f;   // width of damage bar track
    static constexpr float k_bar_h      = 8.f;     // height of bar
};
