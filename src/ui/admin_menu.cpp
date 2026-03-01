// Admin menu — F1 overlay with labelled toggle buttons for all debug features.
#include "ui/admin_menu.h"
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Colour palette
// ─────────────────────────────────────────────────────────────────────────────
static constexpr glm::vec4 COL_BG         = {0.07f, 0.08f, 0.12f, 0.92f};
static constexpr glm::vec4 COL_HEADER_BG  = {0.10f, 0.12f, 0.20f, 0.97f};
static constexpr glm::vec4 COL_TITLE      = {0.80f, 0.88f, 1.00f, 1.00f};
static constexpr glm::vec4 COL_BTN_OFF    = {0.14f, 0.16f, 0.22f, 0.95f};
static constexpr glm::vec4 COL_BTN_ON     = {0.15f, 0.40f, 0.20f, 0.95f};
static constexpr glm::vec4 COL_BTN_HOV    = {0.20f, 0.23f, 0.32f, 0.95f};
static constexpr glm::vec4 COL_TXT        = {0.88f, 0.90f, 0.95f, 1.00f};
static constexpr glm::vec4 COL_TAG_OFF    = {0.50f, 0.20f, 0.20f, 1.00f};
static constexpr glm::vec4 COL_TAG_ON     = {0.20f, 0.70f, 0.30f, 1.00f};
static constexpr glm::vec4 COL_BORDER     = {0.25f, 0.28f, 0.38f, 0.90f};
static constexpr glm::vec4 COL_SECT_BG    = {0.10f, 0.13f, 0.20f, 0.70f};
static constexpr glm::vec4 COL_SECT_TXT   = {0.55f, 0.65f, 0.85f, 1.00f};
// Action button colours
static constexpr glm::vec4 COL_ACT_NORM   = {0.10f, 0.22f, 0.38f, 0.95f};
static constexpr glm::vec4 COL_ACT_HOV    = {0.15f, 0.32f, 0.55f, 0.95f};
static constexpr glm::vec4 COL_ACT_TAG    = {0.20f, 0.55f, 0.90f, 1.00f};

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
    bool hovered = cursor.x >= pos.x && cursor.x <= pos.x + w &&
                   cursor.y >= pos.y && cursor.y <= pos.y + h;
    bool clicked = hovered && lmb_just_pressed;

    glm::vec4 bg;
    if      (active)  bg = COL_BTN_ON;
    else if (hovered) bg = COL_BTN_HOV;
    else              bg = COL_BTN_OFF;

    m_ui.rect(pos, {w, h}, bg, 5.f);

    const float BT = 1.f;
    m_ui.rect(pos,                     {w, BT}, COL_BORDER);
    m_ui.rect(pos,                     {BT, h}, COL_BORDER);
    m_ui.rect({pos.x, pos.y + h - BT}, {w, BT}, COL_BORDER);
    m_ui.rect({pos.x + w - BT, pos.y}, {BT, h}, COL_BORDER);

    m_ui.text({pos.x + 10.f, pos.y + h * 0.5f - 7.f}, label, COL_TXT, 14.f);

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
// Action button — fired event, no ON/OFF state, blue scheme.
bool AdminMenu::draw_action_button(glm::vec2 pos, float w, float h,
                                   const char* label,
                                   glm::vec2 cursor, bool lmb_just_pressed)
{
    bool hovered = cursor.x >= pos.x && cursor.x <= pos.x + w &&
                   cursor.y >= pos.y && cursor.y <= pos.y + h;
    bool clicked = hovered && lmb_just_pressed;

    glm::vec4 bg = hovered ? COL_ACT_HOV : COL_ACT_NORM;
    m_ui.rect(pos, {w, h}, bg, 5.f);

    const float BT = 1.f;
    m_ui.rect(pos,                     {w, BT}, COL_BORDER);
    m_ui.rect(pos,                     {BT, h}, COL_BORDER);
    m_ui.rect({pos.x, pos.y + h - BT}, {w, BT}, COL_BORDER);
    m_ui.rect({pos.x + w - BT, pos.y}, {BT, h}, COL_BORDER);

    m_ui.text({pos.x + 10.f, pos.y + h * 0.5f - 7.f}, label, COL_TXT, 14.f);

    // "RUN" tag
    float tag_w = 30.f;
    float tag_x = pos.x + w - tag_w - 8.f;
    float tag_y = pos.y + (h - 18.f) * 0.5f;
    m_ui.rect({tag_x, tag_y}, {tag_w, 18.f}, COL_ACT_TAG, 3.f);
    m_ui.text({tag_x + 4.f, tag_y + 2.f}, "RUN", {1.f, 1.f, 1.f, 1.f}, 12.f);

    return clicked;
}

// ─────────────────────────────────────────────────────────────────────────────
// A slim labelled separator rendered inside a column.
void AdminMenu::draw_section_label(glm::vec2 pos, float w, const char* text)
{
    m_ui.rect(pos, {w, SECT_H}, COL_SECT_BG, 3.f);
    m_ui.text({pos.x + 7.f, pos.y + 2.f}, text, COL_SECT_TXT, 12.f);
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

    // ── Panel geometry ──────────────────────────────────────────────────────
    constexpr float MARGIN_X = 12.f;
    constexpr float MARGIN_Y = 12.f;

    // Column x-offsets relative to panel origin
    constexpr float COL_A_X = PAD;
    constexpr float COL_B_X = PAD + COL_W + COL_GAP;

    // Content heights for each column (to size the panel)
    // Left col:  section hdr + 10 toggle btns
    constexpr float LEFT_H  = SECT_H + SECT_GAP
                              + 10 * BTN_H + 9 * BTN_GAP;
    // Right col: section hdr + 5 toggles + gap + section hdr + 5 actions
    constexpr float RIGHT_H = SECT_H + SECT_GAP
                              + 5 * BTN_H + 4 * BTN_GAP
                              + SECT_GAP
                              + SECT_H + SECT_GAP
                              + 5 * BTN_H + 4 * BTN_GAP;

    constexpr float CONTENT_H = (LEFT_H > RIGHT_H) ? LEFT_H : RIGHT_H;
    float panel_h = HEADER_H + PAD + CONTENT_H + PAD;

    glm::vec2 panel_pos = {MARGIN_X, MARGIN_Y};

    // ── Panel backdrop ──────────────────────────────────────────────────────
    m_ui.rect(panel_pos, {PANEL_W, panel_h}, COL_BG, 7.f);

    // ── Header bar ───────────────────────────────────────────────────────────
    m_ui.rect(panel_pos, {PANEL_W, HEADER_H}, COL_HEADER_BG, 7.f);
    m_ui.text({panel_pos.x + 12.f, panel_pos.y + 12.f},
              "ADMIN / DEV MENU  [F1 or ESC to close]",
              COL_TITLE, 13.f);

    // ── Thin vertical divider between columns ────────────────────────────────
    float div_x = panel_pos.x + PAD + COL_W + COL_GAP * 0.5f - 0.5f;
    m_ui.rect({div_x, panel_pos.y + HEADER_H + PAD},
              {1.f, CONTENT_H},
              {0.30f, 0.34f, 0.48f, 0.60f});

    const float content_top = panel_pos.y + HEADER_H + PAD;

    // ══════════════════════════════════════════════════════════════════════════
    // LEFT COLUMN — Visual / Debug toggles
    // ══════════════════════════════════════════════════════════════════════════
    float ay = content_top;
    float ax = panel_pos.x + COL_A_X;

    draw_section_label({ax, ay}, COL_W, "  VISUAL / DEBUG");
    ay += SECT_H + SECT_GAP;

    struct TogDef { const char* label; bool s; bool* r; };
    TogDef left_btns[] = {
        { "Noclip",            state.noclip,              &result.toggle_noclip              },
        { "God Mode",          state.godmode,             &result.toggle_godmode             },
        { "Build Mode",        state.build_mode,          &result.toggle_build_mode          },
        { "Gas Overlay",       state.gas_overlay,         &result.toggle_gas_overlay         },
        { "Debug Overlay",     state.debug_overlay,       &result.toggle_debug_overlay       },
        { "Player Stats",      state.player_stats,        &result.toggle_player_stats        },
        { "Verbose Logging",   state.verbose_log,         &result.toggle_verbose_log         },
        { "Fullbright",        state.fullbright,          &result.toggle_fullbright          },
        { "Ambient Occlusion", state.ambient_occlusion,   &result.toggle_ambient_occlusion   },
        { "Wireframe",         state.wireframe,           &result.toggle_wireframe           },
    };
    for (auto& b : left_btns) {
        if (draw_toggle_button({ax, ay}, COL_W, BTN_H, b.label, b.s, cursor, lmb_just_pressed))
            *b.r = true;
        ay += BTN_H + BTN_GAP;
    }

    // ══════════════════════════════════════════════════════════════════════════
    // RIGHT COLUMN — Simulation toggles + one-shot actions
    // ══════════════════════════════════════════════════════════════════════════
    float by = content_top;
    float bx = panel_pos.x + COL_B_X;

    draw_section_label({bx, by}, COL_W, "  SIMULATION");
    by += SECT_H + SECT_GAP;

    TogDef sim_btns[] = {
        { "Freeze Simulation", state.freeze_sim,     &result.toggle_freeze_sim      },
        { "Slow Motion (0.1x)",state.slow_motion,    &result.toggle_slow_motion     },
        { "Auto-Heal",         state.auto_heal,      &result.toggle_auto_heal       },
        { "Zero-G Override",   state.zerog_override, &result.toggle_zerog_override  },
        { "Infinite Oxygen",   state.infinite_oxy,   &result.toggle_infinite_oxy    },
    };
    for (auto& b : sim_btns) {
        if (draw_toggle_button({bx, by}, COL_W, BTN_H, b.label, b.s, cursor, lmb_just_pressed))
            *b.r = true;
        by += BTN_H + BTN_GAP;
    }

    // Gap + "ACTIONS" section
    by += SECT_GAP;
    draw_section_label({bx, by}, COL_W, "  ONE-SHOT ACTIONS");
    by += SECT_H + SECT_GAP;

    struct ActDef { const char* label; bool* r; };
    ActDef act_btns[] = {
        { "Full Heal",          &result.action_full_heal         },
        { "Kill Player",        &result.action_kill_player       },
        { "Teleport to Origin", &result.action_teleport_origin   },
        { "Rebuild Atmos",      &result.action_force_atmos       },
        { "Spawn Test Items",   &result.action_spawn_items       },
    };
    for (auto& b : act_btns) {
        if (draw_action_button({bx, by}, COL_W, BTN_H, b.label, cursor, lmb_just_pressed))
            *b.r = true;
        by += BTN_H + BTN_GAP;
    }

    (void)static_cast<float>(m_ui.fb_width());
    (void)static_cast<float>(m_ui.fb_height());
    return result;
}
