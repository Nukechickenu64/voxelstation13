#include "ui/hud.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <glm/glm.hpp>

HUD::HUD(UIRenderer& ui)
    : m_ui(ui)
{}

void HUD::draw(const HUDState& state, const Inventory& inv, int hotbar_slot)
{
    draw_health_bar(state);
    draw_suit_sensors(state);
    draw_hotbar(inv, hotbar_slot);
    draw_hands(inv, state.active_hand_is_left);
    draw_clock(state.clock_str);
    if (!state.examine_label.empty())
        draw_examine_label(state.examine_label);
    if (!state.radio_log.empty())
        draw_radio_log(state.radio_log);
}

void HUD::draw_health_bar(const HUDState& s)
{
    const float BAR_W  = 200.f, BAR_H = 16.f;
    const float PAD    = 12.f;
    const float fb_h   = static_cast<float>(m_ui.fb_height());
    glm::vec2 pos      = {PAD, fb_h - PAD - BAR_H};

    float ratio = (s.health_max > 0) ? (s.health / s.health_max) : 0.f;
    ratio = std::clamp(ratio, 0.f, 1.f);

    // Background
    m_ui.rect(pos, {BAR_W, BAR_H}, {0.1f, 0.1f, 0.1f, 0.7f}, 4.f);
    // Fill — green → red
    glm::vec4 fill = {1.f - ratio, ratio * 0.8f, 0.1f, 0.9f};
    m_ui.rect(pos, {BAR_W * ratio, BAR_H}, fill, 4.f);
    // Label
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(0) << s.health << " / " << s.health_max;
    m_ui.text(pos + glm::vec2(4.f, 1.f), ss.str(), {1,1,1,1}, 12.f);
}

void HUD::draw_suit_sensors(const HUDState& s)
{
    const float PAD = 12.f;
    float fb_h = static_cast<float>(m_ui.fb_height());
    glm::vec2 pos = {PAD, fb_h - 100.f};
    m_ui.rect(pos, {180.f, 70.f}, {0.05f, 0.05f, 0.05f, 0.6f}, 4.f);

    auto fmt_f = [](float v, int dec) {
        std::ostringstream ss; ss << std::fixed << std::setprecision(dec) << v; return ss.str();
    };
    m_ui.text(pos + glm::vec2(6,6),  "O2: "  + fmt_f(s.oxy_sat * 100.f, 0) + "%", {0.5f,0.9f,1,1}, 11.f);
    m_ui.text(pos + glm::vec2(6,20), "TOX: " + fmt_f(s.tox_level, 2),              {1,0.6f,0.2f,1}, 11.f);
    m_ui.text(pos + glm::vec2(6,34), "P: "   + fmt_f(s.suit_pressure_kpa, 1)+" kPa",{0.8f,1,0.8f,1},11.f);
    if (!s.suit_temp_str.empty())
        m_ui.text(pos + glm::vec2(6,48), "T: " + s.suit_temp_str,                  {0.9f,0.9f,0.9f,1},11.f);
}

void HUD::draw_hotbar(const Inventory& inv, int active_slot)
{
    const int  SLOTS     = 9;
    const float SZ       = 48.f;
    const float PAD      = 4.f;
    float total_w  = SLOTS * (SZ + PAD) - PAD;
    float fb_w     = static_cast<float>(m_ui.fb_width());
    float fb_h     = static_cast<float>(m_ui.fb_height());
    float start_x  = (fb_w - total_w) * 0.5f;
    float y        = fb_h - SZ - 8.f;

    // Hotbar is backed by the first 9 hotbar slots; we approximate using l/r hands here
    for (int i = 0; i < SLOTS; ++i) {
        glm::vec2 pos = {start_x + i * (SZ + PAD), y};
        bool is_active = (i == active_slot);
        glm::vec4 bg   = is_active ? glm::vec4{0.3f,0.3f,0.5f,0.85f}
                                   : glm::vec4{0.1f,0.1f,0.1f,0.6f};
        m_ui.rect(pos, {SZ, SZ}, bg, 4.f);
        if (is_active)
            m_ui.rect(pos, {SZ, SZ}, {0.6f,0.7f,1,0.5f}, 4.f); // highlight border
        // TODO: draw item icon if slot has item
        (void)inv;
        // Slot number label
        m_ui.text(pos + glm::vec2(2,2), std::to_string(i + 1), {0.7f,0.7f,0.7f,0.8f}, 10.f);
    }
}

void HUD::draw_hands(const Inventory& inv, bool left_active)
{
    const float SZ   = 56.f, PAD = 8.f;
    float fb_w = static_cast<float>(m_ui.fb_width());
    float fb_h = static_cast<float>(m_ui.fb_height());

    glm::vec2 lpos = {fb_w * 0.5f - SZ - PAD, fb_h - SZ - 64.f};
    glm::vec2 rpos = {fb_w * 0.5f + PAD,       fb_h - SZ - 64.f};

    auto draw_hand = [&](glm::vec2 pos, const std::string& slot_id, bool active) {
        glm::vec4 bg = active ? glm::vec4{0.25f,0.35f,0.5f,0.85f}
                              : glm::vec4{0.1f,0.1f,0.1f,0.65f};
        m_ui.rect(pos, {SZ, SZ}, bg, 4.f);
        const auto* slot = inv.find_slot(slot_id);
        if (slot && slot->item) {
            // TODO: draw item icon
            m_ui.text(pos + glm::vec2(4, SZ - 16), slot->item->def->name,
                      {1,1,1,0.9f}, 10.f);
        }
    };

    draw_hand(lpos, "l_hand", left_active);
    draw_hand(rpos, "r_hand", !left_active);
}

void HUD::draw_examine_label(const std::string& label)
{
    float fb_w = static_cast<float>(m_ui.fb_width());
    float fb_h = static_cast<float>(m_ui.fb_height());
    glm::vec2 pos = {fb_w * 0.5f + 20.f, fb_h * 0.5f - 10.f};
    m_ui.rect(pos - glm::vec2(4,2), {static_cast<float>(label.size() * 7 + 8), 18.f},
              {0,0,0,0.6f}, 2.f);
    m_ui.text(pos, label, {1,1,0.8f,1}, 13.f);
}

void HUD::draw_radio_log(const std::deque<std::string>& log)
{
    const float LINE_H = 14.f, PAD = 8.f;
    const int   MAX_LINES = 8;
    float fb_h = static_cast<float>(m_ui.fb_height());
    float y    = fb_h * 0.6f;
    int shown  = 0;
    for (auto it = log.rbegin(); it != log.rend() && shown < MAX_LINES; ++it, ++shown) {
        m_ui.text({PAD, y - shown * LINE_H}, *it, {0.85f,0.9f,1,0.85f}, 11.f);
    }
}

void HUD::draw_clock(const std::string& time_str)
{
    if (time_str.empty()) return;
    float fb_w = static_cast<float>(m_ui.fb_width());
    m_ui.text({fb_w - 80.f, 10.f}, time_str, {0.8f,0.8f,0.8f,0.8f}, 13.f);
}
