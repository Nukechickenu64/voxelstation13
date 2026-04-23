#include "ui/hud.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/glm.hpp>

// ─────────────────────────────────────────────────────────────────────────────
//  Layout constants  (all in logical/CSS pixels)
// ─────────────────────────────────────────────────────────────────────────────
static constexpr float BAR_H     = 64.f;   // single-row slot area
static constexpr float BAR_PAD   =  7.f;   // top/bottom inner padding
static constexpr float SWAP_H    =  0.f;   // no swap strip
static constexpr float HAND_SZ   = 56.f;   // hand slot square
static constexpr float HAND_GAP  =  4.f;   // gap between left and right hand slots
static constexpr float EQUIP_SZ  = HAND_SZ; // all slots same size as hands
static constexpr float EQUIP_GAP =  4.f;
static constexpr float SEC_GAP   = 10.f;   // gap between sections

// Right-side panel margin from screen edge
static constexpr float PANEL_X_MARGIN = 8.f;

// ─────────────────────────────────────────────────────────────────────────────
//  Colour constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr glm::vec4 k_bar_bg    = {0.04f, 0.05f, 0.07f, 0.00f};
static constexpr glm::vec4 k_bar_top   = {0.22f, 0.45f, 0.88f, 0.00f};
static constexpr glm::vec4 k_slot_bg   = {0.11f, 0.11f, 0.14f, 0.00f};
static constexpr glm::vec4 k_slot_act  = {0.18f, 0.28f, 0.52f, 0.00f};
static constexpr glm::vec4 k_slot_ring = {0.32f, 0.58f, 1.00f, 0.72f};
static constexpr glm::vec4 k_sec_div   = {0.20f, 0.26f, 0.40f, 0.00f};
static constexpr glm::vec4 k_intent_col[4] = {
    {0.08f, 0.55f, 0.08f, 1.f},
    {0.16f, 0.36f, 0.80f, 1.f},
    {0.82f, 0.50f, 0.00f, 1.f},
    {0.75f, 0.08f, 0.08f, 1.f},
};
static constexpr const char* k_intent_label[4] = { "HELP", "DSRM", "GRAB", "HARM" };

// ─────────────────────────────────────────────────────────────────────────────
HUD::HUD(UIRenderer& ui) : m_ui(ui)
{
    static const char* BASE = "legacysets/extracted/hud/screen_gen/";
    static const char* MID  = "legacysets/extracted/hud/screen_midnight/";
    auto L = [&](const std::string& name) -> SDL_GPUTexture* {
        return ui.load_texture((BASE + name).c_str());
    };
    auto LM = [&](const std::string& name) -> SDL_GPUTexture* {
        return ui.load_texture((std::string(MID) + name).c_str());
    };

    // Intent icons (indexed by Intent enum)
    m_intent_tex[0] = L("help.png");
    m_intent_tex[1] = L("disarm.png");
    m_intent_tex[2] = L("grab.png");
    m_intent_tex[3] = L("harm.png");

    // Zone sprites — order matches BodyZone enum: Chest=0 … Groin=6
    static const char* k_zone_name[7] = {
        "chest", "head", "l_arm", "r_arm", "l_leg", "r_leg", "groin"
    };
    for (int z = 0; z < 7; ++z) {
        m_zone_sel_tex[z] = L(std::string(k_zone_name[z]) + ".png");
        for (int d = 0; d < 5; ++d)
            m_zone_dmg_tex[z][d] = L(std::string(k_zone_name[z]) + std::to_string(d) + ".png");
    }

    // Overall health-state background sprites
    for (int i = 0; i < 5; ++i)
        m_living_tex[i] = L("living" + std::to_string(i) + ".png");

    // Health-state indicator icons (ui_health: health0=full … health7=dead)
    for (int i = 0; i < 8; ++i)
        m_health_tex[i] = L("health" + std::to_string(i) + ".gif");

    // Mood sprites (ui_mood: mood1-9.png)
    for (int i = 0; i < 9; ++i)
        m_mood_tex[i] = L("mood" + std::to_string(i + 1) + ".png");

    // Internal O2 indicator (ui_internal)
    m_internal_tex[0] = L("internal0.png");
    m_internal_tex[1] = L("internal1.png");
    m_internal_tex[2] = L("internal2.gif");

    // Doll outline shared by both the damage doll and target doll
    m_doll_base_tex = LM("zone_sel.png");

    // Suit pressure sprites
    m_suit_tex[0] = L("spacesuit_empty.png");
    m_suit_tex[1] = L("spacesuit_low.png");
    m_suit_tex[2] = L("spacesuit_mid.png");
    m_suit_tex[3] = L("spacesuit_high.png");

    // Generic slot background frame — use midnight's richer template
    m_template_tex        = LM("template.png");
    m_template_active_tex = LM("template_active.png");
    // Pull indicator — midnight version has "PULL" text badge
    m_pull_tex = LM("pull.png");

    // Hand slot sprites (screen_midnight)
    m_hand_l_tex        = LM("hand_l.png");
    m_hand_r_tex        = LM("hand_r.png");
    m_lhand_active_tex  = LM("lhandactive.png");
    m_rhand_active_tex  = LM("rhandactive.png");

    // Movement intent sprites (screen_midnight)
    m_walking_tex = LM("walking.png");
    m_running_tex = LM("running.png");

    // INV toggle sprites (screen_midnight)
    m_toggle_tex        = LM("toggle.png");
    m_toggle_active_tex = LM("toggle_active.png");
    // Per-slot empty icons (screen_midnight) — drawn when slot is empty
    m_slot_icon_tex["head"]         = LM("head.png");
    m_slot_icon_tex["eyes"]         = LM("glasses.png");
    m_slot_icon_tex["ears"]         = LM("ears.png");
    m_slot_icon_tex["mask"]         = LM("mask.png");
    m_slot_icon_tex["suit"]         = LM("suit.png");
    m_slot_icon_tex["uniform"]      = LM("uniform.png");
    m_slot_icon_tex["back"]         = LM("back.png");
    m_slot_icon_tex["gloves"]       = LM("gloves.png");
    m_slot_icon_tex["belt"]         = LM("belt.png");
    m_slot_icon_tex["boots"]        = LM("shoes.png");
    m_slot_icon_tex["neck"]         = LM("neck.png");
    m_slot_icon_tex["id_card"]      = LM("id.png");
    m_slot_icon_tex["suit_storage"] = LM("suit_storage.png");
    m_slot_icon_tex["l_pocket"]     = LM("pocket.png");
    m_slot_icon_tex["r_pocket"]     = LM("pocket.png");
}

// ─────────────────────────────────────────────────────────────────────────────
//  draw() — TG-faithful layout  (hud.dm screen_loc reference)
//
//  Left of hands:   suit_storage | id | belt | back    (C-5 C-4 C-3 C-2)
//  Hands (center):  [l_hand]  [r_hand]
//  Right of hands:  pda | storage1                     (C+1 C+2)
//  EAST edge bar:   [intent] [zone]
//  WEST edge bar:   [INV toggle]
//  Right float:     health panel (center height)
// ─────────────────────────────────────────────────────────────────────────────
std::string HUD::draw(HUDState& state, const Inventory& inv,
                      glm::vec2 mouse_pos, bool lmb_clicked)
{
    const float fb_w   = static_cast<float>(m_ui.fb_width());
    const float fb_h   = static_cast<float>(m_ui.fb_height());
    const float bar_y  = fb_h - BAR_H;
    // Slots sit in the lower 68px of the bar (top 12px = swap strip area)
    const float slot_y = bar_y + SWAP_H + (BAR_H - SWAP_H - EQUIP_SZ) * 0.5f;
    const float hand_y = slot_y;   // hands align with equipment slots
    const float cx     = fb_w * 0.5f;

    // Hand slot edges — symmetric around cx, directly adjacent (HAND_GAP = SEP)
    const float lhand_x  = cx - HAND_SZ - HAND_GAP * 0.5f;
    const float rhand_end = lhand_x + HAND_SZ + HAND_GAP + HAND_SZ;  // right edge of r_hand
    constexpr float SEP = 4.f;   // gap between adjacent slots

    // ── Right-side health panel (TG: EAST-1:28 column, CENTER-4 to CENTER-1) ─
    draw_health_panel(state);

    // ── Full-width bottom bar background ──────────────────────────────────────
    m_ui.rect({0.f, bar_y}, {fb_w, BAR_H}, k_bar_bg,  0.f);
    m_ui.rect({0.f, bar_y}, {fb_w, 2.f},   k_bar_top, 0.f);

    const bool in_bar = (mouse_pos.y >= bar_y - 2.f);
    const bool click  = in_bar && lmb_clicked;
    std::string clicked;

    // ── WEST edge: inventory toggle ─────────────────────────────────────────────
    {
        const glm::vec2 bp = {6.f, slot_y};
        bool hov = (mouse_pos.x >= bp.x && mouse_pos.x < bp.x + EQUIP_SZ &&
                    mouse_pos.y >= bp.y && mouse_pos.y < bp.y + EQUIP_SZ);
        if (click && hov) state.inv_open = !state.inv_open;
        glm::vec4 col = state.inv_open
            ? glm::vec4{0.22f, 0.38f, 0.72f, 0.00f}
            : (hov ? glm::vec4{0.22f, 0.32f, 0.52f, 0.00f}
                   : glm::vec4{0.11f, 0.13f, 0.20f, 0.00f});
        m_ui.rect(bp, {EQUIP_SZ, EQUIP_SZ}, col, 4.f);
        SDL_GPUTexture* tog = state.inv_open ? m_toggle_active_tex : m_toggle_tex;
        if (tog)
            m_ui.image(bp, {EQUIP_SZ, EQUIP_SZ}, tog, hov ? 1.0f : 0.85f);
        else if (m_template_tex)
            m_ui.image(bp, {EQUIP_SZ, EQUIP_SZ}, m_template_tex, 0.65f);
    }

    // ── Left cluster: back | belt | id | suit_storage (packed right→left from left hand)
    // TG order is C-2:back  C-3:belt  C-4:id  C-5:sstore1
    {
        struct S { const char* id; const char* lbl; };
        static const S kLeft[] = {
            {"back",         "BACK"},
            {"belt",         "BELT"},
            {"id_card",      "ID"  },
            {"suit_storage", "SSTR"},
        };
        float rx = lhand_x - SEP;   // right edge of next slot
        for (const auto& s : kLeft) {
            float sx = rx - EQUIP_SZ;
            if (draw_slot(inv, s.id, {sx, slot_y}, EQUIP_SZ, s.lbl, false, mouse_pos, click))
                clicked = s.id;
            rx = sx - SEP;
        }
    }

    // ── Hands (center) ────────────────────────────────────────────────────────
    draw_hand_slots(inv, state.active_hand_is_left,
                    {lhand_x, hand_y}, mouse_pos, click, clicked);

    // ── Right cluster: l_pocket | r_pocket (packed left→right from right hand)
    // TG: C+1:storage1 (l_pocket)  C+2:storage2 (r_pocket)
    {
        struct S { const char* id; const char* lbl; };
        static const S kRight[] = {
            {"l_pocket", "PKT"},
            {"r_pocket", "PKT"},
        };
        float sx = rhand_end + SEP;
        for (const auto& s : kRight) {
            if (draw_slot(inv, s.id, {sx, slot_y}, EQUIP_SZ, s.lbl, false, mouse_pos, click))
                clicked = s.id;
            sx += EQUIP_SZ + SEP;
        }
    }

    // ── EAST edge: intent + zone selector ────────────────────────────────
    draw_zone_intent(state, mouse_pos, click);

    // ── Body-slot panel (shown above bar when inv_open) ───────────────────────
    if (state.inv_open) {
        constexpr float PAD = 6.f;
        constexpr float PROWS = 4.f;
        const float inner_h = PROWS * EQUIP_SZ + (PROWS - 1) * SEP;
        const float panel_h = inner_h + 2.f * PAD;
        const float panel_y = bar_y - panel_h - 4.f;
        draw_body_equip(inv, {6.f + PAD, panel_y + PAD},
                        mouse_pos, lmb_clicked, clicked);
    }
    draw_clock(state.clock_str);
    if (!state.examine_label.empty())
        draw_examine_label(state.examine_label);
    if (!state.radio_log.empty())
        draw_radio_log(state.radio_log);
    return clicked;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Generic slot — returns true when LMB clicked inside.
// ─────────────────────────────────────────────────────────────────────────────
bool HUD::draw_slot(const Inventory& inv, const char* slot_id,
                    glm::vec2 pos, float sz,
                    const char* fallback_label,
                    bool highlight_active,
                    glm::vec2 mouse, bool click)
{
    bool hov = (mouse.x >= pos.x && mouse.x < pos.x + sz &&
                mouse.y >= pos.y && mouse.y < pos.y + sz);

    if (highlight_active)
        m_ui.rect(pos - glm::vec2(2.f), {sz + 4.f, sz + 4.f}, k_slot_ring, 6.f);

    // Background rect then template overlay
    glm::vec4 bg = highlight_active ? k_slot_act : k_slot_bg;
    if (hov) bg = {bg.r + 0.06f, bg.g + 0.07f, bg.b + 0.10f, bg.a};
    m_ui.rect(pos, {sz, sz}, bg, 4.f);

    // Use template_active for active/hover, normal template otherwise
    SDL_GPUTexture* tmpl = (highlight_active && m_template_active_tex)
        ? m_template_active_tex : m_template_tex;
    if (tmpl)
        m_ui.image(pos, {sz, sz}, tmpl, hov ? 0.90f : 0.75f);

    const auto* slot = inv.find_slot(slot_id);
    if (slot && slot->item && slot->item->def) {
        const auto& def = *slot->item->def;
        const float pad = sz * 0.08f;
        SDL_GPUTexture* icon = m_ui.item_icon(def.id);
        if (icon) {
            m_ui.image(pos + glm::vec2(pad), {sz - pad*2, sz - pad*2}, icon);
        } else {
            m_ui.rect(pos + glm::vec2(pad), {sz - pad*2, sz - pad*2},
                      {0.20f, 0.32f, 0.52f, 0.65f}, 2.f);
            std::string n = def.name;
            size_t mx = static_cast<size_t>(sz / 6.5f);
            if (n.size() > mx) n = n.substr(0, mx - 1) + ".";
            m_ui.text(pos + glm::vec2(pad, sz - 12.f), n, {1,1,1,0.85f}, 8.f);
        }
        if (slot->item->count > 1)
            m_ui.text(pos + glm::vec2(2.f, 2.f),
                      "x" + std::to_string(slot->item->count), {1,1,0.4f,1}, 8.f);
        if (slot->item->integrity < 1.f) {
            float fil = std::max(0.f, slot->item->integrity) * (sz - 4.f);
            glm::vec4 ic = (slot->item->integrity > 0.5f)
                ? glm::vec4{0.15f,0.85f,0.25f,0.9f}
                : glm::vec4{0.90f,0.28f,0.08f,0.9f};
            m_ui.rect({pos.x+2.f, pos.y+sz-3.f}, {sz-4.f, 2.f}, {0.12f,0.12f,0.12f,0.7f}, 0.f);
            m_ui.rect({pos.x+2.f, pos.y+sz-3.f}, {fil, 2.f}, ic, 0.f);
        }
    } else {
        // Show per-slot icon if available, otherwise fall back to dim text
        auto it = m_slot_icon_tex.find(slot_id);
        if (it != m_slot_icon_tex.end() && it->second) {
            m_ui.image(pos, {sz, sz}, it->second, hov ? 0.85f : 0.60f);
        } else {
            float tw = static_cast<float>(strlen(fallback_label)) * 5.8f;
            m_ui.text(pos + glm::vec2((sz - tw) * 0.5f, sz * 0.5f - 5.f),
                      fallback_label, {0.26f,0.30f,0.40f,0.68f}, 8.f);
        }
    }
    return hov && click;
}

// ─────────────────────────────────────────────────────────────────────────────
//  draw_health_panel — right-side column, TG screen_loc positions scaled
//  from 32px tiles to EQUIP_SZ (56px).
//
//  TG Y formula (top-down):  top_y = fb_h/2 + N*32 - O - 32  for CENTER-N:O
//  Scaled (×SZ/32):          top_y = fb_h/2 + N*SZ - O*(SZ/32) - SZ
//
//  Elements rendered (top→bottom):
//   ui_mood       CENTER:21    → mood sprite (mood1-9)
//   ui_health     CENTER-1:19  → health state sprite (health0-7)
//   ui_healthdoll CENTER-2:17  → body doll (living + zone damage overlays)
//   ui_internal   below doll   → O2 tank indicator (internal0-2)
//   ui_spacesuit  CENTER-4:14  → suit pressure sprite
// ─────────────────────────────────────────────────────────────────────────────
void HUD::draw_health_panel(const HUDState& s)
{
    const float fb_w  = static_cast<float>(m_ui.fb_width());
    const float fb_h  = static_cast<float>(m_ui.fb_height());

    constexpr float SZ    = EQUIP_SZ;           // 56px
    const float     sc    = SZ / 32.f;          // scale factor 1.75
    const float     col_x = fb_w - SZ - PANEL_X_MARGIN;

    // Y positions (top of sprite, scaled from TG 32px tiles)
    // CENTER+N:O → top_y = fb_h/2 - N*SZ - O*sc - SZ
    // CENTER-N:O → top_y = fb_h/2 + N*SZ - O*sc - SZ
    const float y_mood     = fb_h * 0.5f + 0.f*SZ - 21.f*sc - SZ; // CENTER:21
    const float y_health   = fb_h * 0.5f + 1.f*SZ - 19.f*sc - SZ; // CENTER-1:19
    const float y_doll     = fb_h * 0.5f + 2.f*SZ - 17.f*sc - SZ; // CENTER-2:17
    const float y_internal = y_doll + SZ + EQUIP_GAP;               // below doll
    const float y_suit     = fb_h * 0.5f + 4.f*SZ - 14.f*sc - SZ; // CENTER-4:14

    float ratio = (s.health_max > 0.f)
        ? std::clamp(s.health / s.health_max, 0.f, 1.f) : 0.f;


    // ── ui_mood: mood sprite ─────────────────────────────────────────────────
    {
        int idx = std::clamp(s.mood_level, 0, 8);
        if (m_mood_tex[idx])
            m_ui.image({col_x, y_mood}, {SZ, SZ}, m_mood_tex[idx]);
    }

    // ── ui_health: health state sprite ──────────────────────────────────────
    {
        int lvl = s.dead ? 7 : std::min(6, static_cast<int>((1.f - ratio) * 7.f));
        if (m_health_tex[lvl])
            m_ui.image({col_x, y_health}, {SZ, SZ}, m_health_tex[lvl]);
        else {
            glm::vec4 hcol = ratio > 0.7f ? glm::vec4{0.10f, 0.80f, 0.10f, 0.9f}
                           : ratio > 0.3f ? glm::vec4{0.90f, 0.70f, 0.10f, 0.9f}
                                          : glm::vec4{0.90f, 0.10f, 0.10f, 0.9f};
            m_ui.rect({col_x, y_health}, {SZ, SZ}, {0.1f, 0.1f, 0.1f, 0.7f}, 3.f);
            std::string hs = std::to_string(static_cast<int>(std::max(0.f, s.health)));
            m_ui.text({col_x + SZ * 0.5f - static_cast<float>(hs.size()) * 3.5f,
                        y_health + SZ * 0.5f - 6.f}, hs, hcol, 10.f);
        }
    }

    // ── ui_healthdoll: body doll with zone damage overlays ───────────────────
    {
        int lvl = s.dead ? 4 : std::min(4, static_cast<int>((1.f - ratio) * 5.f));
        if (lvl > 0 && m_living_tex[lvl])
            m_ui.image({col_x, y_doll}, {SZ, SZ}, m_living_tex[lvl]);

        auto dmg_lvl = [&](float dmg) -> int {
            float pct = (s.health_max > 0.f) ? dmg / s.health_max : 0.f;
            if (pct < 0.05f) return 0;
            if (pct < 0.25f) return 1;
            if (pct < 0.50f) return 2;
            if (pct < 0.75f) return 3;
            return 4;
        };
        int bl = dmg_lvl(s.dmg_brute);
        int fl = dmg_lvl(s.dmg_burn);
        const int zone_lvl[7] = { std::max(bl,fl), std::max(bl,fl), bl, bl, bl, bl, bl };
        for (int z = 0; z < 7; ++z)
            if (m_zone_dmg_tex[z][zone_lvl[z]])
                m_ui.image({col_x, y_doll}, {SZ, SZ}, m_zone_dmg_tex[z][zone_lvl[z]]);

        if (s.is_pulling && m_pull_tex) {
            constexpr float PULL_SZ = 14.f;
            m_ui.image({col_x + SZ - PULL_SZ - 2.f, y_doll - PULL_SZ - 2.f},
                       {PULL_SZ, PULL_SZ}, m_pull_tex);
        }
    }

    // ── ui_internal: O2 tank indicator ──────────────────────────────────────
    {
        int lvl = s.oxy_sat > 0.90f ? 0
                : s.oxy_sat > 0.50f ? 1 : 2;
        if (m_internal_tex[lvl])
            m_ui.image({col_x, y_internal}, {SZ, SZ}, m_internal_tex[lvl]);
    }

    // ── ui_spacesuit: suit pressure sprite ──────────────────────────────────
    {
        int suit_lvl = 3; // ok (hidden)
        if      (s.suit_pressure_kpa < 20.f)  suit_lvl = 0;
        else if (s.suit_pressure_kpa < 70.f)  suit_lvl = 1;
        else if (s.suit_pressure_kpa < 120.f) suit_lvl = 2;
        if (suit_lvl < 3 && m_suit_tex[suit_lvl])
            m_ui.image({col_x, y_suit}, {SZ, SZ}, m_suit_tex[suit_lvl]);
    }

    // ── Dead / Critical overlay ──────────────────────────────────────────────
    float pulse = (s.in_crit || s.dead)
        ? (0.5f + 0.5f * std::sin(static_cast<float>(SDL_GetTicks()) * 0.008f)) : 0.f;
    if (s.dead) {
        m_ui.text({col_x + 4.f, y_doll + SZ * 0.5f - 5.f},
                  "DEAD", {0.55f, 0.55f, 0.60f, 0.90f}, 9.f);
    } else if (s.in_crit && pulse > 0.42f) {
        m_ui.text({col_x + 4.f, y_doll + SZ * 0.5f - 5.f},
                  "CRIT", {1.f, 0.06f, 0.06f, 0.88f + pulse * 0.12f}, 9.f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Hand slots — large, center-anchored
// ─────────────────────────────────────────────────────────────────────────────
void HUD::draw_hand_slots(const Inventory& inv, bool left_active,
                           glm::vec2 origin,
                           glm::vec2 mouse, bool click, std::string& out_click)
{
    const float lx = origin.x;
    const float rx = origin.x + HAND_SZ + HAND_GAP;
    const float hy = origin.y;

    const std::string grip_hand = inv.gripped_hand_id();   // "" if no two-hander

    auto draw_one = [&](float px, const char* sid, bool active)
    {
        bool is_grip = (!grip_hand.empty() && std::string(sid) == grip_hand);

        // Amber ring for the gripped (support) hand
        if (is_grip)
            m_ui.rect({px-3.f,hy-3.f}, {HAND_SZ+6.f,HAND_SZ+6.f},
                      {0.80f,0.52f,0.05f,0.85f}, 7.f);

        bool hov = (mouse.x >= px && mouse.x < px+HAND_SZ &&
                    mouse.y >= hy && mouse.y < hy+HAND_SZ);
        glm::vec4 bg = is_grip ? glm::vec4{0.35f,0.22f,0.03f,0.00f} : k_slot_bg;
        m_ui.rect({px,hy}, {HAND_SZ,HAND_SZ}, bg, 5.f);
        SDL_GPUTexture* hand_bg = (std::string(sid) == "l_hand") ? m_hand_l_tex : m_hand_r_tex;
        if (hand_bg) m_ui.image({px,hy}, {HAND_SZ,HAND_SZ}, hand_bg, hov ? 1.0f : 0.85f);
        if (active && !is_grip) {
            SDL_GPUTexture* act_tex = (std::string(sid) == "l_hand") ? m_lhand_active_tex : m_rhand_active_tex;
            if (act_tex) m_ui.image({px,hy}, {HAND_SZ,HAND_SZ}, act_tex);
        }

        if (is_grip) {
            // The item icon is already shown in the holding hand — just label this one.
            float tw = 4.f * 6.f; // "GRIP" is 4 chars
            m_ui.text({px + (HAND_SZ - tw) * 0.5f, hy + HAND_SZ * 0.5f - 5.f},
                      "GRIP", {1.f, 0.78f, 0.12f, 0.95f}, 9.f);
            if (hov && click) out_click = sid;
            return;
        }

        const auto* slot = inv.find_slot(sid);
        if (slot && slot->item && slot->item->def) {
            const auto& def = *slot->item->def;
            const float pad = 4.f;
            SDL_GPUTexture* icon = m_ui.item_icon(def.id);
            if (icon) {
                m_ui.image({px+pad,hy+pad}, {HAND_SZ-pad*2,HAND_SZ-pad*2}, icon);
            } else {
                m_ui.rect({px+pad,hy+pad}, {HAND_SZ-pad*2,HAND_SZ-pad*2},
                          {0.20f,0.32f,0.52f,0.68f}, 3.f);
                std::string n = def.name.size() > 7 ? def.name.substr(0,6)+"." : def.name;
                m_ui.text({px+pad, hy+HAND_SZ-14.f}, n, {1,1,1,0.9f}, 9.f);
            }
            if (slot->item->count > 1)
                m_ui.text({px+2.f,hy+2.f}, "x"+std::to_string(slot->item->count), {1,1,0.4f,1}, 9.f);
            if (slot->item->integrity < 1.f) {
                float fil = std::max(0.f, slot->item->integrity) * (HAND_SZ-4.f);
                glm::vec4 ic = (slot->item->integrity>0.5f)
                    ? glm::vec4{0.15f,0.85f,0.25f,0.9f} : glm::vec4{0.90f,0.28f,0.08f,0.9f};
                m_ui.rect({px+2.f,hy+HAND_SZ-3.f}, {HAND_SZ-4.f,2.f}, {0.12f,0.12f,0.12f,0.7f},0.f);
                m_ui.rect({px+2.f,hy+HAND_SZ-3.f}, {fil,2.f}, ic, 0.f);
            }
        }
        if (hov && click) out_click = sid;
    };

    draw_one(lx, "l_hand",  left_active);
    draw_one(rx, "r_hand", !left_active);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Body-equipment panel  (2 rows × 4 cols)
//  Row 0: HEAD  EYES  EARS  MASK
//  Row 1: SUIT  UNIF  GLVS  BOOT
//  panel_tl = top-left of the inner slot area (inside padding).
// ─────────────────────────────────────────────────────────────────────────────
void HUD::draw_body_equip(const Inventory& inv, glm::vec2 panel_tl,
                           glm::vec2 mouse, bool click, std::string& out_click)
{
    constexpr float SEP     = 4.f;
    constexpr float PAD     = 6.f;
    constexpr int   COLS    = 3;
    constexpr int   ROWS    = 4;

    // TG pop-up inventory layout (cols = WEST / WEST+1 / WEST+2, rows top→bottom):
    //  Row 0 (SOUTH+3): eyes     head    —
    //  Row 1 (SOUTH+2): neck     mask    ears
    //  Row 2 (SOUTH+1): uniform  suit    gloves
    //  Row 3 (SOUTH+0): —        shoes   —
    struct E { const char* id; const char* lbl; };
    static const E k[ROWS][COLS] = {
        { {"eyes","EYES"},    {"head","HEAD"},  {"",      ""    } },
        { {"neck","NECK"},    {"mask","MASK"},  {"ears",  "EARS"} },
        { {"uniform","UNIF"}, {"suit","SUIT"},  {"gloves","GLVS"} },
        { {"",      ""},      {"boots","BOOT"}, {"",      ""    } },
    };

    // Panel background
    const float inner_w = COLS * EQUIP_SZ + (COLS - 1) * SEP;
    const float inner_h = ROWS * EQUIP_SZ + (ROWS - 1) * SEP;
    const glm::vec2 bg_tl = panel_tl - glm::vec2(PAD, PAD);
    const glm::vec2 bg_sz = {inner_w + 2.f * PAD, inner_h + 2.f * PAD};


    for (int row = 0; row < ROWS; ++row)
        for (int col = 0; col < COLS; ++col) {
            if (k[row][col].id[0] == '\0') continue;   // empty cell
            glm::vec2 p = panel_tl + glm::vec2(col * (EQUIP_SZ + SEP),
                                               row * (EQUIP_SZ + SEP));
            if (draw_slot(inv, k[row][col].id, p, EQUIP_SZ,
                          k[row][col].lbl, false, mouse, click))
                out_click = k[row][col].id;
        }
}

// ─────────────────────────────────────────────────────────────────────────────
//  draw_zone_intent — bottom-right cluster: intent | movi | zone_sel
//
//  Button size = EQUIP_SZ (56px), gap = EQUIP_GAP (4px) — same as every other slot.
//  Row aligns with slot_y inside the bar (swap strip + centred in remaining space).
// ─────────────────────────────────────────────────────────────────────────────
void HUD::draw_zone_intent(HUDState& s, glm::vec2 mouse, bool click)
{
    const float fb_w  = static_cast<float>(m_ui.fb_width());
    const float fb_h  = static_cast<float>(m_ui.fb_height());
    const float bar_y = fb_h - BAR_H;

    constexpr float BTN = EQUIP_SZ;   // 56px — same as hand/equip slots
    constexpr float SEP = EQUIP_GAP;  //  4px gap

    // 3 columns packed right→left with 8px right margin (mirrors 6px left margin)
    const float col_zone_x   = fb_w - BTN - 8.f;
    const float col_movi_x   = col_zone_x   - BTN - SEP;
    const float col_intent_x = col_movi_x   - BTN - SEP;

    // Row: same vertical centre as all other bar slots
    const float row_main_y = bar_y + SWAP_H + (BAR_H - SWAP_H - BTN) * 0.5f;

    // ── intent | movi | zone_sel ─────────────────────────────────────────────

    // Intent (col_intent, SOUTH) — cycles Help→Disarm→Grab→Harm
    {
        glm::vec2 p = {col_intent_x, row_main_y};
        int idx = static_cast<int>(s.intent);
        bool hov = (mouse.x >= p.x && mouse.x < p.x + BTN &&
                    mouse.y >= p.y && mouse.y < p.y + BTN);
        if (click && hov)
            s.intent = static_cast<Intent>((idx + 1) % 4);
        if (hov)
            m_ui.rect({p.x - 1.f, p.y - 1.f}, {BTN + 2.f, BTN + 2.f},
                      {1.f, 1.f, 1.f, 0.12f}, 3.f);
        if (m_intent_tex[idx])
            m_ui.image(p, {BTN, BTN}, m_intent_tex[idx]);
        else {
            m_ui.rect(p, {BTN, BTN}, k_intent_col[idx], 3.f);
            m_ui.text({p.x + 8.f, p.y + BTN * 0.5f - 5.f}, k_intent_label[idx], {1.f, 1.f, 1.f, 1.f}, 10.f);
        }
    }

    // Movement intent (col_movi, SOUTH) — walk/run toggle
    {
        glm::vec2 p = {col_movi_x, row_main_y};
        bool hov = (mouse.x >= p.x && mouse.x < p.x + BTN &&
                    mouse.y >= p.y && mouse.y < p.y + BTN);
        if (click && hov)
            s.is_running = !s.is_running;
        SDL_GPUTexture* tex = s.is_running ? m_running_tex : m_walking_tex;
        if (tex)
            m_ui.image(p, {BTN, BTN}, tex, hov ? 1.0f : 0.85f);
        else {
            glm::vec4 col = s.is_running
                ? glm::vec4{0.14f, 0.52f, 0.14f, 0.90f}
                : glm::vec4{0.18f, 0.30f, 0.55f, 0.90f};
            m_ui.rect(p, {BTN, BTN}, col, 3.f);
            const char* lbl = s.is_running ? "RUN" : "WLK";
            m_ui.text({p.x + 12.f, p.y + BTN * 0.5f - 5.f}, lbl, {1.f, 1.f, 1.f, 1.f}, 10.f);
        }
    }

    // Zone selector (col_zone, SOUTH) — click body zones to target
    {
        glm::vec2 p = {col_zone_x, row_main_y};
        if (m_doll_base_tex)
            m_ui.image(p, {BTN, BTN}, m_doll_base_tex);
        const int sel_z = static_cast<int>(s.target_zone);
        if (sel_z < 7 && m_zone_sel_tex[sel_z])
            m_ui.image(p, {BTN, BTN}, m_zone_sel_tex[sel_z]);

        // Listed from most-specific to least-specific so "first match wins" gives correct
        // priority where AABBs overlap (e.g. arm over chest, legs over groin).
        struct ZR { BodyZone zone; glm::vec2 tl; glm::vec2 sz; };
        static const ZR k_z[] = {
            { BodyZone::Head,  {10.f,  1.f}, {12.f,  9.f} },  // head (top, small)
            { BodyZone::LArm,  { 0.f,  7.f}, { 6.f, 13.f} },  // narrow arms before chest
            { BodyZone::RArm,  {26.f,  7.f}, { 6.f, 13.f} },
            { BodyZone::LLeg,  { 4.f, 23.f}, {10.f,  9.f} },  // legs before groin
            { BodyZone::RLeg,  {18.f, 23.f}, {10.f,  9.f} },
            { BodyZone::Groin, { 7.f, 18.f}, {18.f,  7.f} },  // groin before chest
            { BodyZone::Chest, { 4.f,  8.f}, {24.f, 11.f} },  // chest last (large catch-all)
        };
        const float scale = BTN / 32.f;
        for (const auto& z : k_z) {
            glm::vec2 hp  = p + z.tl * scale;
            glm::vec2 hsz = z.sz * scale;
            bool hov = (mouse.x >= hp.x && mouse.x < hp.x + hsz.x &&
                        mouse.y >= hp.y && mouse.y < hp.y + hsz.y);
            if (click && hov) {
                s.target_zone = z.zone;
                break; // first match wins; more specific zones are listed earlier
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Examine label
// ─────────────────────────────────────────────────────────────────────────────
void HUD::draw_examine_label(const std::string& label)
{
    const float fb_w = static_cast<float>(m_ui.fb_width());
    const float fb_h = static_cast<float>(m_ui.fb_height());
    glm::vec2 pos = {fb_w*0.5f+24.f, fb_h*0.5f-10.f};
    float w = static_cast<float>(label.size()) * 7.f + 10.f;
    m_ui.rect(pos-glm::vec2(5.f,2.f), {w,18.f}, {0,0,0,0.00f}, 3.f);
    m_ui.text(pos, label, {1.f,1.f,0.75f,1.f}, 13.f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Radio / chat log — left side, above the bar
// ─────────────────────────────────────────────────────────────────────────────
void HUD::draw_radio_log(const std::deque<std::string>& log)
{
    constexpr float LINE_H = 14.f, PAD_X = 8.f;
    constexpr int MAX_SHOW = 8;
    const float fb_h   = static_cast<float>(m_ui.fb_height());
    const float base_y = fb_h - BAR_H - LINE_H - 4.f;
    int shown = 0;
    for (auto it = log.rbegin(); it != log.rend() && shown < MAX_SHOW; ++it, ++shown) {
        float y = base_y - shown * LINE_H;
        m_ui.text({PAD_X+1.f, y+1.f}, *it, {0,0,0,0.55f}, 11.f);
        m_ui.text({PAD_X,     y    }, *it, {0.82f,0.90f,1.f,0.88f}, 11.f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Clock — top-right corner
// ─────────────────────────────────────────────────────────────────────────────
void HUD::draw_clock(const std::string& time_str)
{
    if (time_str.empty()) return;
    const float fb_w = static_cast<float>(m_ui.fb_width());
    m_ui.text({fb_w-81.f, 11.f}, time_str, {0,0,0,0.50f}, 13.f);
    m_ui.text({fb_w-82.f, 10.f}, time_str, {0.85f,0.90f,0.95f,0.88f}, 13.f);
}