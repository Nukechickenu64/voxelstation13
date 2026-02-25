#include "ui/debug_overlay.h"
#include "simulation/atmos.h"   // AtmosStatus flags
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

// ── helpers ───────────────────────────────────────────────────────────────────
static std::string fmt_f(float v, int dec = 2)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(dec) << v;
    return ss.str();
}

static std::string fmt_f3(glm::vec3 v, int dec = 3)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(dec)
       << v.x << " / " << v.y << " / " << v.z;
    return ss.str();
}

// Cardinal + inter-cardinal direction from yaw (degrees, 0 = -Z = North).
static std::string cardinal_from_yaw(float yaw_deg)
{
    // Normalise to [0, 360)
    float y = std::fmod(yaw_deg, 360.f);
    if (y < 0.f) y += 360.f;

    // 8-segment compass
    if      (y <  22.5f || y >= 337.5f) return "North (-Z)";
    else if (y <  67.5f)                return "NE (+X/-Z)";
    else if (y < 112.5f)                return "East  (+X)";
    else if (y < 157.5f)                return "SE (+X/+Z)";
    else if (y < 202.5f)                return "South (+Z)";
    else if (y < 247.5f)                return "SW (-X/+Z)";
    else if (y < 292.5f)                return "West  (-X)";
    else                                return "NW (-X/-Z)";
}

// ── DebugOverlay ──────────────────────────────────────────────────────────────
DebugOverlay::DebugOverlay(UIRenderer& ui)
    : m_ui(ui)
{}

void DebugOverlay::emit(glm::vec2 pos, const std::string& str, glm::vec4 col)
{
    if (str.empty()) return;
    // Approximate text width: each character ≈ font_size * 0.55
    float text_w = static_cast<float>(str.size()) * k_font_size * 0.55f;
    float pill_w = text_w + k_text_pad_x * 2.f;
    float pill_h = k_font_size + k_text_pad_y * 2.f;
    m_ui.rect(pos, {pill_w, pill_h}, {0.f, 0.f, 0.f, 0.60f}, 2.f);
    m_ui.text(pos + glm::vec2(k_text_pad_x, k_text_pad_y), str, col, k_font_size);
}

void DebugOverlay::draw(const DebugOverlayState& state)
{
    draw_left_column(state);
    draw_right_column(state);
}

void DebugOverlay::draw_left_column(const DebugOverlayState& s)
{
    const float fb_w = static_cast<float>(m_ui.fb_width());
    (void)fb_w;

    float x = k_margin;
    float y = k_margin;
    auto next = [&]() { y += k_line_h; };
    auto skip = [&]() { y += k_line_h * 0.6f; };

    const glm::vec4 white   = {1.0f, 1.0f, 1.0f, 1.0f};
    const glm::vec4 yellow  = {1.0f, 1.0f, 0.4f, 1.0f};
    const glm::vec4 cyan    = {0.4f, 1.0f, 1.0f, 1.0f};
    const glm::vec4 green   = {0.5f, 1.0f, 0.5f, 1.0f};
    const glm::vec4 red     = {1.0f, 0.4f, 0.4f, 1.0f};
    const glm::vec4 orange  = {1.0f, 0.7f, 0.2f, 1.0f};
    const glm::vec4 grey    = {0.7f, 0.7f, 0.7f, 1.0f};

    // ── Header ────────────────────────────────────────────────────────────────
    emit({x, y}, "VoxelStation 13  [debug F5]", yellow); next();
    skip();

    // ── Performance ───────────────────────────────────────────────────────────
    {
        std::ostringstream ss;
        ss << "FPS: " << std::fixed << std::setprecision(1) << s.fps
           << "  (" << std::setprecision(2) << s.frame_ms << " ms)";
        emit({x, y}, ss.str(), green);
    }
    next();
    {
        std::ostringstream ss;
        ss << "Tick: " << s.tick_count;
        emit({x, y}, ss.str(), grey);
    }
    next();
    skip();

    // ── Position ──────────────────────────────────────────────────────────────
    emit({x, y}, "XYZ: " + fmt_f3(s.cam_pos), white); next();

    // Chunk position (16-voxel chunks on X and Z; Y is flat for now)
    int cx = static_cast<int>(std::floor(s.cam_pos.x / 16.f));
    int cy = static_cast<int>(std::floor(s.cam_pos.y / 16.f));
    int cz = static_cast<int>(std::floor(s.cam_pos.z / 16.f));
    {
        std::ostringstream ss;
        ss << "Chunk: " << cx << ", " << cy << ", " << cz;
        emit({x, y}, ss.str(), grey);
    }
    next();
    skip();

    // ── Facing ────────────────────────────────────────────────────────────────
    {
        std::ostringstream ss;
        ss << "Facing: " << cardinal_from_yaw(s.yaw)
           << "  Yaw: " << fmt_f(s.yaw, 1) << " deg"
           << "  Pitch: " << fmt_f(s.pitch, 1) << " deg";
        emit({x, y}, ss.str(), cyan);
    }
    next();
    skip();

    // ── Velocity ──────────────────────────────────────────────────────────────
    {
        float speed_xz = std::sqrt(s.velocity.x * s.velocity.x + s.velocity.z * s.velocity.z);
        std::ostringstream ss;
        ss << "Vel: " << fmt_f3(s.velocity, 2)
           << "  |xz|: " << fmt_f(speed_xz, 2);
        emit({x, y}, ss.str(), grey);
    }
    next();
    skip();

    // ── Target voxel ──────────────────────────────────────────────────────────
    if (s.has_hit) {
        std::ostringstream ss;
        ss << "Target: [" << s.hit_voxel.x << ", " << s.hit_voxel.y << ", " << s.hit_voxel.z
           << "]";
        if (!s.hit_type_name.empty())
            ss << " [" << s.hit_type_name << "]";
        emit({x, y}, ss.str(), orange);
    } else {
        emit({x, y}, "Target: -", grey);
    }
    next();
    skip();

    // ── Player flags ──────────────────────────────────────────────────────────
    auto bool_str = [](const char* label, bool v, glm::vec4& col,
                       glm::vec4 on_col, glm::vec4 off_col) -> std::string {
        col = v ? on_col : off_col;
        return std::string(label) + (v ? ": ON" : ": OFF");
    };

    glm::vec4 nc_col, zg_col, gr_col;
    emit({x, y}, bool_str("Noclip",   s.noclip,    nc_col, red,   grey), nc_col); next();
    emit({x, y}, bool_str("Zero-G",   s.zero_g,    zg_col, orange, grey), zg_col); next();
    emit({x, y}, bool_str("Grounded", s.on_ground, gr_col, green, grey), gr_col); next();
    skip();

    // ── Build mode ────────────────────────────────────────────────────────────
    {
        std::string bm_str = s.build_mode ? "Build Mode: ON" : "Build Mode: OFF";
        if (s.build_mode && !s.build_type_name.empty())
            bm_str += "  [" + s.build_type_name + "]";
        glm::vec4 bm_col = s.build_mode ? yellow : grey;
        emit({x, y}, bm_str, bm_col);
    }
    next();

    emit({x, y}, "Active hand: " + s.active_hand, white); next();
}

void DebugOverlay::draw_right_column(const DebugOverlayState& s)
{
    const float fb_w = static_cast<float>(m_ui.fb_width());

    // Right column starts so its pills end near the right edge.
    // We approximate max line width and anchor from the right.
    // Use a fixed offset from the right edge:
    const float col_w = 200.f;
    float x = fb_w - col_w - k_margin;
    float y = k_margin;
    auto next = [&]() { y += k_line_h; };
    auto skip = [&]() { y += k_line_h * 0.6f; };

    const glm::vec4 white  = {1.0f, 1.0f, 1.0f, 1.0f};
    const glm::vec4 yellow = {1.0f, 1.0f, 0.4f, 1.0f};
    const glm::vec4 cyan   = {0.4f, 1.0f, 1.0f, 1.0f};
    const glm::vec4 green  = {0.5f, 1.0f, 0.5f, 1.0f};
    const glm::vec4 red    = {1.0f, 0.4f, 0.4f, 1.0f};
    const glm::vec4 grey   = {0.7f, 0.7f, 0.7f, 1.0f};
    const glm::vec4 orange = {1.0f, 0.7f, 0.2f, 1.0f};

    // ── Atmosphere ────────────────────────────────────────────────────────────
    emit({x, y}, "-- Atmosphere --", yellow); next();
    skip();
    {
        std::ostringstream ss;
        ss << "Zone ID: " << s.zone_id
           << "  (" << s.room_cell_count << " cells)";
        emit({x, y}, ss.str(), grey);
    }
    next();
    {
        std::ostringstream ss;
        ss << "Rooms: " << s.total_rooms
           << "  Adj: " << s.room_adj_count;
        emit({x, y}, ss.str(), grey);
    }
    next();
    skip();

    // Status flags row
    {
        std::string flags;
        uint8_t st = s.atmos_status;
        bool ok  = (st == ATMOS_OK);
        if (ok) flags = "OK";
        if (st & ATMOS_LOW_O2)    flags += (flags.empty() ? "" : " ") + std::string("LOW-O2");
        if (st & ATMOS_LOW_PRESS) flags += (flags.empty() ? "" : " ") + std::string("LOW-P");
        if (st & ATMOS_HIGH_CO2)  flags += (flags.empty() ? "" : " ") + std::string("CO2!");
        if (st & ATMOS_TOXIC)     flags += (flags.empty() ? "" : " ") + std::string("TOXIC");
        if (st & ATMOS_HIGH_TEMP) flags += (flags.empty() ? "" : " ") + std::string("HOT");
        if (st & ATMOS_FIRE)      flags += (flags.empty() ? "" : " ") + std::string("FIRE!");
        if (st & ATMOS_DECOMP)    flags += (flags.empty() ? "" : " ") + std::string("DECOMP");
        glm::vec4 sc = ok ? green
                     : (st & (ATMOS_FIRE | ATMOS_DECOMP)) ? red : orange;
        emit({x, y}, "Status: " + flags, sc);
    }
    next();
    if (s.pressure_loss_rate > 0.1f) {
        std::ostringstream ss;
        ss << "Leak: -" << fmt_f(s.pressure_loss_rate, 1) << " kPa/s";
        emit({x, y}, ss.str(), red); next();
    } else { next(); }
    skip();

    const GasMixture& g = s.gas_mix;
    auto gas_line = [&](const char* label, float kpa) {
        std::ostringstream ss;
        ss << label << ": " << fmt_f(kpa, 1) << " kPa";
        glm::vec4 col = (kpa > 0.01f) ? cyan : grey;
        emit({x, y}, ss.str(), col);
        next();
    };
    gas_line("O2     ", g.o2);
    gas_line("N2     ", g.n2);
    gas_line("CO2    ", g.co2);
    gas_line("Plasma ", g.plasma);
    gas_line("N2O    ", g.n2o);
    gas_line("BZ     ", g.bz);
    gas_line("Tritium", g.tritium);
    skip();
    {
        float total = g.total_pressure();
        glm::vec4 p_col = (total < 20.f)  ? red
                        : (total < 80.f)  ? yellow
                        : (total < 150.f) ? green
                        :                   white;
        emit({x, y}, "Total:   " + fmt_f(total, 1) + " kPa", p_col);
    }
    next();
    emit({x, y}, "Temp:    " + fmt_f(g.temperature, 1) + " K", white); next();
    skip();

    // ── Enclosure ─────────────────────────────────────────────────────────────
    emit({x, y}, "-- Enclosure --", yellow); next();
    skip();
    {
        glm::vec4 enc_col = s.enclosed ? green : red;
        emit({x, y}, s.enclosed ? "Enclosed: YES" : "Enclosed: NO", enc_col);
    }
    next();
}
