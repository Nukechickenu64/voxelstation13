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
static constexpr float BAR_H     = 78.f;   // total bar height
static constexpr float BAR_PAD   =  7.f;   // top/bottom inner padding
static constexpr float HAND_SZ   = 56.f;   // hand slot square
static constexpr float HAND_GAP  = 10.f;   // gap between the two hands
static constexpr float EQUIP_SZ  = 32.f;   // equipment slot (2 rows fit: 2*32+4=68 ≈ inner)
static constexpr float EQUIP_GAP =  4.f;
static constexpr float INTENT_SZ = 32.f;
static constexpr float INTENT_GAP=  4.f;
static constexpr float SEC_GAP   = 10.f;   // gap between sections

// Status section widths
static constexpr float STATUS_DOLL_W = 30.f;
static constexpr float STATUS_INFO_W = 116.f;
static constexpr float STATUS_TOT_W  = STATUS_DOLL_W + 6.f + STATUS_INFO_W;

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
HUD::HUD(UIRenderer& ui) : m_ui(ui) {}

// ─────────────────────────────────────────────────────────────────────────────
//  draw() — master layout: unified full-width bottom bar
// ─────────────────────────────────────────────────────────────────────────────
std::string HUD::draw(HUDState& state, const Inventory& inv,
                      glm::vec2 mouse_pos, bool lmb_clicked)
{
    const float fb_w   = static_cast<float>(m_ui.fb_width());
    const float fb_h   = static_cast<float>(m_ui.fb_height());
    const float bar_y  = fb_h - BAR_H;
    const float inner_y = bar_y + BAR_PAD;

    // ── Full-width bottom bar ─────────────────────────────────────────────────
    m_ui.rect({0.f, bar_y}, {fb_w, BAR_H}, k_bar_bg,  0.f);
    m_ui.rect({0.f, bar_y}, {fb_w, 2.f},   k_bar_top, 0.f);

    // Only allow HUD slot clicks when the cursor is inside the bar area
    const bool in_bar = (mouse_pos.y >= bar_y - 2.f);
    const bool click  = in_bar && lmb_clicked;

    std::string clicked;

    // Helper: thin vertical divider
    auto div = [&](float x) {
        m_ui.rect({x, bar_y + 8.f}, {1.f, BAR_H - 16.f}, k_sec_div, 0.f);
    };

    // ── 1. Status section (leftmost) ─────────────────────────────────────────
    draw_status_section(state, {0.f, bar_y}, BAR_H);
    float x = BAR_PAD + STATUS_TOT_W + SEC_GAP;
    div(x - SEC_GAP * 0.5f);

    // ── 2. Body-equipment cluster  (4 cols × 2 rows) ─────────────────────────
    draw_body_equip(inv, {x, inner_y}, mouse_pos, click, clicked);
    x += 4.f * (EQUIP_SZ + EQUIP_GAP);
    div(x + SEC_GAP * 0.5f - EQUIP_GAP);
    x += SEC_GAP - EQUIP_GAP;

    // ── 3+4+5. Left pocket | Hands | Right pocket  (all anchored to screen center)
    {
        const float hands_w = HAND_SZ * 2.f + HAND_GAP;
        const float hands_x = fb_w * 0.5f - hands_w * 0.5f;
        const float hand_y  = bar_y + (BAR_H - HAND_SZ) * 0.5f;
        const float pkt_y   = bar_y + (BAR_H - EQUIP_SZ) * 0.5f;
        const float pkt_gap = 4.f;

        // Left pocket immediately left of left hand
        if (draw_slot(inv, "l_pocket", {hands_x - pkt_gap - EQUIP_SZ, pkt_y},
                      EQUIP_SZ, "PKT", false, mouse_pos, click))
            clicked = "l_pocket";

        draw_hand_slots(inv, state.active_hand_is_left,
                        {hands_x, hand_y}, mouse_pos, click, clicked);

        // Right pocket immediately right of right hand
        if (draw_slot(inv, "r_pocket", {hands_x + hands_w + pkt_gap, pkt_y},
                      EQUIP_SZ, "PKT", false, mouse_pos, click))
            clicked = "r_pocket";

        x = hands_x + hands_w + pkt_gap + EQUIP_SZ + SEC_GAP;
    }
    div(x - SEC_GAP * 0.5f);

    // ── 6. Storage cluster  (2 cols × 2 rows) ────────────────────────────────
    draw_storage_equip(inv, {x, inner_y}, mouse_pos, click, clicked);
    x += 2.f * (EQUIP_SZ + EQUIP_GAP);
    div(x + SEC_GAP * 0.5f - EQUIP_GAP);

    // ── 7. Intent 2×2 + Zone selector (right-anchored) ───────────────────────
    {
        const float intent_blk = INTENT_SZ * 2.f + INTENT_GAP;
        const float zone_blk   = 46.f;
        const float intent_x   = fb_w - BAR_PAD - intent_blk;
        const float zone_x     = intent_x - SEC_GAP - zone_blk;
        draw_intent_zone(state,
                         {zone_x,   inner_y},
                         {intent_x, inner_y},
                         mouse_pos, click);
    }

    // ── Overlays above the bar ────────────────────────────────────────────────
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

    glm::vec4 bg = highlight_active ? k_slot_act : k_slot_bg;
    if (hov) bg = {bg.r + 0.06f, bg.g + 0.07f, bg.b + 0.10f, bg.a};
    m_ui.rect(pos, {sz, sz}, bg, 4.f);

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
        float tw = static_cast<float>(strlen(fallback_label)) * 5.8f;
        m_ui.text(pos + glm::vec2((sz - tw) * 0.5f, sz * 0.5f - 5.f),
                  fallback_label, {0.26f,0.30f,0.40f,0.68f}, 8.f);
    }
    return hov && click;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Status section — compact body doll + readouts inside the bar
// ─────────────────────────────────────────────────────────────────────────────
void HUD::draw_status_section(const HUDState& s, glm::vec2 bar_tl, float bar_h)
{
    const glm::vec2 origin = bar_tl + glm::vec2(BAR_PAD);
    const float inner_h = bar_h - BAR_PAD * 2.f;

    float ratio = (s.health_max > 0.f)
        ? std::clamp(s.health / s.health_max, 0.f, 1.f) : 0.f;
    bool  crit  = (ratio < 0.25f);
    float pulse = crit
        ? (0.5f + 0.5f * std::sin(static_cast<float>(SDL_GetTicks()) * 0.008f)) : 0.f;

    glm::vec4 body_col = (ratio > 0.60f)
        ? glm::vec4{0.12f, 0.78f + pulse * 0.04f, 0.12f, 0.95f}
        : (ratio > 0.30f ? glm::vec4{0.82f, 0.72f, 0.05f, 0.95f}
                         : glm::vec4{0.82f + pulse*0.1f, 0.07f, 0.07f, 0.95f});

    // ── Compact body doll (fits in ~30×64px) ──────────────────────────────────
    const glm::vec2 d = origin;
    m_ui.rect(d + glm::vec2( 8.f, 0.f), {13.f, 12.f}, body_col, 2.f); // head
    m_ui.rect(d + glm::vec2(11.f,12.f), { 5.f,  3.f}, body_col, 0.f); // neck
    m_ui.rect(d + glm::vec2( 5.f,15.f), {19.f, 20.f}, body_col, 2.f); // torso
    m_ui.rect(d + glm::vec2( 0.f,15.f), { 4.f, 16.f}, body_col, 1.f); // l.arm
    m_ui.rect(d + glm::vec2(25.f,15.f), { 4.f, 16.f}, body_col, 1.f); // r.arm
    m_ui.rect(d + glm::vec2( 7.f,35.f), {15.f,  7.f}, body_col, 1.f); // groin
    m_ui.rect(d + glm::vec2( 5.f,42.f), { 9.f, 22.f}, body_col, 2.f); // l.leg
    m_ui.rect(d + glm::vec2(15.f,42.f), { 9.f, 22.f}, body_col, 2.f); // r.leg

    // ── Readouts right of doll ────────────────────────────────────────────────
    auto fmt = [](float v, int dec) -> std::string {
        std::ostringstream o; o << std::fixed << std::setprecision(dec) << v; return o.str();
    };
    const glm::vec2 ri = origin + glm::vec2(STATUS_DOLL_W + 6.f, 0.f);
    const float LS = 12.5f;

    glm::vec4 hpc = crit
        ? glm::vec4{1.f, 0.22f, 0.12f, 0.9f + pulse*0.08f}
        : (ratio < 0.6f ? glm::vec4{0.90f,0.75f,0.15f,1.f}
                        : glm::vec4{0.88f,0.90f,0.88f,1.f});
    {
        std::ostringstream hs;
        hs << std::fixed << std::setprecision(0) << s.health << "/" << s.health_max;
        m_ui.text(ri,                       "HP", hpc, 9.f);
        m_ui.text(ri + glm::vec2(16.f,0.f), hs.str(), hpc, 9.f);
    }

    float o2 = s.oxy_sat * 100.f;
    glm::vec4 o2c = (o2 < 50.f) ? glm::vec4{1.f,0.28f,0.08f,1.f} : glm::vec4{0.38f,0.82f,1.f,1.f};
    m_ui.text(ri + glm::vec2(0.f,LS),   "O2", o2c, 9.f);
    m_ui.text(ri + glm::vec2(16.f,LS),  fmt(o2,0)+"%", o2c, 9.f);

    glm::vec4 txc = (s.tox_level > 0.5f) ? glm::vec4{1.f,0.48f,0.04f,1.f} : glm::vec4{0.50f,0.82f,0.38f,0.82f};
    m_ui.text(ri + glm::vec2(0.f,LS*2),  "TOX", txc, 9.f);
    m_ui.text(ri + glm::vec2(22.f,LS*2), fmt(s.tox_level,1), txc, 9.f);

    glm::vec4 pc = (s.suit_pressure_kpa < 20.f || s.suit_pressure_kpa > 250.f)
        ? glm::vec4{1.f,0.38f,0.08f,1.f} : glm::vec4{0.65f,1.f,0.65f,0.88f};
    m_ui.text(ri + glm::vec2(0.f,LS*3),  "KPA", pc, 9.f);
    m_ui.text(ri + glm::vec2(22.f,LS*3), fmt(s.suit_pressure_kpa,0), pc, 9.f);

    if (!s.suit_temp_str.empty())
        m_ui.text(ri + glm::vec2(0.f,LS*4), "T " + s.suit_temp_str,
                  {0.78f,0.78f,0.78f,0.80f}, 9.f);

    if (crit && pulse > 0.42f)
        m_ui.text(ri + glm::vec2(0.f, inner_h - 11.f), "CRITICAL",
                  {1.f, 0.06f, 0.06f, 0.88f + pulse*0.12f}, 9.f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Body-equipment cluster  (4 cols × 2 rows)
//  Row 0: HEAD  EYES  EARS  MASK
//  Row 1: SUIT  UNIF  GLVS  BOOT
// ─────────────────────────────────────────────────────────────────────────────
void HUD::draw_body_equip(const Inventory& inv, glm::vec2 origin,
                           glm::vec2 mouse, bool click, std::string& out_click)
{
    struct E { const char* id; const char* lbl; };
    static const E k[2][4] = {
        { {"head","HEAD"}, {"eyes","EYES"}, {"ears","EARS"}, {"mask","MASK"} },
        { {"suit","SUIT"}, {"uniform","UNIF"}, {"gloves","GLVS"}, {"boots","BOOT"} },
    };
    for (int row = 0; row < 2; ++row)
        for (int col = 0; col < 4; ++col) {
            glm::vec2 p = origin + glm::vec2(col*(EQUIP_SZ+EQUIP_GAP), row*(EQUIP_SZ+EQUIP_GAP));
            if (draw_slot(inv, k[row][col].id, p, EQUIP_SZ, k[row][col].lbl, false, mouse, click))
                out_click = k[row][col].id;
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

    auto draw_one = [&](float px, const char* sid, bool active)
    {
        if (active)
            m_ui.rect({px-3.f,hy-3.f}, {HAND_SZ+6.f,HAND_SZ+6.f}, k_slot_ring, 7.f);

        bool hov = (mouse.x >= px && mouse.x < px+HAND_SZ &&
                    mouse.y >= hy && mouse.y < hy+HAND_SZ);
        glm::vec4 bg = active ? k_slot_act
            : (hov ? glm::vec4{0.17f,0.18f,0.22f,0.88f} : k_slot_bg);
        m_ui.rect({px,hy}, {HAND_SZ,HAND_SZ}, bg, 5.f);

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
//  Storage cluster:  BACK / BELT  (col 0),  ID / PDA  (col 1)
// ─────────────────────────────────────────────────────────────────────────────
void HUD::draw_storage_equip(const Inventory& inv, glm::vec2 origin,
                               glm::vec2 mouse, bool click, std::string& out_click)
{
    struct E { const char* id; const char* lbl; };
    static const E k[2][2] = {
        { {"back","BACK"}, {"id_card","ID"} },
        { {"belt","BELT"}, {"pda","PDA"}    },
    };
    for (int row = 0; row < 2; ++row)
        for (int col = 0; col < 2; ++col) {
            glm::vec2 p = origin + glm::vec2(col*(EQUIP_SZ+EQUIP_GAP), row*(EQUIP_SZ+EQUIP_GAP));
            if (draw_slot(inv, k[row][col].id, p, EQUIP_SZ, k[row][col].lbl, false, mouse, click))
                out_click = k[row][col].id;
        }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Intent 2×2 + Zone body-part selector — all inside the bar
// ─────────────────────────────────────────────────────────────────────────────
void HUD::draw_intent_zone(HUDState& s,
                            glm::vec2 zone_origin,
                            glm::vec2 intent_origin,
                            glm::vec2 mouse, bool click)
{
    // ── Intent 2×2 ────────────────────────────────────────────────────────────
    m_ui.text(intent_origin - glm::vec2(0.f, 10.f), "INTENT",
              {0.40f,0.50f,0.72f,0.70f}, 8.f);

    static const int k_lay[2][2] = { {0,1},{2,3} };
    for (int row = 0; row < 2; ++row)
        for (int col = 0; col < 2; ++col) {
            int idx = k_lay[row][col];
            glm::vec2 p = intent_origin + glm::vec2(col*(INTENT_SZ+INTENT_GAP),
                                                     row*(INTENT_SZ+INTENT_GAP));
            bool is_act = (static_cast<int>(s.intent) == idx);
            bool hov = (mouse.x>=p.x && mouse.x<p.x+INTENT_SZ &&
                        mouse.y>=p.y && mouse.y<p.y+INTENT_SZ);
            if (click && hov) s.intent = static_cast<Intent>(idx);

            glm::vec4 base = k_intent_col[idx];
            float b = is_act ? 1.f : (hov ? 0.58f : 0.28f);
            m_ui.rect(p, {INTENT_SZ,INTENT_SZ}, {base.r*b,base.g*b,base.b*b,0.92f}, 4.f);
            if (is_act)
                m_ui.rect(p-glm::vec2(2.f), {INTENT_SZ+4.f,INTENT_SZ+4.f},
                          {base.r,base.g,base.b,0.85f}, 5.f);
            float tw = static_cast<float>(strlen(k_intent_label[idx])) * 5.8f;
            m_ui.text(p + glm::vec2((INTENT_SZ-tw)*0.5f, INTENT_SZ*0.5f-4.f),
                      k_intent_label[idx],
                      is_act ? glm::vec4{1,1,1,1} : glm::vec4{0.72f,0.72f,0.72f,0.82f}, 8.f);
        }

    // ── Zone body-part selector ───────────────────────────────────────────────
    struct ZR { BodyZone zone; glm::vec2 tl; glm::vec2 sz; };
    static const ZR k_z[] = {
        { BodyZone::Head,  {13.f, 0.f}, {16.f,14.f} },
        { BodyZone::Chest, { 8.f,16.f}, {24.f,18.f} },
        { BodyZone::LArm,  { 0.f,16.f}, { 7.f,15.f} },
        { BodyZone::RArm,  {33.f,16.f}, { 7.f,15.f} },
        { BodyZone::Groin, { 9.f,35.f}, {20.f, 8.f} },
        { BodyZone::LLeg,  { 7.f,44.f}, {10.f,20.f} },
        { BodyZone::RLeg,  {21.f,44.f}, {10.f,20.f} },
    };
    m_ui.text(zone_origin - glm::vec2(0.f, 10.f), "ZONE",
              {0.40f,0.50f,0.72f,0.70f}, 8.f);

    for (const auto& z : k_z) {
        glm::vec2 p = zone_origin + z.tl;
        bool is_act = (s.target_zone == z.zone);
        bool hov = (mouse.x>=p.x && mouse.x<p.x+z.sz.x &&
                    mouse.y>=p.y && mouse.y<p.y+z.sz.y);
        if (click && hov) s.target_zone = z.zone;

        glm::vec4 col = is_act
            ? glm::vec4{0.28f,0.58f,1.00f,0.95f}
            : (hov ? glm::vec4{0.48f,0.52f,0.62f,0.88f}
                   : glm::vec4{0.18f,0.22f,0.30f,0.80f});
        m_ui.rect(p, z.sz, col, 2.f);
        if (is_act)
            m_ui.rect(p-glm::vec2(1.f), z.sz+glm::vec2(2.f), {0.42f,0.72f,1.f,0.68f}, 3.f);
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
