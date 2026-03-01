#pragma once
#include "render/ui_renderer.h"
#include "inventory/inventory.h"
#include "simulation/atmos.h"
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <string>
#include <deque>

// ── TG-style intent ───────────────────────────────────────────────────────────
enum class Intent : uint8_t {
    Help    = 0,   // green  — default, non-aggressive
    Disarm  = 1,   // blue   — shove/disarm
    Grab    = 2,   // yellow — grab mobs/objects
    Harm    = 3,   // red    — attack
};

// ── Targeting zone (body part selector) ──────────────────────────────────────
enum class BodyZone : uint8_t {
    Chest  = 0,   // default
    Head   = 1,
    LArm   = 2,
    RArm   = 3,
    LLeg   = 4,
    RLeg   = 5,
    Groin  = 6,
};

struct HUDState {
    float       health      = 100.f;
    float       health_max  = 100.f;
    float       oxy_sat     = 1.f;       // 0-1
    float       tox_level   = 0.f;
    float       suit_pressure_kpa = 101.325f;
    std::string suit_temp_str;
    std::string clock_str;
    std::string examine_label;           // label over targeted face
    bool        active_hand_is_left = false;
    float       cam_pitch = 0.f;         // degrees, negative = looking down
    std::deque<std::string> radio_log;   // capped at ~30 entries

    // TG-specific combat state
    Intent   intent      = Intent::Help;
    BodyZone target_zone = BodyZone::Chest;
};

// Draws the always-on TG-style HUD in a unified bottom bar.
//
// Returns the slot ID that was clicked this frame (empty string if none).
// The caller should call player_inv.swap(returned_id, active_hand_id).
class HUD {
public:
    explicit HUD(UIRenderer& ui);

    // mouse_pos   — current cursor position (pass {-9999,-9999} when cursor captured)
    // lmb_clicked — true on the frame the primary mouse button is pressed
    std::string draw(HUDState& state, const Inventory& inv,
                     glm::vec2 mouse_pos, bool lmb_clicked);

private:
    // ── Unified bottom bar sections ────────────────────────────────────────
    void draw_status_section    (const HUDState& s,           // leftmost
                                 glm::vec2 bar_tl, float bar_h);
    void draw_body_equip        (const Inventory& inv,        // head/suit cluster
                                 glm::vec2 origin,
                                 glm::vec2 mouse, bool click,
                                 std::string& out_click);
    void draw_hand_slots        (const Inventory& inv,        // center
                                 bool left_active,
                                 glm::vec2 origin,
                                 glm::vec2 mouse, bool click,
                                 std::string& out_click);
    void draw_storage_equip     (const Inventory& inv,        // back/belt/pockets
                                 glm::vec2 origin,
                                 glm::vec2 mouse, bool click,
                                 std::string& out_click);
    void draw_intent_zone       (HUDState& s,                 // rightmost
                                 glm::vec2 zone_origin,
                                 glm::vec2 intent_origin,
                                 glm::vec2 mouse, bool click);
    void draw_examine_label     (const std::string& label);
    void draw_radio_log         (const std::deque<std::string>& log);
    void draw_clock             (const std::string& time_str);

    // Draw one slot box; returns true if it was clicked.
    bool draw_slot(const Inventory& inv, const char* slot_id,
                   glm::vec2 pos, float sz,
                   const char* fallback_label,
                   bool highlight_active,   // blue ring when true
                   glm::vec2 mouse, bool click);

    UIRenderer& m_ui;
};
