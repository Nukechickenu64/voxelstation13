#pragma once
#include "render/ui_renderer.h"
#include "simulation/atmos.h"
#include <glm/glm.hpp>
#include <string>

// ── DebugOverlayState ──────────────────────────────────────────────────────────
// All data fed into the debug overlay each frame.
struct DebugOverlayState {
    // ── Renderer / performance
    double      fps         = 0.0;
    double      frame_ms    = 0.0;
    uint64_t    tick_count  = 0;

    // ── Camera / player transform
    glm::vec3   cam_pos{};
    float       yaw    = 0.f;  // degrees
    float       pitch  = 0.f;  // degrees

    // ── Velocity (from VelocityComponent when available)
    glm::vec3   velocity{};

    // ── Ray-hit target
    bool        has_hit       = false;
    glm::ivec3  hit_voxel{};
    std::string hit_type_name;

    // ── Player controller state
    bool        noclip    = false;
    bool        zero_g    = false;
    bool        on_ground = false;

    // ── Build mode
    bool        build_mode      = false;
    std::string build_type_name;

    // ── Active hand ("Left" / "Right")
    std::string active_hand;

    // ── Atmosphere at player position
    AtmosZoneID zone_id      = 0;
    GasMixture  gas_mix{};
    uint8_t     atmos_status = 0;   // AtmosStatus bitmask
    float       pressure_loss_rate = 0.f;  // kPa/s drained to space

    // ── Room info
    int  room_cell_count = 0;
    int  room_adj_count  = 0;
    int  total_rooms     = 0;

    // ── Enclosure
    bool enclosed = false;
};

// ── DebugOverlay ──────────────────────────────────────────────────────────────
// Minecraft-style F-key debug screen.  Toggle visibility in main.cpp with F5.
// Call draw() each frame while visible; it is a pure UI pass on UIRenderer.
class DebugOverlay {
public:
    explicit DebugOverlay(UIRenderer& ui);

    void draw(const DebugOverlayState& state);

private:
    void draw_left_column (const DebugOverlayState& s);
    void draw_right_column(const DebugOverlayState& s);

    // Emit one line of text over a translucent background pill.
    // `x` is the left edge; the background widens to fit the text.
    void emit(glm::vec2 pos, const std::string& str, glm::vec4 col);

    UIRenderer& m_ui;

    static constexpr float k_font_size  = 12.f;
    static constexpr float k_line_h     = 14.f;   // vertical advance per line
    static constexpr float k_margin     = 4.f;    // screen-edge margin
    static constexpr float k_text_pad_x = 3.f;    // horizontal text padding inside pill
    static constexpr float k_text_pad_y = 1.f;    // vertical   text padding inside pill
};
