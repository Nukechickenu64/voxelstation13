#include "ui/pause_menu.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Colour palette (consistent with MainMenu aesthetic)
// ─────────────────────────────────────────────────────────────────────────────
static constexpr glm::vec4 COL_OVERLAY     = {0.00f, 0.00f, 0.04f, 0.72f};
static constexpr glm::vec4 COL_PANEL       = {0.08f, 0.09f, 0.16f, 0.96f};
static constexpr glm::vec4 COL_PANEL_HDR   = {0.10f, 0.12f, 0.22f, 1.00f};
static constexpr glm::vec4 COL_BORDER      = {0.25f, 0.30f, 0.48f, 0.80f};
static constexpr glm::vec4 COL_BORDER_HOV  = {0.40f, 0.60f, 1.00f, 0.70f};
static constexpr glm::vec4 COL_TITLE       = {0.80f, 0.90f, 1.00f, 1.00f};
static constexpr glm::vec4 COL_LABEL       = {0.82f, 0.88f, 0.96f, 0.92f};
static constexpr glm::vec4 COL_VALUE       = {0.60f, 0.80f, 1.00f, 1.00f};
static constexpr glm::vec4 COL_BTN         = {0.13f, 0.16f, 0.28f, 0.95f};
static constexpr glm::vec4 COL_BTN_HOV     = {0.20f, 0.44f, 0.68f, 0.95f};
static constexpr glm::vec4 COL_BTN_CLICK   = {0.18f, 0.55f, 0.80f, 1.00f};
static constexpr glm::vec4 COL_BTN_EXIT    = {0.22f, 0.12f, 0.12f, 0.95f};
static constexpr glm::vec4 COL_BTN_EXIT_HOV= {0.55f, 0.16f, 0.16f, 0.95f};
static constexpr glm::vec4 COL_BTN_TXT     = {0.92f, 0.95f, 1.00f, 1.00f};
static constexpr glm::vec4 COL_TAB         = {0.10f, 0.12f, 0.20f, 0.90f};
static constexpr glm::vec4 COL_TAB_ACT     = {0.18f, 0.40f, 0.70f, 0.95f};
static constexpr glm::vec4 COL_TAB_HOV     = {0.15f, 0.25f, 0.45f, 0.95f};
static constexpr glm::vec4 COL_TRACK       = {0.12f, 0.15f, 0.26f, 1.00f};
static constexpr glm::vec4 COL_TRACK_FILL  = {0.20f, 0.50f, 0.85f, 1.00f};
static constexpr glm::vec4 COL_THUMB       = {0.50f, 0.78f, 1.00f, 1.00f};
static constexpr glm::vec4 COL_SEP         = {0.22f, 0.28f, 0.45f, 0.50f};
static constexpr glm::vec4 COL_ON          = {0.10f, 0.62f, 0.28f, 0.95f};
static constexpr glm::vec4 COL_OFF         = {0.26f, 0.13f, 0.12f, 0.95f};
static constexpr glm::vec4 COL_ON_HOV      = {0.15f, 0.78f, 0.38f, 0.95f};
static constexpr glm::vec4 COL_OFF_HOV     = {0.50f, 0.18f, 0.15f, 0.95f};
static constexpr glm::vec4 COL_ROW_HOV     = {1.00f, 1.00f, 1.00f, 0.04f};
static constexpr glm::vec4 COL_SCANLINE    = {0.00f, 0.00f, 0.00f, 0.05f};

// ─────────────────────────────────────────────────────────────────────────────
PauseMenu::PauseMenu(UIRenderer& ui) : m_ui(ui) {}

void PauseMenu::open()
{
    m_open        = true;
    m_in_settings = false;
    m_drag_slider = -1;
}

void PauseMenu::close()
{
    m_open        = false;
    m_in_settings = false;
    m_drag_slider = -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Widget helpers
// ─────────────────────────────────────────────────────────────────────────────

bool PauseMenu::draw_button(glm::vec2 pos, float w, float h,
                             const char* label, glm::vec2 cursor, bool lmb,
                             bool warn_style)
{
    bool hov = cursor.x >= pos.x && cursor.x <= pos.x + w &&
               cursor.y >= pos.y && cursor.y <= pos.y + h;
    bool clicked = hov && lmb;

    glm::vec4 col_base = warn_style ? COL_BTN_EXIT     : COL_BTN;
    glm::vec4 col_hov  = warn_style ? COL_BTN_EXIT_HOV : COL_BTN_HOV;
    glm::vec4 bg       = hov ? (clicked ? COL_BTN_CLICK : col_hov) : col_base;

    // Shadow
    m_ui.rect({pos.x + 3.f, pos.y + 3.f}, {w, h}, {0.f,0.f,0.f,0.40f}, 7.f);
    // Body
    m_ui.rect(pos, {w, h}, bg, 6.f);
    // Top highlight
    m_ui.rect(pos, {w, 2.f}, {1.f,1.f,1.f, hov ? 0.22f : 0.09f});
    // Border
    const float BT = 1.5f;
    glm::vec4 bord = hov ? COL_BORDER_HOV : COL_BORDER;
    m_ui.rect(pos,                          {w,  BT},  bord);
    m_ui.rect(pos,                          {BT, h},   bord);
    m_ui.rect({pos.x,       pos.y + h - BT},{w,  BT},  bord);
    m_ui.rect({pos.x + w - BT, pos.y},      {BT, h},   bord);

    float tw = static_cast<float>(std::strlen(label)) * 9.5f;
    m_ui.text({pos.x + (w - tw) * 0.5f, pos.y + (h - 16.f) * 0.5f - 1.f},
              label, COL_BTN_TXT, 17.f);
    return clicked;
}

// ─────────────────────────────────────────────────────────────────────────────
bool PauseMenu::draw_tab_btn(glm::vec2 pos, float w, float h,
                              const char* label, bool active,
                              glm::vec2 cursor, bool lmb)
{
    bool hov = cursor.x >= pos.x && cursor.x <= pos.x + w &&
               cursor.y >= pos.y && cursor.y <= pos.y + h;
    bool clicked = hov && lmb;

    glm::vec4 bg = active ? COL_TAB_ACT : (hov ? COL_TAB_HOV : COL_TAB);
    m_ui.rect(pos, {w, h}, bg, 4.f);

    // Active underline
    if (active)
        m_ui.rect({pos.x + 4.f, pos.y + h - 3.f}, {w - 8.f, 3.f}, {0.55f,0.85f,1.0f,0.90f});

    // Border on active tab
    if (active) {
        const float BT = 1.f;
        m_ui.rect(pos,                          {w,  BT},  COL_BORDER_HOV);
        m_ui.rect(pos,                          {BT, h},   COL_BORDER);
        m_ui.rect({pos.x + w - BT, pos.y},      {BT, h},   COL_BORDER);
    }

    float tw = static_cast<float>(std::strlen(label)) * 8.f;
    glm::vec4 tc = active ? glm::vec4{1.f,1.f,1.f,1.f} : COL_LABEL;
    m_ui.text({pos.x + (w - tw) * 0.5f, pos.y + (h - 14.f) * 0.5f - 1.f},
              label, tc, 14.f);
    return clicked && !active;
}

// ─────────────────────────────────────────────────────────────────────────────
float PauseMenu::draw_slider(glm::vec2 row_pos, float row_w,
                              const char* label, float value,
                              float vmin, float vmax, const char* fmt,
                              glm::vec2 cursor, bool lmb_pressed, bool lmb_held,
                              int slider_id)
{
    // Layout within row
    const float PAD    = 20.f;
    const float LABEL_W= 185.f;
    const float TRACK_W= 170.f;
    const float TRACK_H= 8.f;
    const float ROW_H_  = ROW_H;
    const float THUMB_R = 7.f;  // thumb half-size

    glm::vec2 lbl_pos  = {row_pos.x + PAD, row_pos.y + (ROW_H_ - 14.f) * 0.5f};
    float track_x      = row_pos.x + LABEL_W;
    float track_y      = row_pos.y + (ROW_H_ - TRACK_H) * 0.5f;
    float val_x        = track_x + TRACK_W + 10.f;

    // Row hover background
    bool row_hov = cursor.x >= row_pos.x && cursor.x <= row_pos.x + row_w &&
                   cursor.y >= row_pos.y && cursor.y <= row_pos.y + ROW_H_;
    if (row_hov)
        m_ui.rect(row_pos, {row_w, ROW_H_}, COL_ROW_HOV, 3.f);

    // Label
    m_ui.text(lbl_pos, label, COL_LABEL, 14.f);

    // If mouse released, stop dragging
    if (!lmb_held && m_drag_slider == slider_id)
        m_drag_slider = -1;

    // Start drag on press inside the track area (or within the thumb reach)
    bool on_track = cursor.x >= track_x - THUMB_R && cursor.x <= track_x + TRACK_W + THUMB_R &&
                    cursor.y >= row_pos.y && cursor.y <= row_pos.y + ROW_H_;
    if (lmb_pressed && on_track)
        m_drag_slider = slider_id;

    // Update value while dragging
    if (m_drag_slider == slider_id && lmb_held) {
        float t = (cursor.x - track_x) / TRACK_W;
        t     = std::clamp(t, 0.f, 1.f);
        value = vmin + t * (vmax - vmin);
    }

    // Draw track background
    m_ui.rect({track_x, track_y}, {TRACK_W, TRACK_H}, COL_TRACK, 4.f);

    // Draw filled portion
    float t_val = (vmax > vmin) ? (value - vmin) / (vmax - vmin) : 0.f;
    t_val = std::clamp(t_val, 0.f, 1.f);
    float fill_w = t_val * TRACK_W;
    if (fill_w > 0.f)
        m_ui.rect({track_x, track_y}, {fill_w, TRACK_H}, COL_TRACK_FILL, 4.f);

    // Draw thumb
    float thumb_x = track_x + fill_w;
    bool  dragging = (m_drag_slider == slider_id);
    glm::vec4 thumb_col = dragging ? glm::vec4{0.80f,0.95f,1.0f,1.f} : COL_THUMB;
    m_ui.rect({thumb_x - THUMB_R, track_y - 3.f},
              {THUMB_R * 2.f, TRACK_H + 6.f}, thumb_col, 3.f);

    // Value label
    char buf[32];
    std::snprintf(buf, sizeof(buf), fmt, value);
    m_ui.text({val_x, lbl_pos.y}, buf, COL_VALUE, 14.f);

    return value;
}

// ─────────────────────────────────────────────────────────────────────────────
bool PauseMenu::draw_toggle(glm::vec2 row_pos, float row_w,
                             const char* label, bool value,
                             glm::vec2 cursor, bool lmb)
{
    const float PAD     = 20.f;
    const float PILL_W  = 68.f;
    const float PILL_H  = 24.f;
    const float ROW_H_  = ROW_H;

    glm::vec2 lbl_pos  = {row_pos.x + PAD, row_pos.y + (ROW_H_ - 14.f) * 0.5f};
    float pill_x       = row_pos.x + row_w - PAD - PILL_W;
    float pill_y       = row_pos.y + (ROW_H_ - PILL_H) * 0.5f;

    // Row hover background
    bool row_hov = cursor.x >= row_pos.x && cursor.x <= row_pos.x + row_w &&
                   cursor.y >= row_pos.y && cursor.y <= row_pos.y + ROW_H_;
    if (row_hov)
        m_ui.rect(row_pos, {row_w, ROW_H_}, COL_ROW_HOV, 3.f);

    bool pill_hov = cursor.x >= pill_x && cursor.x <= pill_x + PILL_W &&
                    cursor.y >= pill_y && cursor.y <= pill_y + PILL_H;
    bool clicked  = pill_hov && lmb;
    bool display  = clicked ? !value : value;  // show pending state on click

    glm::vec4 pill_col = display
        ? (pill_hov ? COL_ON_HOV  : COL_ON)
        : (pill_hov ? COL_OFF_HOV : COL_OFF);

    // Label
    m_ui.text(lbl_pos, label, COL_LABEL, 14.f);

    // Pill
    m_ui.rect({pill_x, pill_y},         {PILL_W, PILL_H}, pill_col, 5.f);
    // Border
    const float BT = 1.f;
    glm::vec4 bord = pill_hov ? COL_BORDER_HOV : COL_BORDER;
    m_ui.rect({pill_x,           pill_y},           {PILL_W, BT},   bord);
    m_ui.rect({pill_x,           pill_y},           {BT,  PILL_H},  bord);
    m_ui.rect({pill_x,           pill_y+PILL_H-BT}, {PILL_W, BT},   bord);
    m_ui.rect({pill_x+PILL_W-BT, pill_y},           {BT,  PILL_H},  bord);

    // Text inside pill
    const char* pill_lbl = display ? "ON" : "OFF";
    float tw = static_cast<float>(std::strlen(pill_lbl)) * 8.5f;
    m_ui.text({pill_x + (PILL_W - tw) * 0.5f, pill_y + (PILL_H - 14.f) * 0.5f - 1.f},
              pill_lbl, COL_BTN_TXT, 14.f);

    return clicked;
}

// ─────────────────────────────────────────────────────────────────────────────
void PauseMenu::draw_separator(glm::vec2 pos, float w)
{
    m_ui.rect(pos, {w, 1.f}, COL_SEP);
}

// ─────────────────────────────────────────────────────────────────────────────
// Main draw
// ─────────────────────────────────────────────────────────────────────────────
PauseMenuResult PauseMenu::draw(glm::vec2 cursor, bool lmb_pressed, bool lmb_held,
                                 bool esc_pressed, GameSettings& s)
{
    PauseMenuResult result;
    if (!m_open) return result;

    const float fw = static_cast<float>(m_ui.fb_width());
    const float fh = static_cast<float>(m_ui.fb_height());

    // ── Full-screen dim overlay ────────────────────────────────────────────────
    m_ui.rect({0.f, 0.f}, {fw, fh}, COL_OVERLAY);
    // Faint scanlines for atmosphere
    for (float y = 0.f; y < fh; y += 4.f)
        m_ui.rect({0.f, y}, {fw, 1.f}, COL_SCANLINE);

    if (!m_in_settings) {
        // ══════════════════════════════════════════════════════════════════════
        // PAUSE PANEL
        // ══════════════════════════════════════════════════════════════════════
        const float ph = PANEL_H_PAUSE;
        const float px = (fw - PANEL_W) * 0.5f;
        const float py = (fh - ph)      * 0.5f;

        // Panel shadow
        m_ui.rect({px + 6.f, py + 6.f}, {PANEL_W, ph}, {0.f,0.f,0.f,0.50f}, 12.f);
        // Panel body
        m_ui.rect({px, py}, {PANEL_W, ph}, COL_PANEL, 10.f);
        // Header bar
        m_ui.rect({px, py}, {PANEL_W, 50.f}, COL_PANEL_HDR, 0.f);
        // Panel border
        const float BT = 1.5f;
        m_ui.rect({px,              py           }, {PANEL_W, BT},   COL_BORDER);
        m_ui.rect({px,              py           }, {BT, ph        }, COL_BORDER);
        m_ui.rect({px,              py + ph - BT }, {PANEL_W, BT   }, COL_BORDER);
        m_ui.rect({px + PANEL_W - BT, py         }, {BT, ph        }, COL_BORDER);

        // Title
        constexpr const char* TITLE = "[ PAUSED ]";
        float tw = static_cast<float>(std::strlen(TITLE)) * 12.5f;
        m_ui.text({px + (PANEL_W - tw) * 0.5f, py + 15.f}, TITLE, COL_TITLE, 22.f);

        // Separator below header
        m_ui.rect({px + 16.f, py + 52.f}, {PANEL_W - 32.f, 1.f}, COL_SEP);

        // Buttons
        const float bx = px + (PANEL_W - BTN_W) * 0.5f;
        float       by = py + 70.f;

        if (draw_button({bx, by}, BTN_W, BTN_H, "RESUME", cursor, lmb_pressed)) {
            result.resume_clicked = true;
            close();
        }
        by += BTN_H + BTN_GAP;

        if (draw_button({bx, by}, BTN_W, BTN_H, "SETTINGS", cursor, lmb_pressed)) {
            m_in_settings = true;
            m_tab = SettingsTab::Graphics;
        }
        by += BTN_H + BTN_GAP;

        // Separator before destructive actions
        draw_separator({bx, by + 4.f}, BTN_W);
        by += 16.f;

        if (draw_button({bx, by}, BTN_W, BTN_H, "EXIT TO MAIN MENU", cursor, lmb_pressed, true)) {
            result.exit_to_main_clicked = true;
            close();
        }
        by += BTN_H + BTN_GAP;

        if (draw_button({bx, by}, BTN_W, BTN_H, "EXIT GAME", cursor, lmb_pressed, true)) {
            result.exit_game_clicked = true;
            close();
        }

        // Escape = resume
        if (esc_pressed) {
            result.resume_clicked = true;
            close();
        }

    } else {
        // ══════════════════════════════════════════════════════════════════════
        // SETTINGS PANEL
        // ══════════════════════════════════════════════════════════════════════
        const float ph = PANEL_H_SETTINGS;
        const float px = (fw - PANEL_W) * 0.5f;
        const float py = (fh - ph)      * 0.5f;

        // Panel shadow + body
        m_ui.rect({px + 6.f, py + 6.f}, {PANEL_W, ph}, {0.f,0.f,0.f,0.50f}, 12.f);
        m_ui.rect({px, py}, {PANEL_W, ph}, COL_PANEL, 10.f);
        // Header
        m_ui.rect({px, py}, {PANEL_W, 50.f}, COL_PANEL_HDR, 0.f);
        // Panel border
        const float BT = 1.5f;
        m_ui.rect({px,              py           }, {PANEL_W, BT},   COL_BORDER);
        m_ui.rect({px,              py           }, {BT, ph        }, COL_BORDER);
        m_ui.rect({px,              py + ph - BT }, {PANEL_W, BT   }, COL_BORDER);
        m_ui.rect({px + PANEL_W - BT, py         }, {BT, ph        }, COL_BORDER);

        // Title
        constexpr const char* TITLE = "SETTINGS";
        float tw = static_cast<float>(std::strlen(TITLE)) * 12.5f;
        m_ui.text({px + (PANEL_W - tw) * 0.5f, py + 15.f}, TITLE, COL_TITLE, 22.f);

        // ── Tab bar ────────────────────────────────────────────────────────────
        const float TAB_Y  = py + 52.f;
        const float TAB_H  = 34.f;
        const float TAB_W  = PANEL_W / 4.f;   // 4 tabs
        static const char* s_tab_labels[] = { "GRAPHICS", "AUDIO", "CONTROLS", "GAMEPLAY" };

        for (int i = 0; i < 4; ++i) {
            bool act = (static_cast<int>(m_tab) == i);
            if (draw_tab_btn({px + TAB_W * i, TAB_Y}, TAB_W, TAB_H,
                             s_tab_labels[i], act, cursor, lmb_pressed))
                m_tab = static_cast<SettingsTab>(i);
        }

        // Separator below tabs
        m_ui.rect({px, TAB_Y + TAB_H}, {PANEL_W, 1.f}, COL_BORDER);

        // ── Content area ───────────────────────────────────────────────────────
        const float CONT_X = px + 10.f;
        const float CONT_W = PANEL_W - 20.f;
        float       row_y  = TAB_Y + TAB_H + 8.f;

        // scroll clip area: ph - (title+tabs+back_button+padding)
        // Draw settings rows according to current tab
        int slider_base = static_cast<int>(m_tab) * 10; // offset IDs per tab

        switch (m_tab) {
        // ── GRAPHICS ──────────────────────────────────────────────────────────
        case SettingsTab::Graphics: {
            s.fov = draw_slider({CONT_X, row_y}, CONT_W,
                                "Field of View", s.fov,
                                60.f, 120.f, "%.0f\xc2\xb0",
                                cursor, lmb_pressed, lmb_held, slider_base + 0);
            row_y += ROW_H;

            draw_separator({CONT_X + 8.f, row_y}, CONT_W - 16.f);
            row_y += 10.f;

            if (draw_toggle({CONT_X, row_y}, CONT_W,
                            "Ambient Occlusion", s.ambient_occlusion, cursor, lmb_pressed))
                s.ambient_occlusion = !s.ambient_occlusion;
            row_y += ROW_H;

            if (draw_toggle({CONT_X, row_y}, CONT_W,
                            "Fullbright Mode", s.fullbright, cursor, lmb_pressed))
                s.fullbright = !s.fullbright;
            row_y += ROW_H;

            if (draw_toggle({CONT_X, row_y}, CONT_W,
                            "Wireframe Mode", s.wireframe, cursor, lmb_pressed))
                s.wireframe = !s.wireframe;
            row_y += ROW_H;

            draw_separator({CONT_X + 8.f, row_y}, CONT_W - 16.f);
            row_y += 10.f;

            if (draw_toggle({CONT_X, row_y}, CONT_W,
                            "PSX Vertex Wobble", s.psx_wobble, cursor, lmb_pressed))
                s.psx_wobble = !s.psx_wobble;
            row_y += ROW_H;

            s.psx_snap_res = draw_slider({CONT_X, row_y}, CONT_W,
                                         "PSX Precision", s.psx_snap_res,
                                         20.f, 320.f, "%.0f",
                                         cursor, lmb_pressed, lmb_held, slider_base + 1);
            row_y += ROW_H;

            if (draw_toggle({CONT_X, row_y}, CONT_W,
                            "PSX Y-Shear", s.psx_yshear, cursor, lmb_pressed))
                s.psx_yshear = !s.psx_yshear;
            row_y += ROW_H;

            draw_separator({CONT_X + 8.f, row_y}, CONT_W - 16.f);
            row_y += 10.f;

            if (draw_toggle({CONT_X, row_y}, CONT_W,
                            "Affine Texture Mapping", s.affine_enabled, cursor, lmb_pressed))
                s.affine_enabled = !s.affine_enabled;
            row_y += ROW_H;

            {
                float v = s.affine_mix * 100.f;
                v = draw_slider({CONT_X, row_y}, CONT_W,
                                "Affine Strength", v, 0.f, 100.f, "%.0f%%",
                                cursor, lmb_pressed, lmb_held, slider_base + 2);
                s.affine_mix = v / 100.f;
            }
            row_y += ROW_H;
            break;

        }

        // ── AUDIO ─────────────────────────────────────────────────────────────
        // Audio sliders store 0-1 internally; display as 0-100%.
        case SettingsTab::Audio: {
            {
                float v = s.master_volume * 100.f;
                v = draw_slider({CONT_X, row_y}, CONT_W,
                                "Master Volume", v, 0.f, 100.f, "%.0f%%",
                                cursor, lmb_pressed, lmb_held, slider_base + 0);
                s.master_volume = v / 100.f;
            }
            row_y += ROW_H;
            {
                float v = s.sfx_volume * 100.f;
                v = draw_slider({CONT_X, row_y}, CONT_W,
                                "SFX Volume", v, 0.f, 100.f, "%.0f%%",
                                cursor, lmb_pressed, lmb_held, slider_base + 1);
                s.sfx_volume = v / 100.f;
            }
            row_y += ROW_H;
            {
                float v = s.music_volume * 100.f;
                v = draw_slider({CONT_X, row_y}, CONT_W,
                                "Music Volume", v, 0.f, 100.f, "%.0f%%",
                                cursor, lmb_pressed, lmb_held, slider_base + 2);
                s.music_volume = v / 100.f;
            }
            row_y += ROW_H;
            break;
        }

        // ── CONTROLS ──────────────────────────────────────────────────────────
        case SettingsTab::Controls: {
            s.mouse_sensitivity = draw_slider({CONT_X, row_y}, CONT_W,
                                              "Mouse Sensitivity", s.mouse_sensitivity,
                                              0.02f, 0.50f, "%.2f",
                                              cursor, lmb_pressed, lmb_held, slider_base + 0);
            row_y += ROW_H;

            draw_separator({CONT_X + 8.f, row_y}, CONT_W - 16.f);
            row_y += 10.f;

            if (draw_toggle({CONT_X, row_y}, CONT_W,
                            "Invert Mouse Y", s.invert_mouse_y, cursor, lmb_pressed))
                s.invert_mouse_y = !s.invert_mouse_y;
            row_y += ROW_H;
            break;
        }

        // ── GAMEPLAY ──────────────────────────────────────────────────────────
        case SettingsTab::Gameplay: {
            if (draw_toggle({CONT_X, row_y}, CONT_W,
                            "Show Debug Overlay", s.show_debug_overlay, cursor, lmb_pressed))
                s.show_debug_overlay = !s.show_debug_overlay;
            row_y += ROW_H;

            if (draw_toggle({CONT_X, row_y}, CONT_W,
                            "Show Gas Overlay", s.show_gas_overlay, cursor, lmb_pressed))
                s.show_gas_overlay = !s.show_gas_overlay;
            row_y += ROW_H;

            if (draw_toggle({CONT_X, row_y}, CONT_W,
                            "Show Player Stats", s.show_player_stats, cursor, lmb_pressed))
                s.show_player_stats = !s.show_player_stats;
            row_y += ROW_H;

            draw_separator({CONT_X + 8.f, row_y}, CONT_W - 16.f);
            row_y += 10.f;

            if (draw_toggle({CONT_X, row_y}, CONT_W,
                            "Show Crosshair", s.show_crosshair, cursor, lmb_pressed))
                s.show_crosshair = !s.show_crosshair;
            row_y += ROW_H;

            if (draw_toggle({CONT_X, row_y}, CONT_W,
                            "Show Radio Log", s.show_radio_log, cursor, lmb_pressed))
                s.show_radio_log = !s.show_radio_log;
            row_y += ROW_H;

            draw_separator({CONT_X + 8.f, row_y}, CONT_W - 16.f);
            row_y += 10.f;

            if (draw_toggle({CONT_X, row_y}, CONT_W,
                            "Third-Person Camera", s.third_person, cursor, lmb_pressed))
                s.third_person = !s.third_person;
            row_y += ROW_H;

            if (draw_toggle({CONT_X, row_y}, CONT_W,
                            "Freeze Simulation", s.freeze_sim, cursor, lmb_pressed))
                s.freeze_sim = !s.freeze_sim;
            row_y += ROW_H;
            break;
        }
        }

        // ── Back button ────────────────────────────────────────────────────────
        const float back_w = 160.f;
        const float back_h = 38.f;
        const float back_x = px + (PANEL_W - back_w) * 0.5f;
        const float back_y = py + ph - back_h - 14.f;

        // Subtle separator above back button
        m_ui.rect({px + 16.f, back_y - 10.f}, {PANEL_W - 32.f, 1.f}, COL_SEP);

        if (draw_button({back_x, back_y}, back_w, back_h, "BACK", cursor, lmb_pressed)) {
            m_in_settings = false;
            m_drag_slider = -1;
        }

        // Escape = back to pause panel
        if (esc_pressed) {
            m_in_settings = false;
            m_drag_slider = -1;
        }
    }

    return result;
}
