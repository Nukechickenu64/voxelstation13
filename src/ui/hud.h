#pragma once
#include "render/ui_renderer.h"
#include "inventory/inventory.h"
#include "simulation/atmos.h"
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <string>
#include <deque>
#include <unordered_map>

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

    // SS13-style damage buckets (from HealthComponent)
    float dmg_brute = 0.f;   // physical / melee / explosion
    float dmg_burn  = 0.f;   // heat / electrical / laser
    float dmg_tox   = 0.f;   // toxin / plasma gas / poison
    float dmg_oxy   = 0.f;   // oxygen deprivation / asphyxia
    float dmg_clone = 0.f;   // radiation / mutagenic
    bool  dead      = false;
    bool  in_crit   = false;

    // TG-specific combat state
    Intent   intent      = Intent::Help;
    BodyZone target_zone = BodyZone::Chest;
    bool     is_running  = false;   // walk/run toggle (ui_movi)
    bool     is_pulling  = false;   // currently dragging/pulling something (shows pull.png)
    bool     inv_open    = false;   // body-slot panel expanded
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
    // player_mirror_tex — assembled front-facing sprite from Renderer (may be nullptr)
    std::string draw(HUDState& state, const Inventory& inv,
                     glm::vec2 mouse_pos, bool lmb_clicked,
                     SDL_GPUTexture* player_mirror_tex = nullptr);

private:
    // ── Right-side floating health doll panel (TG: EAST-1, CENTER) ────────
    void draw_health_panel      (const HUDState& s, glm::vec2 origin);

    // ── Bottom bar sections ────────────────────────────────────────────────
    void draw_hand_slots        (const Inventory& inv,        // center
                                 bool left_active,
                                 glm::vec2 origin,
                                 glm::vec2 mouse, bool click,
                                 std::string& out_click);
    // TG bottom-right: zone selector doll + intent + movement intent buttons
    void draw_zone_intent       (HUDState& s,
                                 glm::vec2 mouse, bool click);
    void draw_body_equip        (const Inventory& inv,
                                 glm::vec2 panel_tl,
                                 glm::vec2 mouse, bool click,
                                 std::string& out_click);
    void draw_examine_label     (const std::string& label);
    void draw_radio_log         (const std::deque<std::string>& log);
    void draw_clock             (const std::string& time_str);
    // Draw the player mirror panel in the top-left corner.
    // sprite_tex: 32×32 RGBA assembled sprite; pass nullptr to show placeholder.
    void draw_mirror            (SDL_GPUTexture* sprite_tex);

    // Draw one slot box; returns true if it was clicked.
    bool draw_slot(const Inventory& inv, const char* slot_id,
                   glm::vec2 pos, float sz,
                   const char* fallback_label,
                   bool highlight_active,   // blue ring when true
                   glm::vec2 mouse, bool click);

    UIRenderer& m_ui;

    // ── Legacyset HUD sprites (loaded once in constructor) ─────────────────
    // Intent icons indexed by Intent enum value (Help=0, Disarm=1, Grab=2, Harm=3)
    SDL_GPUTexture* m_intent_tex[4]      = {};
    // Zone highlight overlays (chest=0, head=1, l_arm=2, r_arm=3, l_leg=4, r_leg=5, groin=6)
    SDL_GPUTexture* m_zone_sel_tex[7]    = {};
    // Body-part damage level sprites [zone][level 0-4]
    SDL_GPUTexture* m_zone_dmg_tex[7][5] = {};
    // Overall health-state background sprites [level 0-4]
    SDL_GPUTexture* m_living_tex[5]      = {};
    // Suit pressure sprites (empty=0, low=1, mid=2, high=3)
    SDL_GPUTexture* m_suit_tex[4]        = {};
    // Body-doll outline (zone_sel.png from screen_midnight) used by both dolls
    SDL_GPUTexture* m_doll_base_tex       = nullptr;
    // Generic slot background frame (template.png)
    SDL_GPUTexture* m_template_tex        = nullptr;
    // Active slot frame (template_active.png)
    SDL_GPUTexture* m_template_active_tex = nullptr;
    // Pull indicator (pull.png) shown in health panel
    SDL_GPUTexture* m_pull_tex            = nullptr;
    // Hand slot backgrounds and active-hand overlays (screen_midnight)
    SDL_GPUTexture* m_hand_l_tex          = nullptr;
    SDL_GPUTexture* m_hand_r_tex          = nullptr;
    SDL_GPUTexture* m_lhand_active_tex    = nullptr;
    SDL_GPUTexture* m_rhand_active_tex    = nullptr;
    // Movement intent sprites (screen_midnight)
    SDL_GPUTexture* m_walking_tex         = nullptr;
    SDL_GPUTexture* m_running_tex         = nullptr;
    // INV toggle sprites (screen_midnight)
    SDL_GPUTexture* m_toggle_tex          = nullptr;
    SDL_GPUTexture* m_toggle_active_tex   = nullptr;
    // Per-slot empty-state icons keyed by slot_id (screen_midnight)
    std::unordered_map<std::string, SDL_GPUTexture*> m_slot_icon_tex;
};
