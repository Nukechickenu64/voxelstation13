#include "ui/hud.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <glm/glm.hpp>

HUD::HUD(UIRenderer& ui)
    : m_ui(ui)
{
}

void HUD::draw(const HUDState& state, const Inventory& inv)
{
    draw_health_bar(state);
    draw_suit_sensors(state);
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

    // Background — pulse dark red when below 25%
    bool critical = (ratio < 0.25f);
    float pulse = critical
        ? (0.5f + 0.5f * std::sin(static_cast<float>(SDL_GetTicks()) * 0.008f))
        : 0.f;
    glm::vec4 bg_col = critical
        ? glm::vec4{0.25f + pulse * 0.1f, 0.02f, 0.02f, 0.85f}
        : glm::vec4{0.1f, 0.1f, 0.1f, 0.7f};
    m_ui.rect(pos, {BAR_W, BAR_H}, bg_col, 4.f);
    // Fill — green → red
    glm::vec4 fill = {1.f - ratio, ratio * 0.8f, 0.1f, 0.9f};
    m_ui.rect(pos, {BAR_W * ratio, BAR_H}, fill, 4.f);
    // Label
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(0) << s.health << " / " << s.health_max;
    m_ui.text(pos + glm::vec2(4.f, 1.f), ss.str(), {1,1,1,1}, 12.f);

    // Critical warning text (flashing)
    if (critical && pulse > 0.4f) {
        m_ui.text(pos + glm::vec2(BAR_W + 6.f, 1.f), "CRITICAL",
                  {1.f, 0.15f, 0.1f, 0.9f + pulse * 0.1f}, 12.f);
    }
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

void HUD::draw_hands(const Inventory& inv, bool left_active)
{
    // --- Always-on hand slots (small, above hotbar) ---
    const float SZ   = 56.f, PAD = 8.f;
    float fb_w = static_cast<float>(m_ui.fb_width());
    float fb_h = static_cast<float>(m_ui.fb_height());

    glm::vec2 lpos = {fb_w * 0.5f - SZ - PAD, fb_h - SZ - 20.f};
    glm::vec2 rpos = {fb_w * 0.5f + PAD,       fb_h - SZ - 20.f};

    auto draw_slot = [&](glm::vec2 pos, const std::string& slot_id, bool active,
                         const char* hand_label) {
        glm::vec4 bg = active ? glm::vec4{0.25f,0.35f,0.5f,0.85f}
                              : glm::vec4{0.1f,0.1f,0.1f,0.65f};
        m_ui.rect(pos, {SZ, SZ}, bg, 4.f);
        // Active hand ring
        if (active)
            m_ui.rect(pos - glm::vec2(2.f), {SZ + 4.f, SZ + 4.f},
                      {0.4f, 0.6f, 1.f, 0.6f}, 5.f);

        const auto* slot = inv.find_slot(slot_id);
        if (slot && slot->item && slot->item->def) {
            const auto& def = *slot->item->def;
            SDL_GPUTexture* icon = m_ui.item_icon(def.id);
            if (icon) {
                m_ui.image(pos + glm::vec2(4.f, 4.f), {SZ - 8.f, SZ - 8.f}, icon, 1.f);
            } else {
                // Fallback: tinted rectangle + truncated name
                m_ui.rect(pos + glm::vec2(4.f, 4.f), {SZ - 8.f, SZ - 8.f},
                          {0.25f, 0.38f, 0.58f, 0.7f}, 2.f);
                std::string n = def.name;
                if (n.size() > 7) n = n.substr(0, 6) + ".";
                m_ui.text(pos + glm::vec2(4.f, SZ - 16.f), n, {1,1,1,0.9f}, 9.f);
            }
            // Stack count badge in the top-left corner
            if (slot->item->count > 1) {
                std::string cnt = "x" + std::to_string(slot->item->count);
                m_ui.text(pos + glm::vec2(2.f, 2.f), cnt, {1.f, 1.f, 0.4f, 1.f}, 9.f);
            }
            // Integrity bar at bottom edge
            if (slot->item->integrity < 1.f) {
                float fil = std::max(0.f, slot->item->integrity) * (SZ - 4.f);
                float bary = pos.y + SZ - 3.f;
                m_ui.rect({pos.x + 2.f, bary}, {SZ - 4.f, 2.f},
                          {0.25f, 0.25f, 0.25f, 0.7f}, 0.f);
                glm::vec4 ic = (slot->item->integrity > 0.5f)
                                ? glm::vec4{0.2f, 0.9f, 0.3f, 0.9f}
                                : glm::vec4{0.9f, 0.35f, 0.1f, 0.9f};
                m_ui.rect({pos.x + 2.f, bary}, {fil, 2.f}, ic, 0.f);
            }
        }

        // Hand label (L / R) below the slot box
        glm::vec4 lbl_col = active
            ? glm::vec4{0.55f, 0.8f, 1.f, 0.95f}
            : glm::vec4{0.45f, 0.5f, 0.6f, 0.7f};
        float lbl_x = pos.x + SZ * 0.5f - 5.f;
        m_ui.text({lbl_x, pos.y + SZ + 2.f}, hand_label, lbl_col, 10.f);
    };

    draw_slot(lpos, "l_hand", left_active,  "L");
    draw_slot(rpos, "r_hand", !left_active, "R");
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
