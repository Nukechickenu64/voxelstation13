// ── gas_overlay.cpp ───────────────────────────────────────────────────────────
// F4 atmospherics visualisation overlay.
//
// Visual language:
//   Cell quads  — small coloured squares projected onto each tracked air cell.
//                 Colour encodes the zone's gas status (OK = faint teal,
//                 low-O2 = orange, low pressure = yellow, decompression = cyan,
//                 fire = red, toxic = magenta, space = dark blue).
//
//   Flow arrows — drawn at door-link midpoints when a pressure differential
//                 ≥ k_flow_threshold kPa exists between the two zones.
//                 Arrow points from the high-pressure zone toward the low-
//                 pressure zone; magnitude scales with the pressure gap.
//                 The arrows pulse in opacity so they "breathe".
//
//   Zone labels — total pressure (kPa) shown near each zone's centroid.
//
//   Legend      — colour key in the lower-left corner.
// ─────────────────────────────────────────────────────────────────────────────
#include "ui/gas_overlay.h"
#include <glm/gtc/matrix_transform.hpp>
#include <unordered_map>
#include <cmath>
#include <string>
#include <algorithm>

// ── Constructor ───────────────────────────────────────────────────────────────
GasOverlay::GasOverlay(UIRenderer& ui) : m_ui(ui) {}

// ── world_to_screen ───────────────────────────────────────────────────────────
bool GasOverlay::world_to_screen(glm::vec3 wp,
                                 const glm::mat4& vp,
                                 int w, int h,
                                 glm::vec2& out) const
{
    glm::vec4 clip = vp * glm::vec4(wp, 1.f);
    if (clip.w <= 0.f) return false;          // behind camera
    glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (ndc.z > 1.f || ndc.z < -1.f) return false;  // outside depth range
    out.x = (ndc.x * 0.5f + 0.5f) * float(w);
    out.y = (0.5f - ndc.y * 0.5f) * float(h);
    return true;
}

// ── zone_color ────────────────────────────────────────────────────────────────
glm::vec4 GasOverlay::zone_color(const AtmosZone& z) const
{
    if (z.is_space)                         return {0.10f, 0.10f, 0.40f, 0.35f};
    if (z.status & ATMOS_FIRE)              return {1.00f, 0.20f, 0.00f, 0.60f};
    if (z.status & ATMOS_HIGH_TEMP)         return {1.00f, 0.50f, 0.00f, 0.55f};
    if (z.status & ATMOS_DECOMP)            return {0.00f, 1.00f, 1.00f, 0.60f};
    if (z.status & ATMOS_TOXIC)             return {0.80f, 0.00f, 0.80f, 0.55f};
    if (z.status & ATMOS_LOW_PRESS)         return {1.00f, 0.85f, 0.00f, 0.45f};
    if (z.status & ATMOS_LOW_O2)            return {1.00f, 0.45f, 0.00f, 0.45f};
    if (z.status & ATMOS_HIGH_CO2)          return {0.40f, 0.90f, 0.40f, 0.40f};
    // Normal breathable air — very subtle teal tint
    return {0.30f, 0.85f, 1.00f, 0.12f};
}

// ── draw ──────────────────────────────────────────────────────────────────────
void GasOverlay::draw(const AtmosSimulator& atmos,
                      const GasOverlayState& s,
                      double sim_time)
{
    const auto& cells = atmos.all_cells();
    if (cells.empty()) return;

    // ── 1. Compute per-zone centroids ─────────────────────────────────────────
    std::unordered_map<AtmosZoneID, glm::vec3> zone_sum;
    std::unordered_map<AtmosZoneID, int>       zone_cnt;
    zone_sum.reserve(64);
    zone_cnt.reserve(64);
    for (const auto& [pos, zid] : cells) {
        // Distance pre-cull (use squared distance for speed)
        glm::vec3 wpos = glm::vec3(pos) + glm::vec3(0.5f);
        float dx = wpos.x - s.cam_pos.x;
        float dy = wpos.y - s.cam_pos.y;
        float dz = wpos.z - s.cam_pos.z;
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 > k_draw_radius * k_draw_radius) continue;
        zone_sum[zid] += wpos;
        zone_cnt[zid]++;
    }
    for (auto& [zid, sum] : zone_sum) {
        auto it = zone_cnt.find(zid);
        if (it != zone_cnt.end() && it->second > 0)
            sum /= float(it->second);
    }

    // ── 2. Cell overlays ─────────────────────────────────────────────────────
    const float inv_radius = 1.f / k_draw_radius;
    for (const auto& [pos, zid] : cells) {
        glm::vec3 wpos = glm::vec3(pos) + glm::vec3(0.5f);
        float dist = glm::length(wpos - s.cam_pos);
        if (dist > k_draw_radius) continue;

        glm::vec2 sp;
        if (!world_to_screen(wpos, s.view_proj, s.fb_w, s.fb_h, sp)) continue;

        const AtmosZone* z = atmos.zone(zid);
        if (!z) continue;
        if (z->is_space) continue;  // skip: millions of void cells would overflow the vertex buffer

        glm::vec4 col = zone_color(*z);
        // Fade with distance
        float fade = 1.f - (dist * inv_radius);
        col.a *= fade * fade;   // squared for sharper centre falloff
        if (col.a < 0.02f) continue;

        // Pixel size scales slightly with distance (further = bigger for readability)
        float half = std::max(4.f, 9.f - dist * 0.35f);
        m_ui.rect(sp - glm::vec2(half), glm::vec2(half * 2.f), col, half * 0.5f);
    }

    // ── 3. Zone pressure labels ───────────────────────────────────────────────
    for (const auto& [zid, center] : zone_sum) {
        const AtmosZone* z = atmos.zone(zid);
        if (!z || z->is_space) continue;

        float dist = glm::length(center - s.cam_pos);
        if (dist > k_draw_radius * 0.9f) continue;

        glm::vec2 sp;
        if (!world_to_screen(center, s.view_proj, s.fb_w, s.fb_h, sp)) continue;

        float pressure = z->gas.total_pressure();
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f kPa", pressure);
        std::string label(buf);

        float alpha = 1.f - (dist / (k_draw_radius * 0.9f));
        alpha = alpha * alpha;
        if (alpha < 0.05f) continue;

        // Background pill
        float tw = float(label.size()) * 6.6f + 8.f;
        m_ui.rect(sp - glm::vec2(tw * 0.5f, 9.f),
                  {tw, 16.f},
                  {0.f, 0.f, 0.f, 0.55f * alpha},
                  3.f);
        glm::vec4 tc = zone_color(*z);
        tc.a = alpha;
        // Ensure label is bright enough
        if (tc.r < 0.4f && tc.g < 0.4f && tc.b < 0.4f) tc = {0.8f, 0.8f, 0.8f, alpha};
        m_ui.text(sp - glm::vec2(tw * 0.5f - 4.f, 7.f), label, tc, 11.f);
    }

    // ── 4. Flow arrows at door links ─────────────────────────────────────────
    // Pulse factor: arrows "breathe" over ~1.5 s
    float pulse = 0.75f + 0.25f * float(std::sin(sim_time * 4.18879));  // 2π/1.5 ≈ 4.18879

    for (const auto& link : atmos.door_links()) {
        const AtmosZone* za = atmos.zone(link.zone_a);
        const AtmosZone* zb = atmos.zone(link.zone_b);
        if (!za || !zb) continue;

        float pa = za->gas.total_pressure();
        float pb = zb->gas.total_pressure();
        float diff = pa - pb;
        if (std::abs(diff) < k_flow_threshold) continue;

        // Position arrow slightly above floor (y+0.5)
        glm::vec3 mid_world = link.midpoint + glm::vec3(0.f, 0.5f, 0.f);
        float dist = glm::length(mid_world - s.cam_pos);
        if (dist > k_arrow_radius) continue;

        glm::vec2 mid_sp;
        if (!world_to_screen(mid_world, s.view_proj, s.fb_w, s.fb_h, mid_sp)) continue;

        // Determine flow direction in screen space: from high-P zone centroid to low-P zone centroid
        AtmosZoneID src_id = (diff > 0.f) ? link.zone_a : link.zone_b;
        AtmosZoneID dst_id = (diff > 0.f) ? link.zone_b : link.zone_a;

        auto src_it = zone_sum.find(src_id);
        auto dst_it = zone_sum.find(dst_id);
        if (src_it == zone_sum.end() || dst_it == zone_sum.end()) continue;

        glm::vec2 src_sp, dst_sp;
        bool src_ok = world_to_screen(src_it->second + glm::vec3(0.f, 0.5f, 0.f),
                                      s.view_proj, s.fb_w, s.fb_h, src_sp);
        bool dst_ok = world_to_screen(dst_it->second + glm::vec3(0.f, 0.5f, 0.f),
                                      s.view_proj, s.fb_w, s.fb_h, dst_sp);

        // Fall back to a simple downward arrow if zone centers aren't visible
        glm::vec2 flow_dir2d = {0.f, 1.f};
        if (src_ok && dst_ok) {
            glm::vec2 d = dst_sp - src_sp;
            float len = glm::length(d);
            if (len > 0.5f) flow_dir2d = d / len;
        }

        // Arrow length scales with pressure difference, clamped to screen pixels
        float mag   = std::min(std::abs(diff) * 0.6f, 30.f);
        float alpha = pulse * std::min(1.f, std::abs(diff) / 20.f);
        alpha      *= (1.f - dist / k_arrow_radius);
        if (alpha < 0.05f) continue;

        // Colour: cyan for decompression flow, orange/yellow for other imbalances
        glm::vec4 arrow_col;
        if ((za->status & ATMOS_DECOMP) || (zb->status & ATMOS_DECOMP))
            arrow_col = {0.0f, 1.0f, 1.0f, alpha};
        else
            arrow_col = {1.0f, 0.65f, 0.0f, alpha};

        // Draw arrow shaft
        glm::vec2 tip  = mid_sp + flow_dir2d * mag;
        glm::vec2 tail = mid_sp - flow_dir2d * (mag * 0.4f);
        m_ui.line(tail, tip, arrow_col, 2.f);

        // Arrowhead (two short lines from tip)
        glm::vec2 perp   = {-flow_dir2d.y, flow_dir2d.x};
        float     head   = std::max(5.f, mag * 0.4f);
        m_ui.line(tip, tip - flow_dir2d * head + perp * (head * 0.45f), arrow_col, 2.f);
        m_ui.line(tip, tip - flow_dir2d * head - perp * (head * 0.45f), arrow_col, 2.f);

        // Optional: a small pressure-diff label near the arrow
        if (std::abs(diff) >= 5.f && dist < k_arrow_radius * 0.6f) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%.0f", std::abs(diff));
            glm::vec2 lbl_pos = mid_sp + glm::vec2(5.f, -8.f);
            float tw = float(std::string(buf).size() * 6 + 6);
            m_ui.rect(lbl_pos - glm::vec2(0.f, 2.f), {tw, 14.f},
                      {0.f, 0.f, 0.f, 0.5f * alpha}, 2.f);
            m_ui.text(lbl_pos, buf, arrow_col, 11.f);
        }
    }

    // ── 5. Legend ─────────────────────────────────────────────────────────────
    draw_legend(s.fb_w, s.fb_h);
}

// ── draw_legend ───────────────────────────────────────────────────────────────
void GasOverlay::draw_legend(int fb_w, int fb_h) const
{
    (void)fb_w;
    struct Entry { const char* label; glm::vec4 col; };
    static constexpr Entry entries[] = {
        { "Normal air",    {0.30f, 0.85f, 1.00f, 0.80f} },
        { "Low O2",        {1.00f, 0.45f, 0.00f, 0.90f} },
        { "Low pressure",  {1.00f, 0.85f, 0.00f, 0.90f} },
        { "Decompressing", {0.00f, 1.00f, 1.00f, 0.90f} },
        { "Fire",          {1.00f, 0.20f, 0.00f, 0.90f} },
        { "Toxic",         {0.80f, 0.00f, 0.80f, 0.90f} },
        { "Space",         {0.20f, 0.20f, 0.70f, 0.90f} },
        { "Flow arrow",    {1.00f, 0.65f, 0.00f, 0.90f} },
    };
    constexpr int N = static_cast<int>(sizeof(entries) / sizeof(entries[0]));

    constexpr float PAD     = 8.f;
    constexpr float LINE_H  = 16.f;
    constexpr float SWATCH  = 10.f;
    constexpr float FONT_SZ = 11.f;

    float panel_h = PAD * 2.f + N * LINE_H;
    float panel_w = 130.f;
    float x0      = float(PAD);
    float y0      = float(fb_h) - panel_h - PAD;

    // Background panel
    m_ui.rect({x0, y0}, {panel_w, panel_h}, {0.f, 0.f, 0.f, 0.60f}, 5.f);

    // Title
    m_ui.text({x0 + PAD, y0 + PAD - 2.f}, "GAS OVERLAY [F4]",
              {0.9f, 0.9f, 0.9f, 1.f}, FONT_SZ);

    for (int i = 0; i < N; ++i) {
        float ey = y0 + PAD + (i + 1) * LINE_H;
        m_ui.rect({x0 + PAD, ey}, {SWATCH, SWATCH},
                  entries[i].col, 2.f);
        m_ui.text({x0 + PAD + SWATCH + 5.f, ey},
                  entries[i].label,
                  {0.85f, 0.85f, 0.85f, 1.f},
                  FONT_SZ);
    }
}
