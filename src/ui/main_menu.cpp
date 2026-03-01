#include "ui/main_menu.h"
#include <cmath>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Colour palette
// ─────────────────────────────────────────────────────────────────────────────
static constexpr glm::vec4 COL_BG_TOP    = {0.04f, 0.04f, 0.08f, 1.0f};
static constexpr glm::vec4 COL_BG_BOT    = {0.06f, 0.07f, 0.14f, 1.0f};
static constexpr glm::vec4 COL_PANEL     = {0.08f, 0.09f, 0.16f, 0.92f};
static constexpr glm::vec4 COL_BORDER    = {0.25f, 0.30f, 0.45f, 0.80f};
static constexpr glm::vec4 COL_TITLE     = {0.80f, 0.90f, 1.00f, 1.00f};
static constexpr glm::vec4 COL_SUBTITLE  = {0.55f, 0.65f, 0.80f, 0.85f};
static constexpr glm::vec4 COL_BTN       = {0.13f, 0.16f, 0.28f, 0.95f};
static constexpr glm::vec4 COL_BTN_HOV   = {0.20f, 0.44f, 0.68f, 0.95f};
static constexpr glm::vec4 COL_BTN_CLICK = {0.18f, 0.55f, 0.80f, 1.00f};
static constexpr glm::vec4 COL_BTN_EXIT  = {0.22f, 0.12f, 0.12f, 0.95f};
static constexpr glm::vec4 COL_BTN_EXIT_HOV = {0.55f, 0.16f, 0.16f, 0.95f};
static constexpr glm::vec4 COL_BTN_TXT  = {0.92f, 0.95f, 1.00f, 1.00f};
static constexpr glm::vec4 COL_SEP      = {0.25f, 0.30f, 0.50f, 0.60f};
static constexpr glm::vec4 COL_SCANLINE = {0.00f, 0.00f, 0.00f, 0.06f};

// ─────────────────────────────────────────────────────────────────────────────
MainMenu::MainMenu(UIRenderer& ui) : m_ui(ui) {}

// ─────────────────────────────────────────────────────────────────────────────
// Returns true on click frame.
bool MainMenu::draw_button(glm::vec2 pos, float w, float h,
                           const char* label,
                           glm::vec2 cursor, bool lmb)
{
    bool hovered = cursor.x >= pos.x && cursor.x <= pos.x + w &&
                   cursor.y >= pos.y && cursor.y <= pos.y + h;
    bool clicked = hovered && lmb;

    // Pick colour set based on label prefix (Exit button uses warm tones)
    bool is_exit = (label[0] == 'E');
    glm::vec4 base = is_exit ? COL_BTN_EXIT     : COL_BTN;
    glm::vec4 hov  = is_exit ? COL_BTN_EXIT_HOV : COL_BTN_HOV;
    glm::vec4 bg   = hovered ? (clicked ? COL_BTN_CLICK : hov) : base;

    // Shadow
    m_ui.rect({pos.x + 3.f, pos.y + 3.f}, {w, h},
              {0.f, 0.f, 0.f, 0.45f}, 8.f);

    // Body
    m_ui.rect(pos, {w, h}, bg, 7.f);

    // Top highlight strip
    m_ui.rect(pos, {w, 2.f},
              {1.f, 1.f, 1.f, hovered ? 0.25f : 0.10f}, 0.f);

    // Border
    const float BT = 1.5f;
    glm::vec4 bord = hovered ? glm::vec4{0.4f, 0.65f, 1.0f, 0.7f} : COL_BORDER;
    m_ui.rect(pos,                          {w,  BT},  bord);
    m_ui.rect(pos,                          {BT, h},   bord);
    m_ui.rect({pos.x,       pos.y + h - BT},{w,  BT},  bord);
    m_ui.rect({pos.x + w - BT, pos.y},      {BT, h},   bord);

    // Centred label
    float approx_text_w = static_cast<float>(std::strlen(label)) * 9.5f;
    float tx = pos.x + (w - approx_text_w) * 0.5f;
    float ty = pos.y + (h - 16.f) * 0.5f - 1.f;
    m_ui.text({tx, ty}, label, COL_BTN_TXT, 18.f);

    return clicked;
}

// ─────────────────────────────────────────────────────────────────────────────
MainMenuResult MainMenu::draw(glm::vec2 cursor, bool lmb)
{
    MainMenuResult result;

    const float fw = static_cast<float>(m_ui.fb_width());
    const float fh = static_cast<float>(m_ui.fb_height());

    // ── Full-screen background ────────────────────────────────────────────────
    m_ui.rect({0.f, 0.f}, {fw, fh}, COL_BG_TOP);
    // Subtle gradient: darker strip at top, lighter at bottom
    m_ui.rect({0.f, fh * 0.5f}, {fw, fh * 0.5f}, COL_BG_BOT);

    // Faint horizontal scanlines for atmosphere
    constexpr float SCAN_STEP = 4.f;
    for (float y = 0.f; y < fh; y += SCAN_STEP)
        m_ui.rect({0.f, y}, {fw, 1.f}, COL_SCANLINE);

    // Pulsing accent line near top
    float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(m_time * 1.4));
    glm::vec4 accent_col = {0.25f, 0.55f, 0.90f, 0.30f + 0.20f * pulse};
    m_ui.rect({0.f, fh * 0.18f - 1.f}, {fw, 2.f}, accent_col);

    // ── Central panel ─────────────────────────────────────────────────────────
    constexpr float PANEL_W = 380.f;
    constexpr float PANEL_H = 380.f;
    float px = (fw - PANEL_W) * 0.5f;
    float py = (fh - PANEL_H) * 0.5f;

    // Panel shadow
    m_ui.rect({px + 6.f, py + 6.f}, {PANEL_W, PANEL_H},
              {0.f, 0.f, 0.f, 0.50f}, 12.f);
    // Panel body
    m_ui.rect({px, py}, {PANEL_W, PANEL_H}, COL_PANEL, 10.f);
    // Panel border
    const float PBT = 1.5f;
    m_ui.rect({px, py},                              {PANEL_W, PBT},        COL_BORDER);
    m_ui.rect({px, py},                              {PBT, PANEL_H},        COL_BORDER);
    m_ui.rect({px, py + PANEL_H - PBT},              {PANEL_W, PBT},        COL_BORDER);
    m_ui.rect({px + PANEL_W - PBT, py},              {PBT, PANEL_H},        COL_BORDER);

    // ── Title ─────────────────────────────────────────────────────────────────
    // Main title
    {
        const char* title = "VOXELSTATION 13";
        float title_w = static_cast<float>(std::strlen(title)) * 12.5f;
        float title_x = px + (PANEL_W - title_w) * 0.5f;
        // Glow shadow
        m_ui.text({title_x + 1.f, py + 42.f},
                  title, {0.15f, 0.40f, 0.80f, 0.60f}, 22.f);
        m_ui.text({title_x, py + 41.f}, title, COL_TITLE, 22.f);
    }

    // Separator line under title
    m_ui.rect({px + 20.f, py + 78.f}, {PANEL_W - 40.f, 1.f}, COL_SEP);

    // Subtitle / version
    {
        const char* sub = "Pre-Alpha Build";
        float sub_w = static_cast<float>(std::strlen(sub)) * 7.5f;
        m_ui.text({px + (PANEL_W - sub_w) * 0.5f, py + 86.f}, sub,
                  COL_SUBTITLE, 14.f);
    }

    // ── Buttons ───────────────────────────────────────────────────────────────
    float btn_x = px + (PANEL_W - BTN_W) * 0.5f;
    float btn_y = py + 130.f;

    // Play button
    if (draw_button({btn_x, btn_y}, BTN_W, BTN_H, "Play", cursor, lmb))
        result.play_clicked = true;

    btn_y += BTN_H + BTN_GAP;

    // Setup Character button  (teal-ish — distinct from Play and Exit)
    if (draw_button({btn_x, btn_y}, BTN_W, BTN_H, "Setup Character", cursor, lmb))
        result.char_create_clicked = true;

    btn_y += BTN_H + BTN_GAP;

    // Exit button
    if (draw_button({btn_x, btn_y}, BTN_W, BTN_H, "Exit", cursor, lmb))
        result.exit_clicked = true;

    // ── Bottom credits ────────────────────────────────────────────────────────
    {
        const char* credit = "Setup your character before playing";
        float cw = static_cast<float>(std::strlen(credit)) * 6.5f;
        m_ui.text({px + (PANEL_W - cw) * 0.5f, py + PANEL_H - 32.f},
                  credit,
                  {0.40f, 0.50f, 0.65f, 0.70f}, 13.f);
    }

    return result;
}
