#include "ui/player_stats_overlay.h"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

// ── helpers ───────────────────────────────────────────────────────────────────
namespace {

std::string fmt_f(float v, int dec = 2)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(dec) << v;
    return ss.str();
}

// Simple ASCII health bar: [████░░░░░░]  (fill_chars / total_chars)
std::string ascii_bar(float value, float max_value, int total = 20)
{
    int filled = static_cast<int>(
        std::round(std::clamp(value / max_value, 0.f, 1.f) * total));
    std::string bar = "[";
    for (int i = 0; i < total; ++i)
        bar += (i < filled) ? '#' : '.';
    bar += "]";
    return bar;
}

} // namespace

// ── PlayerStatsOverlay ────────────────────────────────────────────────────────
PlayerStatsOverlay::PlayerStatsOverlay(UIRenderer& ui)
    : m_ui(ui)
{}

void PlayerStatsOverlay::emit(glm::vec2 pos, const std::string& str, glm::vec4 col)
{
    if (str.empty()) return;
    float text_w = static_cast<float>(str.size()) * k_font_size * 0.55f;
    float pill_w = text_w + k_text_pad_x * 2.f;
    float pill_h = k_font_size + k_text_pad_y * 2.f;
    m_ui.rect(pos, {pill_w, pill_h}, {0.f, 0.f, 0.f, 0.60f}, 2.f);
    m_ui.text(pos + glm::vec2(k_text_pad_x, k_text_pad_y), str, col, k_font_size);
}

float PlayerStatsOverlay::draw_bar(float x, float y,
                                   const std::string& label, float value,
                                   float max_value, glm::vec4 fill_col)
{
    const float bar_h  = k_bar_h;
    const float bar_w  = k_bar_w;
    // Label on left
    float lw = static_cast<float>(label.size()) * k_font_size * 0.55f
               + k_text_pad_x * 2.f;
    float lh = k_font_size + k_text_pad_y * 2.f;
    m_ui.rect({x, y}, {lw, lh}, {0.f, 0.f, 0.f, 0.60f}, 2.f);
    m_ui.text({x + k_text_pad_x, y + k_text_pad_y}, label,
              {0.85f, 0.85f, 0.85f, 1.f}, k_font_size);

    float bx = x + lw + 4.f;
    float by = y + (lh - bar_h) * 0.5f;

    // Track (dark background)
    m_ui.rect({bx, by}, {bar_w, bar_h}, {0.15f, 0.15f, 0.15f, 0.8f}, 1.f);
    // Fill
    float ratio = std::clamp(value / max_value, 0.f, 1.f);
    if (ratio > 0.f)
        m_ui.rect({bx, by}, {bar_w * ratio, bar_h}, fill_col, 1.f);

    // Value text overlay
    std::string val_str = fmt_f(value, 1) + " / " + fmt_f(max_value, 0);
    float val_x = bx + bar_w + 6.f;
    float val_w = static_cast<float>(val_str.size()) * k_font_size * 0.55f
                  + k_text_pad_x * 2.f;
    m_ui.rect({val_x, y}, {val_w, lh}, {0.f, 0.f, 0.f, 0.60f}, 2.f);
    m_ui.text({val_x + k_text_pad_x, y + k_text_pad_y}, val_str,
              {0.9f, 0.9f, 0.9f, 1.f}, k_font_size);

    return y + lh + 2.f;   // next Y
}

void PlayerStatsOverlay::draw(const PlayerStatsOverlayState& s)
{
    const glm::vec4 white   = {1.0f, 1.0f, 1.0f, 1.0f};
    const glm::vec4 yellow  = {1.0f, 1.0f, 0.4f, 1.0f};
    const glm::vec4 cyan    = {0.4f, 1.0f, 1.0f, 1.0f};
    const glm::vec4 green   = {0.5f, 1.0f, 0.5f, 1.0f};
    const glm::vec4 red     = {1.0f, 0.35f, 0.35f, 1.0f};
    const glm::vec4 orange  = {1.0f, 0.65f, 0.2f, 1.0f};
    const glm::vec4 grey    = {0.70f, 0.70f, 0.70f, 1.0f};
    const glm::vec4 purple  = {0.8f, 0.5f, 1.0f, 1.0f};
    const glm::vec4 teal    = {0.3f, 0.9f, 0.8f, 1.0f};

    // ── Left column: identity, health, sensors ────────────────────────────────
    {
        float x = k_margin;
        float y = k_margin;
        auto next = [&]() { y += k_line_h; };
        auto skip = [&]() { y += k_line_h * 0.6f; };

        // Header
        emit({x, y}, "VoxelStation 13  [player F6]", yellow); next();
        skip();

        // Identity
        emit({x, y}, "Species: " + s.species + " / " + s.variant, cyan); next();
        skip();

        // Overall health status
        float effective_hp = std::max(0.f, s.health_max - (s.dmg_brute + s.dmg_burn + s.dmg_tox + s.dmg_oxy));
        {
            glm::vec4 hp_col = s.dead           ? red
                             : (effective_hp < s.health_max * 0.25f) ? orange
                             : (effective_hp < s.health_max * 0.60f) ? yellow
                             :                                          green;
            std::string hp_str = s.dead ? "DEAD" : fmt_f(effective_hp, 1) + " / " + fmt_f(s.health_max, 0);
            emit({x, y}, "Health: " + hp_str + "  " + ascii_bar(effective_hp, s.health_max, 16), hp_col);
        }
        next();
        skip();

        // Individual damage buckets with coloured bars
        emit({x, y}, "-- Damage --", yellow); next(); skip();
        float bar_x = x + 4.f;
        y = draw_bar(bar_x, y, "Brute", s.dmg_brute, s.health_max, {0.95f, 0.35f, 0.35f, 0.9f});
        y = draw_bar(bar_x, y, "Burn ", s.dmg_burn,  s.health_max, {1.0f,  0.65f, 0.1f,  0.9f});
        y = draw_bar(bar_x, y, "Tox  ", s.dmg_tox,   s.health_max, {0.55f, 0.9f,  0.25f, 0.9f});
        y = draw_bar(bar_x, y, "Oxy  ", s.dmg_oxy,   s.health_max, {0.35f, 0.75f, 1.0f,  0.9f});
        skip();

        // Suit sensors
        emit({x, y}, "-- Suit Sensors --", yellow); next(); skip();
        {
            // O2 saturation
            glm::vec4 o2c = (s.oxy_sat < 0.5f) ? red : (s.oxy_sat < 0.8f) ? orange : green;
            emit({x, y}, "O2 sat: " + fmt_f(s.oxy_sat * 100.f, 1) + " %  "
                          + ascii_bar(s.oxy_sat, 1.f, 12), o2c); next();
        }
        {
            glm::vec4 pc = (s.suit_pressure_kpa < 20.f)  ? red
                         : (s.suit_pressure_kpa < 70.f)  ? orange
                         : (s.suit_pressure_kpa > 550.f) ? red
                         : (s.suit_pressure_kpa > 300.f) ? orange
                         : (s.suit_pressure_kpa < 150.f) ? green
                         :                                  white;
            std::string pressure_label = "Pressure: " + fmt_f(s.suit_pressure_kpa, 1) + " kPa";
            if (s.suit_pressure_kpa > 550.f) pressure_label += " [CRUSH]";
            else if (s.suit_pressure_kpa > 300.f) pressure_label += " [HIGH]";
            emit({x, y}, pressure_label, pc); next();
        }
        if (s.tox_level > 0.5f) {
            emit({x, y}, "Toxins: " + fmt_f(s.tox_level, 2) + " kPa", red); next();
        } else {
            emit({x, y}, "Toxins: none", grey); next();
        }
        if (!s.suit_temp_str.empty()) {
            emit({x, y}, "Temp: " + s.suit_temp_str, teal); next();
        }
    }

    // ── Right column: movement / controller ──────────────────────────────────
    {
        const float fb_w = static_cast<float>(m_ui.fb_width());
        const float col_w = 235.f;
        float x = fb_w - col_w - k_margin;
        float y = k_margin;
        auto next = [&]() { y += k_line_h; };
        auto skip = [&]() { y += k_line_h * 0.6f; };

        emit({x, y}, "-- Movement --", yellow); next(); skip();

        {
            std::ostringstream ss;
            ss << "Speed: " << fmt_f(s.move_speed, 1)
               << "  Sprint: x" << fmt_f(s.sprint_mult, 2);
            emit({x, y}, ss.str(), white); next();
        }
        {
            emit({x, y}, "Jump vel: " + fmt_f(s.jump_vel, 1) + " m/s", grey); next();
        }
        {
            std::ostringstream ss;
            ss << "Height: " << fmt_f(s.height, 2)
               << "  Radius: " << fmt_f(s.radius, 2);
            emit({x, y}, ss.str(), grey); next();
        }
        skip();

        // Boolean flags
        auto bool_pill = [&](const char* label, bool val,
                             glm::vec4 on_col, glm::vec4 off_col) {
            std::string str = std::string(label) + (val ? ": ON" : ": OFF");
            emit({x, y}, str, val ? on_col : off_col);
            next();
        };
        bool_pill("Grounded",  s.on_ground,        green,  grey);
        bool_pill("Sprinting", s.sprinting,         yellow, grey);
        bool_pill("Zero-G",    s.zero_g,            orange, grey);
        bool_pill("Noclip",    s.noclip,            red,    grey);
        bool_pill("Jetpack",   s.jetpack_equipped,  teal,   grey);
        bool_pill("GrabWall",  s.grab_wall,         cyan,   grey);
        skip();

        // Active hand item
        emit({x, y}, "-- Hands --", yellow); next(); skip();
        {
            std::string hand_label = "Active: " + s.active_hand_id;
            if (!s.active_hand_name.empty())
                hand_label += "  [" + s.active_hand_name + "]";
            else
                hand_label += "  (empty)";
            emit({x, y}, hand_label, purple); next();
        }
    }
}
