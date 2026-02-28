// Admin menu — F1 overlay with labelled toggle buttons for all debug features.
#include "ui/admin_menu.h"

// ─────────────────────────────────────────────────────────────────────────────
// Colour palette
// ─────────────────────────────────────────────────────────────────────────────
static constexpr glm::vec4 COL_BG        = {0.07f, 0.08f, 0.12f, 0.92f};
static constexpr glm::vec4 COL_HEADER_BG = {0.12f, 0.15f, 0.22f, 0.95f};
static constexpr glm::vec4 COL_TITLE     = {0.80f, 0.88f, 1.00f, 1.00f};
static constexpr glm::vec4 COL_BTN_OFF   = {0.14f, 0.16f, 0.22f, 0.95f};
static constexpr glm::vec4 COL_BTN_ON    = {0.15f, 0.40f, 0.20f, 0.95f};
static constexpr glm::vec4 COL_BTN_HOV   = {0.20f, 0.23f, 0.32f, 0.95f};
static constexpr glm::vec4 COL_TXT       = {0.88f, 0.90f, 0.95f, 1.00f};
static constexpr glm::vec4 COL_TAG_OFF   = {0.50f, 0.20f, 0.20f, 1.00f};
static constexpr glm::vec4 COL_TAG_ON    = {0.20f, 0.70f, 0.30f, 1.00f};
static constexpr glm::vec4 COL_BORDER    = {0.25f, 0.28f, 0.38f, 0.90f};

// ─────────────────────────────────────────────────────────────────────────────
AdminMenu::AdminMenu(UIRenderer& ui) : m_ui(ui) {}

void AdminMenu::open()  { m_open = true;  }
void AdminMenu::close() { m_open = false; }

// ─────────────────────────────────────────────────────────────────────────────
// Returns true on the frame the button is clicked.
bool AdminMenu::draw_toggle_button(glm::vec2 pos, float w, float h,
                                   const char* label, bool active,
                                   glm::vec2 cursor, bool lmb_just_pressed)
{
    // Hit-test
    bool hovered = cursor.x >= pos.x && cursor.x <= pos.x + w &&
                   cursor.y >= pos.y && cursor.y <= pos.y + h;
    bool clicked = hovered && lmb_just_pressed;

    // Background
    glm::vec4 bg;
    if      (active)  bg = COL_BTN_ON;
    else if (hovered) bg = COL_BTN_HOV;
    else              bg = COL_BTN_OFF;

    m_ui.rect(pos, {w, h}, bg, 5.f);

    // Thin border
    const float BT = 1.f;
    m_ui.rect(pos,                  {w, BT},  COL_BORDER);
    m_ui.rect(pos,                  {BT, h},  COL_BORDER);
    m_ui.rect({pos.x, pos.y + h - BT}, {w, BT},  COL_BORDER);
    m_ui.rect({pos.x + w - BT, pos.y}, {BT, h},  COL_BORDER);

    // Label text
    m_ui.text({pos.x + 10.f, pos.y + h * 0.5f - 7.f}, label, COL_TXT, 14.f);

    // ON / OFF tag on the right
    const char* tag_str   = active ? "ON" : "OFF";
    glm::vec4   tag_color = active ? COL_TAG_ON : COL_TAG_OFF;

    float tag_w = active ? 28.f : 34.f;
    float tag_x = pos.x + w - tag_w - 8.f;
    float tag_y = pos.y + (h - 18.f) * 0.5f;
    m_ui.rect({tag_x, tag_y}, {tag_w, 18.f}, tag_color, 3.f);
    m_ui.text({tag_x + (active ? 6.f : 5.f), tag_y + 2.f}, tag_str,
              {1.f, 1.f, 1.f, 1.f}, 12.f);

    return clicked;
}

// ─────────────────────────────────────────────────────────────────────────────
AdminMenuResult AdminMenu::draw(glm::vec2 cursor,
                                bool lmb_just_pressed,
                                bool escape_pressed,
                                const AdminMenuState& state)
{
    AdminMenuResult result;
    if (!m_open) return result;

    if (escape_pressed) {
        result.close_requested = true;
        close();
        return result;
    }

    const float fb_w = static_cast<float>(m_ui.fb_width());
    const float fb_h = static_cast<float>(m_ui.fb_height());

    // Panel anchored top-left with a small margin
    constexpr float MARGIN_X = 12.f;
    constexpr float MARGIN_Y = 12.f;

    // Six buttons
    constexpr int NUM_BTNS = 6;
    float panel_h = HEADER_H + PAD
                    + NUM_BTNS * BTN_H + (NUM_BTNS - 1) * BTN_GAP
                    + PAD;

    glm::vec2 panel_pos = {MARGIN_X, MARGIN_Y};

    // ── Semi-transparent backdrop ──────────────────────────────────────────
    m_ui.rect(panel_pos, {PANEL_W, panel_h}, COL_BG, 7.f);

    // ── Header bar ─────────────────────────────────────────────────────────
    m_ui.rect(panel_pos, {PANEL_W, HEADER_H}, COL_HEADER_BG, 7.f);
    m_ui.text({panel_pos.x + 12.f, panel_pos.y + 10.f},
              "ADMIN MENU  [F1 / ESC to close]",
              COL_TITLE, 13.f);

    // ── Buttons ─────────────────────────────────────────────────────────────
    const float btn_x = panel_pos.x + PAD;
    const float btn_w = PANEL_W - PAD * 2.f;
    float btn_y = panel_pos.y + HEADER_H + PAD;

    struct BtnDef { const char* label; bool state; bool* result_flag; };
    BtnDef btns[NUM_BTNS] = {
        { "Noclip",          state.noclip,        &result.toggle_noclip        },
        { "Build Mode",      state.build_mode,    &result.toggle_build_mode    },
        { "Gas Overlay",     state.gas_overlay,   &result.toggle_gas_overlay   },
        { "Debug Overlay",   state.debug_overlay, &result.toggle_debug_overlay },
        { "Player Stats",    state.player_stats,  &result.toggle_player_stats  },
        { "Verbose Logging", state.verbose_log,   &result.toggle_verbose_log   },
    };

    for (int i = 0; i < NUM_BTNS; ++i) {
        if (draw_toggle_button({btn_x, btn_y}, btn_w, BTN_H,
                               btns[i].label, btns[i].state,
                               cursor, lmb_just_pressed))
            *btns[i].result_flag = true;
        btn_y += BTN_H + BTN_GAP;
    }

    (void)fb_w; (void)fb_h;
    return result;
}
