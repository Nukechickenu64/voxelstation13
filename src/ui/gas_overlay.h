#pragma once
#include "render/ui_renderer.h"
#include "simulation/atmos.h"
#include <glm/glm.hpp>

// ── GasOverlayState ───────────────────────────────────────────────────────────
// Render-frame data fed into the gas overlay each frame.
struct GasOverlayState {
    glm::mat4 view_proj{};   // full VP matrix from the renderer
    glm::vec3 cam_pos{};     // world-space camera position
    int       fb_w = 0;
    int       fb_h = 0;
};

// ── GasOverlay ────────────────────────────────────────────────────────────────
// F4 gas visualisation overlay.  Projects each atmos cell onto the screen and
// draws a translucent colour quad indicating zone status, plus animated flow
// arrows at every door-link boundary where a significant pressure difference
// exists.  A colour legend is shown in the lower-left corner.
//
// All geometry is emitted into UIRenderer (immediate-mode 2-D quads/lines).
class GasOverlay {
public:
    explicit GasOverlay(UIRenderer& ui);

    // Call inside the UI pass (after ui_renderer.begin(), before end()).
    void draw(const AtmosSimulator& atmos,
              const GasOverlayState& state,
              double sim_time);   // monotonic time in seconds (for animations)

private:
    // Project a world position to screen pixels.
    // Returns false when the point is behind the camera or outside the NDC cube.
    bool world_to_screen(glm::vec3 wp,
                         const glm::mat4& vp,
                         int w, int h,
                         glm::vec2& out) const;

    // Pick a semi-transparent colour for a zone based on its status flags.
    glm::vec4 zone_color(const AtmosZone& z) const;

    void draw_legend(int fb_w, int fb_h) const;

    UIRenderer& m_ui;

    // How far from the camera to draw cell overlays (world units).
    static constexpr float k_draw_radius    = 18.f;
    // How far to draw door-link arrows.
    static constexpr float k_arrow_radius   = 30.f;
    // Minimum pressure differential (kPa) to show a flow arrow.
    static constexpr float k_flow_threshold = 1.0f;
};
