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
static constexpr float BAR_H     = 68.f;   // total bar height (slots are 56px + 6px padding each side)
static constexpr float BAR_PAD   =  7.f;   // top/bottom inner padding
static constexpr float HAND_SZ   = 56.f;   // hand slot square
static constexpr float HAND_GAP  = 10.f;   // gap between the two hands
static constexpr float EQUIP_SZ  = HAND_SZ; // all slots same size as hands
static constexpr float EQUIP_GAP =  4.f;
static constexpr float SEC_GAP   = 10.f;   // gap between sections

// Right-side health panel constants
static constexpr float PANEL_DOLL_SZ = 64.f;  // health doll sprite size
static constexpr float PANEL_BAR_W   = 80.f;  // damage bar width
static constexpr float PANEL_W       = PANEL_DOLL_SZ + 6.f + PANEL_BAR_W + 6.f;
static constexpr float PANEL_H       = 90.f;  // enough for doll + bar rows
static constexpr float PANEL_RIGHT   = 8.f;   // margin from right edge

// ─────────────────────────────────────────────────────────────────────────────
//  Colour constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr glm::vec4 k_bar_bg    = {0.04f, 0.05f, 0.07f, 0.90f};
static constexpr glm::vec4 k_bar_top   = {0.22f, 0.45f, 0.88f, 0.70f};
static constexpr glm::vec4 k_slot_bg   = {0.11f, 0.11f, 0.14f, 0.85f};
static constexpr glm::vec4 k_slot_act  = {0.18f, 0.28f, 0.52f, 0.92f};
static constexpr glm::vec4 k_slot_ring = {0.32f, 0.58f, 1.00f, 0.72f};
static constexpr glm::vec4 k_sec_div   = {0.20f, 0.26f, 0.40f, 0.60f};
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
                      glm::vec2 mouse_pos, bool lmb_clicked,
                      SDL_GPUTexture* player_mirror_tex)
{
    const float fb_w   = static_cast<float>(m_ui.fb_width());
    const float fb_h   = static_cast<float>(m_ui.fb_height());
    const float bar_y  = fb_h - BAR_H;
    const float slot_y = bar_y + (BAR_H - EQUIP_SZ) * 0.5f;
    const float hand_y = bar_y + (BAR_H - HAND_SZ)  * 0.5f;
    const float cx     = fb_w * 0.5f;

    // Hand slot edges — symmetric around cx with a HAND_GAP gap at the center.
    // Left-hand left-edge:  cx - HAND_SZ - HAND_GAP/2
    // Right-hand left-edge: cx + HAND_GAP/2
    const float lhand_x  = cx - HAND_SZ - HAND_GAP * 0.5f;
    const float rhand_end = lhand_x + HAND_SZ + HAND_GAP + HAND_SZ;  // right edge of r_hand
    constexpr float SEP = 4.f;   // gap between adjacent slots

    // ── Right-side floating health panel (TG: ui_healthdoll EAST-1:28, CENTER) ─
    {
        const float panel_x = fb_w - PANEL_W - PANEL_RIGHT;
        const float panel_y = fb_h * 0.5f - PANEL_H * 0.5f;
        draw_health_panel(state, {panel_x, panel_y});
    }

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
            ? glm::vec4{0.22f, 0.38f, 0.72f, 0.96f}
            : (hov ? glm::vec4{0.22f, 0.32f, 0.52f, 0.92f}
                   : glm::vec4{0.11f, 0.13f, 0.20f, 0.85f});
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

    // ── EAST edge: intent + zone selector ────────────────────────────────────
    draw_zone_intent(state, mouse_pos, click);

    // ── Body-slot panel (shown above bar when inv_open) ───────────────────────
    if (state.inv_open) {
        constexpr float PAD = 6.f;
        constexpr float PROWS = 2.f;
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
    draw_mirror(player_mirror_tex);
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
//  draw_health_panel — right-side floating panel (TG: EAST-1, CENTER)
//  Contains: body doll sprites + SS13 damage bars + env readout
// ─────────────────────────────────────────────────────────────────────────────
void HUD::draw_health_panel(const HUDState& s, glm::vec2 origin)
{
    float ratio = (s.health_max > 0.f)
        ? std::clamp(s.health / s.health_max, 0.f, 1.f) : 0.f;
    float pulse = (s.in_crit || s.dead)
        ? (0.5f + 0.5f * std::sin(static_cast<float>(SDL_GetTicks()) * 0.008f)) : 0.f;

    // Semi-transparent panel background
    const float panel_pad = 5.f;
    m_ui.rect(origin - glm::vec2(panel_pad),
              {PANEL_W + panel_pad*2.f, PANEL_H + panel_pad*2.f},
              {0.04f, 0.05f, 0.07f, 0.82f}, 5.f);

    // ── Sprite body doll (64×64) ─────────────────────────────────────────────
    {
        int lvl = s.dead ? 4 : std::min(4, static_cast<int>((1.f - ratio) * 5.f));
        float base_alpha = s.dead ? 0.55f : 1.f;

        // living1-4 add injury overlays; living0 is just a solid-colour placeholder so skip it
        if (lvl > 0 && m_living_tex[lvl])
            m_ui.image(origin, {PANEL_DOLL_SZ, PANEL_DOLL_SZ}, m_living_tex[lvl], base_alpha);

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
                m_ui.image(origin, {PANEL_DOLL_SZ, PANEL_DOLL_SZ}, m_zone_dmg_tex[z][zone_lvl[z]]);
    }

    // ── SS13-style damage bars ────────────────────────────────────────────────
    const glm::vec2 ri   = origin + glm::vec2(PANEL_DOLL_SZ + 6.f, 2.f);
    const float bar_row  = 10.f;
    const float bar_h2   =  6.f;

    struct DmgBar { const char* lbl; float dmg; glm::vec4 fill; glm::vec4 track; };
    const DmgBar dbars[] = {
        { "BRT", s.dmg_brute, {0.82f, 0.12f, 0.12f, 0.92f}, {0.28f, 0.08f, 0.08f, 0.65f} },
        { "BRN", s.dmg_burn,  {0.92f, 0.42f, 0.05f, 0.92f}, {0.30f, 0.15f, 0.04f, 0.65f} },
        { "TOX", s.dmg_tox,   {0.18f, 0.82f, 0.18f, 0.92f}, {0.08f, 0.28f, 0.08f, 0.65f} },
        { "OXY", s.dmg_oxy,   {0.15f, 0.62f, 0.92f, 0.92f}, {0.06f, 0.20f, 0.32f, 0.65f} },
    };
    for (int i = 0; i < 4; ++i) {
        const auto& b = dbars[i];
        float y = ri.y + static_cast<float>(i) * bar_row;
        float fill_px = (s.health_max > 0.f)
                        ? std::clamp(b.dmg / s.health_max, 0.f, 1.f) * PANEL_BAR_W : 0.f;
        m_ui.text({ri.x, y + 0.5f}, b.lbl,
                  (b.dmg > 0.5f) ? b.fill : glm::vec4{0.28f, 0.34f, 0.46f, 0.55f}, 8.f);
        m_ui.rect({ri.x + 22.f, y + 1.f}, {PANEL_BAR_W, bar_h2}, b.track, 2.f);
        if (fill_px > 0.f)
            m_ui.rect({ri.x + 22.f, y + 1.f}, {fill_px, bar_h2}, b.fill, 2.f);
    }

    // ── Suit pressure / temperature readout ──────────────────────────────────
    {
        auto fmt = [](float v, int dec) -> std::string {
            std::ostringstream o; o << std::fixed << std::setprecision(dec) << v; return o.str();
        };
        const float bot_y = ri.y + 4.f * bar_row + 3.f;
        glm::vec4 pc = (s.suit_pressure_kpa < 20.f || s.suit_pressure_kpa > 550.f)
            ? glm::vec4{1.f, 0.38f, 0.08f, 1.f}
            : (s.suit_pressure_kpa < 70.f || s.suit_pressure_kpa > 300.f)
            ? glm::vec4{1.f, 0.75f, 0.10f, 1.f}
            : glm::vec4{0.55f, 0.90f, 0.55f, 0.80f};
        m_ui.text({ri.x,        bot_y}, "KPA", pc, 8.f);
        m_ui.text({ri.x + 22.f, bot_y}, fmt(s.suit_pressure_kpa, 0), pc, 8.f);
        if (!s.suit_temp_str.empty())
            m_ui.text({ri.x, bot_y + 10.f}, "T " + s.suit_temp_str,
                      {0.70f, 0.70f, 0.70f, 0.72f}, 8.f);
    }

    // ── Pull indicator (pull.png) — shown top-right of doll when dragging something ───
    if (s.is_pulling && m_pull_tex) {
        constexpr float PULL_SZ = 16.f;
        m_ui.image(origin + glm::vec2(PANEL_DOLL_SZ - PULL_SZ - 2.f, -PULL_SZ - 2.f),
                   {PULL_SZ, PULL_SZ}, m_pull_tex);
    }

    // ── Dead / Critical status flash ──────────────────────────────────────────
    if (s.dead) {
        m_ui.text(origin + glm::vec2(PANEL_DOLL_SZ*0.5f - 14.f, PANEL_DOLL_SZ + 2.f),
                  "DEAD", {0.55f, 0.55f, 0.60f, 0.90f}, 9.f);
    } else if (s.in_crit && pulse > 0.42f) {
        m_ui.text(origin + glm::vec2(0.f, PANEL_DOLL_SZ + 2.f),
                  "CRITICAL", {1.f, 0.06f, 0.06f, 0.88f + pulse * 0.12f}, 9.f);
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

        if (active)
            m_ui.rect({px-3.f,hy-3.f}, {HAND_SZ+6.f,HAND_SZ+6.f}, k_slot_ring, 7.f);

        // Amber ring for the gripped (support) hand
        if (is_grip)
            m_ui.rect({px-3.f,hy-3.f}, {HAND_SZ+6.f,HAND_SZ+6.f},
                      {0.80f,0.52f,0.05f,0.85f}, 7.f);

        bool hov = (mouse.x >= px && mouse.x < px+HAND_SZ &&
                    mouse.y >= hy && mouse.y < hy+HAND_SZ);
        glm::vec4 bg = is_grip ? glm::vec4{0.35f,0.22f,0.03f,0.85f} : k_slot_bg;
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

    // Arrow between hands indicates active side
    const char* arr = left_active ? "<" : ">";
    m_ui.text({lx+HAND_SZ+HAND_GAP*0.5f-4.f, hy+HAND_SZ*0.5f-6.f},
              arr, {0.50f,0.72f,1.f,0.88f}, 12.f);
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
    constexpr int   COLS    = 4;
    constexpr int   ROWS    = 2;

    struct E { const char* id; const char* lbl; };
    static const E k[ROWS][COLS] = {
        { {"head","HEAD"}, {"eyes","EYES"}, {"ears","EARS"}, {"mask","MASK"} },
        { {"suit","SUIT"}, {"uniform","UNIF"}, {"gloves","GLVS"}, {"boots","BOOT"} },
    };

    // Panel background
    const float inner_w = COLS * EQUIP_SZ + (COLS - 1) * SEP;
    const float inner_h = ROWS * EQUIP_SZ + (ROWS - 1) * SEP;
    const glm::vec2 bg_tl = panel_tl - glm::vec2(PAD, PAD);
    const glm::vec2 bg_sz = {inner_w + 2.f * PAD, inner_h + 2.f * PAD};
    m_ui.rect(bg_tl, bg_sz, {0.04f, 0.05f, 0.07f, 0.93f}, 6.f);
    // Top accent line (matches bar top line colour)
    m_ui.rect(bg_tl, {bg_sz.x, 2.f}, k_bar_top, 0.f);

    for (int row = 0; row < ROWS; ++row)
        for (int col = 0; col < COLS; ++col) {
            glm::vec2 p = panel_tl + glm::vec2(col * (EQUIP_SZ + SEP),
                                               row * (EQUIP_SZ + SEP));
            if (draw_slot(inv, k[row][col].id, p, EQUIP_SZ,
                          k[row][col].lbl, false, mouse, click))
                out_click = k[row][col].id;
        }
}

// ─────────────────────────────────────────────────────────────────────────────
//  draw_zone_intent — bottom-right corner, TG positions:
//    ui_acti    "EAST-3:24, SOUTH:5"  → intent sprite (32×32)
//    ui_zonesel "EAST-1:28, SOUTH:5"  → zone body doll (32×32)
//
//  Intent sprites (help/disarm/grab/harm.png) are 32×32 sheets that already
//  show all 4 intents with the relevant one highlighted.  No tinting needed.
//  Clicking cycles Help→Disarm→Grab→Harm→Help.
// ─────────────────────────────────────────────────────────────────────────────
void HUD::draw_zone_intent(HUDState& s, glm::vec2 mouse, bool click)
{
    const float fb_w  = static_cast<float>(m_ui.fb_width());
    const float fb_h  = static_cast<float>(m_ui.fb_height());
    const float bar_y = fb_h - BAR_H;

    // TG positions (EAST = right edge, tile = 32 px, extra pixel offset included)
    // ui_zonesel  "EAST-1:28, SOUTH:5"  → right-most cluster item
    // ui_acti     "EAST-3:24, SOUTH:5"  → two tiles further left
    // BYOND EAST-N means N tiles inward from right edge; offset is sub-tile pixels.
    // In our coordinate system, origin of sprite = fb_w - N*32 - (32 - offset)
    const float SPRITE_SZ = HAND_SZ;
    constexpr float SPRITE_GAP = 4.f;
    const float zone_x   = fb_w - SPRITE_SZ - 8.f;
    const float intent_x = zone_x   - SPRITE_SZ - SPRITE_GAP;
    const float movi_x   = intent_x - SPRITE_SZ - SPRITE_GAP;
    const float sprite_y = bar_y + (BAR_H - SPRITE_SZ) * 0.5f;

    // ── Movement intent button (walk/run toggle) ───────────────────────────────
    {
        bool hov_movi = (mouse.x >= movi_x && mouse.x < movi_x + SPRITE_SZ &&
                         mouse.y >= sprite_y && mouse.y < sprite_y + SPRITE_SZ);
        if (click && hov_movi)
            s.is_running = !s.is_running;

        glm::vec4 movi_bg = s.is_running
            ? glm::vec4{0.14f, 0.52f, 0.14f, 0.90f}    // green = running
            : glm::vec4{0.18f, 0.30f, 0.55f, 0.90f};   // blue  = walking
        if (hov_movi) movi_bg.a = 1.f;
        SDL_GPUTexture* movi_sprite = s.is_running ? m_running_tex : m_walking_tex;
        if (movi_sprite)
            m_ui.image({movi_x, sprite_y}, {SPRITE_SZ, SPRITE_SZ}, movi_sprite,
                       hov_movi ? 1.0f : 0.85f);
        else {
            m_ui.rect({movi_x, sprite_y}, {SPRITE_SZ, SPRITE_SZ}, movi_bg, 4.f);
            const char* movi_lbl = s.is_running ? "RUN" : "WLK";
            float tw = static_cast<float>(SDL_strlen(movi_lbl)) * 5.8f;
            m_ui.text({movi_x + (SPRITE_SZ - tw) * 0.5f, sprite_y + SPRITE_SZ * 0.5f - 5.f},
                      movi_lbl,
                      s.is_running ? glm::vec4{0.5f, 1.f, 0.5f, 1.f}
                                   : glm::vec4{0.6f, 0.85f, 1.f, 1.f}, 8.f);
        }
    }

    // ── Intent button ─────────────────────────────────────────────────────────
    int idx = static_cast<int>(s.intent);
    bool hov_intent = (mouse.x >= intent_x && mouse.x < intent_x + SPRITE_SZ &&
                       mouse.y >= sprite_y  && mouse.y < sprite_y  + SPRITE_SZ);
    if (click && hov_intent)
        s.intent = static_cast<Intent>((idx + 1) % 4);

    if (hov_intent)
        m_ui.rect({intent_x - 1.f, sprite_y - 1.f}, {SPRITE_SZ + 2.f, SPRITE_SZ + 2.f},
                  {1.f, 1.f, 1.f, 0.12f}, 3.f);

    if (m_intent_tex[idx])
        m_ui.image({intent_x, sprite_y}, {SPRITE_SZ, SPRITE_SZ}, m_intent_tex[idx]);

    // ── Zone selector (ui_zonesel) ─────────────────────────────────────────────
    // Click hit-zones — coordinates defined on a 32×32 canvas, scaled to SPRITE_SZ
    struct ZR { BodyZone zone; glm::vec2 tl; glm::vec2 sz; };
    static const ZR k_z[] = {
        { BodyZone::Head,  {10.f,  1.f}, {12.f,  9.f} },
        { BodyZone::Chest, { 4.f,  8.f}, {24.f, 11.f} },
        { BodyZone::LArm,  { 0.f,  7.f}, { 6.f, 13.f} },
        { BodyZone::RArm,  {26.f,  7.f}, { 6.f, 13.f} },
        { BodyZone::Groin, { 7.f, 18.f}, {18.f,  7.f} },
        { BodyZone::LLeg,  { 4.f, 23.f}, {10.f,  9.f} },
        { BodyZone::RLeg,  {18.f, 23.f}, {10.f,  9.f} },
    };
    const float scale = SPRITE_SZ / 32.f;
    const int sel_z = static_cast<int>(s.target_zone);

    if (m_doll_base_tex)
        m_ui.image({zone_x, sprite_y}, {SPRITE_SZ, SPRITE_SZ}, m_doll_base_tex);
    if (sel_z < 7 && m_zone_sel_tex[sel_z])
        m_ui.image({zone_x, sprite_y}, {SPRITE_SZ, SPRITE_SZ}, m_zone_sel_tex[sel_z]);

    for (int i = 0; i < 7; ++i) {
        const auto& z = k_z[i];
        glm::vec2 p  = glm::vec2{zone_x, sprite_y} + z.tl * scale;
        glm::vec2 sz = z.sz * scale;
        bool hov = (mouse.x >= p.x && mouse.x < p.x + sz.x &&
                    mouse.y >= p.y && mouse.y < p.y + sz.y);
        if (click && hov) s.target_zone = z.zone;
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
    m_ui.rect(pos-glm::vec2(5.f,2.f), {w,18.f}, {0,0,0,0.65f}, 3.f);
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
// ─────────────────────────────────────────────────────────────────────────────
//  Mirror — top-left corner, shows the player's front-facing assembled sprite
//  so they can see how other players see them.
// ─────────────────────────────────────────────────────────────────────────────
void HUD::draw_mirror(SDL_GPUTexture* sprite_tex)
{
    if (!sprite_tex) return;

    // Layout constants
    constexpr float SPRITE_PX  = 64.f;   // 2× pixel-art scale of 32px sprite
    constexpr float PAD        =  6.f;
    constexpr float LABEL_H    = 14.f;
    constexpr float PANEL_W    = SPRITE_PX + PAD * 2.f;
    constexpr float PANEL_H    = PAD + LABEL_H + PAD * 0.5f + SPRITE_PX + PAD;
    constexpr float MARGIN     =  8.f;

    const glm::vec2 panel_pos  = { MARGIN, MARGIN };

    // Background + border
    static constexpr glm::vec4 k_bg     = { 0.04f, 0.05f, 0.07f, 0.88f };
    static constexpr glm::vec4 k_border = { 0.32f, 0.58f, 1.00f, 0.55f };
    static constexpr glm::vec4 k_label  = { 0.70f, 0.82f, 1.00f, 0.90f };

    // Outer border (1-px inset)
    m_ui.rect(panel_pos - glm::vec2(1.f), { PANEL_W + 2.f, PANEL_H + 2.f }, k_border, 4.f);
    // Background
    m_ui.rect(panel_pos, { PANEL_W, PANEL_H }, k_bg, 4.f);

    // Label "YOU"
    m_ui.text({ panel_pos.x + PAD, panel_pos.y + PAD * 0.5f },
              "YOU", k_label, 11.f);

    // Sprite (pixel-perfect scaled 32→64)
    const glm::vec2 sprite_pos = {
        panel_pos.x + PAD,
        panel_pos.y + PAD + LABEL_H + PAD * 0.5f
    };
    m_ui.image(sprite_pos, { SPRITE_PX, SPRITE_PX }, sprite_tex, 1.f);
}