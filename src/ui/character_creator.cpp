#include "ui/character_creator.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Style tables — kept here so nothing else needs to include this big list
// ─────────────────────────────────────────────────────────────────────────────
struct StyleEntry { const char* label; const char* file; };

// Hair styles (subset of TG SS13 preferences)
static const StyleEntry k_hair_styles[] = {
    {"Bald",           "bald"},
    {"Messy",          "hair_messy"},
    {"Short Bangs",    "hair_shortbangs"},
    {"Bob",            "hair_bob"},
    {"Bowl Cut",       "hair_bowlcut"},
    {"Buzz Cut",       "hair_buzzcut"},
    {"Crewcut",        "hair_crewcut"},
    {"Ponytail",       "hair_ponytail"},
    {"Long",           "hair_long"},
    {"Spiky",          "hair_spiky"},
    {"Curls",          "hair_curls"},
    {"Afro",           "hair_afro"},
    {"Bun",            "hair_bun"},
    {"Business",       "hair_business"},
    {"Mohawk",         "hair_spikey"},
    {"Dreads",         "hair_dreads"},
    {"Bedhead",        "hair_bedhead"},
    {"Pompadour",      "hair_pompadour"},
    {"Pixie",          "hair_pixie"},
    {"Braided",        "hair_braided"},
};
static constexpr int k_hair_count =
    static_cast<int>(sizeof(k_hair_styles) / sizeof(k_hair_styles[0]));

// Facial hair styles (male-only)
static const StyleEntry k_facial_styles[] = {
    {"None",           ""},
    {"5 O'Clock",      "facial_fiveoclock"},
    {"Full Beard",     "facial_fullbeard"},
    {"Moustache",      "facial_moustache"},
    {"Goatee",         "facial_gt"},
    {"Van Dyke",       "facial_vandyke"},
    {"Chin Beard",     "facial_chin"},
    {"Mutton Chops",   "facial_mutton"},
    {"Sideburns",      "facial_sideburn"},
    {"3 O'Clock",      "facial_3oclock"},
    {"7 O'Clock",      "facial_7oclock"},
};
static constexpr int k_facial_count =
    static_cast<int>(sizeof(k_facial_styles) / sizeof(k_facial_styles[0]));

// Skin tone presets (8 options matching TG SS13)
static const glm::u8vec4 k_skin_colors[] = {
    {255, 230, 210, 255},  // very light / pale
    {240, 205, 178, 255},  // light
    {205, 175, 149, 255},  // medium light
    {185, 150, 118, 255},  // medium
    {160, 120,  88, 255},  // medium dark
    {128,  88,  55, 255},  // dark
    { 90,  56,  30, 255},  // very dark
    {177, 175, 190, 255},  // grey (synthetic)
};
static constexpr int k_skin_count =
    static_cast<int>(sizeof(k_skin_colors) / sizeof(k_skin_colors[0]));

// Hair / facial hair colour presets
static const glm::u8vec4 k_hair_colors[] = {
    { 12,  10,  10, 255},  // black
    { 89,  60,  30, 255},  // dark brown
    {145, 100,  60, 255},  // chestnut
    {200, 160,  90, 255},  // blonde
    {235, 220, 175, 255},  // platinum blonde
    {175,  60,  35, 255},  // auburn
    {200,  36,  20, 255},  // red
    {185, 185, 185, 255},  // grey
    {245, 245, 245, 255},  // white
    { 65, 128, 220, 255},  // blue (dyed)
    {200,  60, 185, 255},  // purple (dyed)
    { 60, 200,  90, 255},  // green (dyed)
};
static constexpr int k_hair_color_count =
    static_cast<int>(sizeof(k_hair_colors) / sizeof(k_hair_colors[0]));

// ─────────────────────────────────────────────────────────────────────────────
// Colour palette
// ─────────────────────────────────────────────────────────────────────────────
static constexpr glm::vec4 CC_BG          = {0.04f, 0.04f, 0.08f, 1.00f};
static constexpr glm::vec4 CC_BG_BOT      = {0.06f, 0.07f, 0.14f, 1.00f};
static constexpr glm::vec4 CC_PANEL       = {0.08f, 0.09f, 0.16f, 0.95f};
static constexpr glm::vec4 CC_PANEL_L     = {0.07f, 0.08f, 0.14f, 0.98f};
static constexpr glm::vec4 CC_BORDER      = {0.25f, 0.30f, 0.45f, 0.80f};
static constexpr glm::vec4 CC_TITLE       = {0.80f, 0.90f, 1.00f, 1.00f};
static constexpr glm::vec4 CC_LABEL       = {0.65f, 0.75f, 0.90f, 0.90f};
static constexpr glm::vec4 CC_VALUE       = {0.90f, 0.92f, 1.00f, 1.00f};
static constexpr glm::vec4 CC_SEP         = {0.25f, 0.30f, 0.50f, 0.50f};
static constexpr glm::vec4 CC_BTN         = {0.13f, 0.16f, 0.28f, 0.95f};
static constexpr glm::vec4 CC_BTN_HOV     = {0.20f, 0.44f, 0.68f, 0.95f};
static constexpr glm::vec4 CC_BTN_TXT     = {0.92f, 0.95f, 1.00f, 1.00f};
static constexpr glm::vec4 CC_BTN_ACCEPT  = {0.10f, 0.36f, 0.12f, 0.95f};
static constexpr glm::vec4 CC_BTN_ACCEPTH = {0.15f, 0.55f, 0.20f, 0.95f};
static constexpr glm::vec4 CC_BTN_RAND    = {0.20f, 0.18f, 0.35f, 0.95f};
static constexpr glm::vec4 CC_BTN_RANDH   = {0.32f, 0.28f, 0.55f, 0.95f};
static constexpr glm::vec4 CC_BTN_BACK    = {0.22f, 0.12f, 0.12f, 0.95f};
static constexpr glm::vec4 CC_BTN_BACKH   = {0.50f, 0.16f, 0.16f, 0.95f};
static constexpr glm::vec4 CC_INPUT_BG    = {0.06f, 0.07f, 0.12f, 1.00f};
static constexpr glm::vec4 CC_INPUT_BGFOC = {0.10f, 0.14f, 0.22f, 1.00f};
static constexpr glm::vec4 CC_INPUT_BRD   = {0.20f, 0.25f, 0.40f, 0.90f};
static constexpr glm::vec4 CC_INPUT_BRDFOC= {0.40f, 0.60f, 0.90f, 1.00f};

// ─────────────────────────────────────────────────────────────────────────────
// Internal helper: draw a bounded panel with shadow + border
// ─────────────────────────────────────────────────────────────────────────────
static void draw_panel_box(UIRenderer& ui, glm::vec2 pos, glm::vec2 size,
                            glm::vec4 bg, glm::vec4 bord, float rounding = 8.f)
{
    ui.rect({pos.x + 5.f, pos.y + 5.f}, size, {0.f, 0.f, 0.f, 0.45f}, rounding);
    ui.rect(pos, size, bg, rounding);
    const float BT = 1.5f;
    ui.rect(pos,                              {size.x, BT},   bord);
    ui.rect(pos,                              {BT, size.y},   bord);
    ui.rect({pos.x,            pos.y + size.y - BT}, {size.x, BT}, bord);
    ui.rect({pos.x + size.x - BT, pos.y},            {BT, size.y}, bord);
}

// ─────────────────────────────────────────────────────────────────────────────
CharacterCreator::CharacterCreator(UIRenderer& ui, SDL_Window* window,
                                   const CharacterProfile& initial)
    : m_ui(ui), m_window(window), m_profile(initial)
{
    // Resolve colour arrays and file names from index fields
    m_profile.skin_color   = k_skin_colors[ m_profile.skin_idx   % k_skin_count];
    m_profile.hair_color   = k_hair_colors[ m_profile.hair_col_idx   % k_hair_color_count];
    m_profile.facial_color = k_hair_colors[ m_profile.facial_col_idx % k_hair_color_count];
    // Resolve file names
    m_profile.hair_file   = (m_profile.hair_idx > 0 && m_profile.hair_idx < k_hair_count)
                             ? k_hair_styles[m_profile.hair_idx].file : "";
    m_profile.facial_file = (m_profile.facial_idx > 0 && m_profile.facial_idx < k_facial_count)
                             ? k_facial_styles[m_profile.facial_idx].file : "";
    preload_sprites();
}

// ─────────────────────────────────────────────────────────────────────────────
SDL_GPUTexture* CharacterCreator::get_sprite(const std::string& path)
{
    auto it = m_sprites.find(path);
    if (it != m_sprites.end()) return it->second;
    auto* tex = m_ui.load_texture(path.c_str());
    m_sprites[path] = tex;
    return tex;
}

// ─────────────────────────────────────────────────────────────────────────────
void CharacterCreator::preload_sprites()
{
    const char* body_base = "legacysets/extracted/mob/human/bodyparts_greyscale/";
    const char* face_base = "legacysets/extracted/mob/human/human_face/";

    // Body parts for both genders (south-facing only for preview)
    static const char* parts_m[] = {
        "human_r_arm_s", "human_chest_m_s", "human_l_arm_s",
        "human_r_hand_s", "human_l_hand_s",
        "human_r_leg_s", "human_l_leg_s", "human_head_m_s"
    };
    static const char* parts_f[] = {
        "human_r_arm_s", "human_chest_f_s", "human_l_arm_s",
        "human_r_hand_s", "human_l_hand_s",
        "human_r_leg_s", "human_l_leg_s", "human_head_f_s"
    };

    for (auto* p : parts_m)
        get_sprite(std::string(body_base) + p + ".png");
    for (auto* p : parts_f)
        get_sprite(std::string(body_base) + p + ".png");

    // Hair styles
    for (int i = 0; i < k_hair_count; ++i) {
        if (k_hair_styles[i].file[0] == '\0') continue;
        get_sprite(std::string(face_base) + k_hair_styles[i].file + "_s.png");
    }
    // Facial hair styles
    for (int i = 1; i < k_facial_count; ++i) {
        get_sprite(std::string(face_base) + k_facial_styles[i].file + "_s.png");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void CharacterCreator::process_event(const SDL_Event& e)
{
    if (!m_name_edit) return;

    if (e.type == SDL_EVENT_TEXT_INPUT) {
        // Append typed text (max 26 chars — same as TG SS13's real_name limit)
        size_t space = 26 - m_profile.name.size();
        if (space > 0) {
            std::string chunk = e.text.text;
            if (chunk.size() > space) chunk = chunk.substr(0, space);
            m_profile.name += chunk;
        }
    } else if (e.type == SDL_EVENT_KEY_DOWN) {
        switch (e.key.key) {
            case SDLK_BACKSPACE:
                if (!m_profile.name.empty())
                    m_profile.name.pop_back();
                break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_ESCAPE:
                m_name_edit = false;
                SDL_StopTextInput(m_window);
                break;
            default:
                break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void CharacterCreator::randomise()
{
    static bool seeded = false;
    if (!seeded) { std::srand(static_cast<unsigned>(std::time(nullptr))); seeded = true; }

    // Random name from a list of first + last names (similar to TG SS13)
    static const char* first_m[] = {
        "James","John","Robert","Michael","William","David","Richard","Joseph",
        "Thomas","Charles","Alex","Marcus","Nathan","Ethan","Samuel","Oliver"
    };
    static const char* first_f[] = {
        "Mary","Patricia","Jennifer","Linda","Barbara","Elizabeth","Susan","Jessica",
        "Sarah","Karen","Emma","Olivia","Aria","Clara","Luna","Sofia"
    };
    static const char* last_names[] = {
        "Smith","Johnson","Williams","Brown","Jones","Garcia","Miller","Davis",
        "Wilson","Anderson","Taylor","Thomas","Moore","Jackson","White","Harris",
        "Martin","Thompson","Lee","Robinson","Walker","Perez","Hall","Lewis"
    };

    m_profile.is_male = (std::rand() % 2 == 0);

    const char** firsts = m_profile.is_male ? first_m : first_f;
    int fn_count = m_profile.is_male ? 16 : 16;
    m_profile.name = std::string(firsts[std::rand() % fn_count]) + " "
                   + last_names[std::rand() % 24];

    m_profile.skin_idx     = std::rand() % k_skin_count;
    m_profile.skin_color   = k_skin_colors[m_profile.skin_idx];

    // Avoid index 0 (Bald) for randoms unless lucky
    m_profile.hair_idx     = (std::rand() % 8 == 0) ? 0 : (1 + std::rand() % (k_hair_count - 1));
    m_profile.hair_col_idx = std::rand() % 9;  // natural colours only (skip dyed)
    m_profile.hair_color   = k_hair_colors[m_profile.hair_col_idx];

    if (m_profile.is_male)
        m_profile.facial_idx = std::rand() % k_facial_count;
    else
        m_profile.facial_idx = 0;
    m_profile.facial_col_idx = m_profile.hair_col_idx;
    m_profile.facial_color   = k_hair_colors[m_profile.facial_col_idx];
    // Resolve file names from new indices
    m_profile.hair_file   = (m_profile.hair_idx > 0 && m_profile.hair_idx < k_hair_count)
                             ? k_hair_styles[m_profile.hair_idx].file : "";
    m_profile.facial_file = (m_profile.facial_idx > 0 && m_profile.facial_idx < k_facial_count)
                             ? k_facial_styles[m_profile.facial_idx].file : "";
}

// ─────────────────────────────────────────────────────────────────────────────
bool CharacterCreator::draw_button(glm::vec2 pos, float w, float h,
                                    const char* label,
                                    glm::vec2 cursor, bool lmb,
                                    glm::vec4 normal_col, glm::vec4 hover_col)
{
    bool hov = cursor.x >= pos.x && cursor.x <= pos.x + w
            && cursor.y >= pos.y && cursor.y <= pos.y + h;
    bool clicked = hov && lmb;

    m_ui.rect({pos.x + 3.f, pos.y + 3.f}, {w, h}, {0.f, 0.f, 0.f, 0.40f}, 6.f);
    m_ui.rect(pos, {w, h}, hov ? hover_col : normal_col, 6.f);
    m_ui.rect(pos, {w, 2.f}, {1.f, 1.f, 1.f, hov ? 0.18f : 0.07f});

    const float BT = 1.f;
    glm::vec4 bord = hov ? glm::vec4{0.40f, 0.65f, 1.00f, 0.70f} : CC_BORDER;
    m_ui.rect(pos,                         {w, BT},  bord);
    m_ui.rect(pos,                         {BT, h},  bord);
    m_ui.rect({pos.x, pos.y + h - BT},     {w, BT},  bord);
    m_ui.rect({pos.x + w - BT, pos.y},     {BT, h},  bord);

    float tw = static_cast<float>(std::strlen(label)) * 9.f;
    m_ui.text({pos.x + (w - tw) * 0.5f, pos.y + (h - 16.f) * 0.5f - 1.f},
              label, CC_BTN_TXT, 17.f);

    return clicked;
}

// ─────────────────────────────────────────────────────────────────────────────
// Returns -1 = clicked prev, +1 = clicked next, 0 = nothing
int CharacterCreator::draw_cycler(glm::vec2 pos, float total_w, float row_h,
                                   const char* value_label,
                                   glm::vec2 cursor, bool lmb)
{
    const float ARR = row_h;        // arrow button is square
    const float mid_w = total_w - ARR * 2.f;

    // Left arrow
    bool prev_clicked  = false;
    bool next_clicked  = false;

    // Left arrow button
    {
        glm::vec2 apos = pos;
        bool hov = cursor.x >= apos.x && cursor.x <= apos.x + ARR
                && cursor.y >= apos.y && cursor.y <= apos.y + ARR;
        prev_clicked = hov && lmb;
        m_ui.rect(apos, {ARR, ARR}, hov ? CC_BTN_HOV : CC_BTN, 4.f);
        const float BT = 1.f;
        m_ui.rect(apos,                              {ARR, BT}, CC_BORDER);
        m_ui.rect(apos,                              {BT, ARR}, CC_BORDER);
        m_ui.rect({apos.x, apos.y + ARR - BT},       {ARR, BT}, CC_BORDER);
        m_ui.rect({apos.x + ARR - BT, apos.y},        {BT, ARR}, CC_BORDER);
        m_ui.text({apos.x + (ARR - 6.f) * 0.5f, apos.y + (ARR - 16.f) * 0.5f - 1.f},
                  "<", CC_BTN_TXT, 15.f);
    }

    // Centre value label area
    {
        glm::vec2 mpos = {pos.x + ARR, pos.y};
        m_ui.rect(mpos, {mid_w, row_h}, {0.07f, 0.08f, 0.14f, 0.95f}, 0.f);
        float tw = static_cast<float>(std::strlen(value_label)) * 8.f;
        float tx = mpos.x + (mid_w - tw) * 0.5f;
        float ty = mpos.y + (row_h - 16.f) * 0.5f - 1.f;
        m_ui.text({tx, ty}, value_label, CC_VALUE, 15.f);
    }

    // Right arrow
    {
        glm::vec2 apos = {pos.x + ARR + mid_w, pos.y};
        bool hov = cursor.x >= apos.x && cursor.x <= apos.x + ARR
                && cursor.y >= apos.y && cursor.y <= apos.y + ARR;
        next_clicked = hov && lmb;
        m_ui.rect(apos, {ARR, ARR}, hov ? CC_BTN_HOV : CC_BTN, 4.f);
        const float BT = 1.f;
        m_ui.rect(apos,                              {ARR, BT}, CC_BORDER);
        m_ui.rect(apos,                              {BT, ARR}, CC_BORDER);
        m_ui.rect({apos.x, apos.y + ARR - BT},       {ARR, BT}, CC_BORDER);
        m_ui.rect({apos.x + ARR - BT, apos.y},        {BT, ARR}, CC_BORDER);
        m_ui.text({apos.x + (ARR - 6.f) * 0.5f, apos.y + (ARR - 16.f) * 0.5f - 1.f},
                  ">", CC_BTN_TXT, 15.f);
    }

    if (prev_clicked) return -1;
    if (next_clicked) return  1;
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Returns index of clicked swatch, or -1 if none
int CharacterCreator::draw_color_row(glm::vec2 pos,
                                      const glm::u8vec4* colors, int count,
                                      int selected_idx,
                                      glm::vec2 cursor, bool lmb)
{
    constexpr float SW  = 20.f;   // swatch size
    constexpr float GAP = 4.f;    // gap between swatches

    int clicked = -1;
    for (int i = 0; i < count; ++i) {
        glm::vec2 sp = {pos.x + i * (SW + GAP), pos.y};
        bool hov = cursor.x >= sp.x && cursor.x <= sp.x + SW
                && cursor.y >= sp.y && cursor.y <= sp.y + SW;
        if (hov && lmb) clicked = i;

        glm::vec4 fc = { colors[i].r / 255.f, colors[i].g / 255.f,
                         colors[i].b / 255.f, 1.f };

        // Selection ring
        if (i == selected_idx)
            m_ui.rect({sp.x - 2.f, sp.y - 2.f}, {SW + 4.f, SW + 4.f},
                      {0.50f, 0.75f, 1.00f, 0.85f}, 4.f);

        m_ui.rect(sp, {SW, SW}, fc, 3.f);

        // Hover overlay
        if (hov)
            m_ui.rect(sp, {SW, SW}, {1.f, 1.f, 1.f, 0.22f}, 3.f);
    }
    return clicked;
}

// ─────────────────────────────────────────────────────────────────────────────
void CharacterCreator::draw_name_field(glm::vec2 pos, float w, float h,
                                        glm::vec2 cursor, bool lmb)
{
    bool hov = cursor.x >= pos.x && cursor.x <= pos.x + w
            && cursor.y >= pos.y && cursor.y <= pos.y + h;

    // Click to focus/unfocus
    if (hov && lmb) {
        if (!m_name_edit) {
            m_name_edit = true;
            SDL_StartTextInput(m_window);
        }
    } else if (!hov && lmb && m_name_edit) {
        m_name_edit = false;
        SDL_StopTextInput(m_window);
    }

    glm::vec4 bg   = m_name_edit ? CC_INPUT_BGFOC : CC_INPUT_BG;
    glm::vec4 bord = m_name_edit ? CC_INPUT_BRDFOC : CC_INPUT_BRD;

    m_ui.rect(pos, {w, h}, bg, 4.f);
    const float BT = 1.5f;
    m_ui.rect(pos,                      {w, BT},  bord);
    m_ui.rect(pos,                      {BT, h},  bord);
    m_ui.rect({pos.x, pos.y + h - BT},  {w, BT},  bord);
    m_ui.rect({pos.x + w - BT, pos.y},  {BT, h},  bord);

    // Display text + blinking cursor
    std::string display = m_profile.name;
    if (m_name_edit) {
        bool cursor_on = (static_cast<int>(m_time * 2.0) & 1) == 0;
        if (cursor_on) display += "|";
    }
    float ty = pos.y + (h - 16.f) * 0.5f - 1.f;
    m_ui.text({pos.x + 8.f, ty}, display, CC_VALUE, 15.f);

    // Placeholder text when empty
    if (m_profile.name.empty() && !m_name_edit) {
        m_ui.text({pos.x + 8.f, ty}, "Enter name...",
                  {0.40f, 0.45f, 0.55f, 0.60f}, 15.f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void CharacterCreator::draw_preview(glm::vec2 pos, float scale)
{
    // SS13 sprites are 32×32 px.  Render at 4× for a crisp 128×128 preview.
    const float SPR = 32.f * scale;

    bool male = m_profile.is_male;
    const char* body_base = "legacysets/extracted/mob/human/bodyparts_greyscale/";
    const char* face_base = "legacysets/extracted/mob/human/human_face/";

    // Body-part tint = skin colour, normalised to 0..1
    glm::vec4 skin = {
        m_profile.skin_color.r / 255.f,
        m_profile.skin_color.g / 255.f,
        m_profile.skin_color.b / 255.f,
        1.f
    };
    glm::vec4 hair_tint = {
        m_profile.hair_color.r / 255.f,
        m_profile.hair_color.g / 255.f,
        m_profile.hair_color.b / 255.f,
        1.f
    };
    glm::vec4 facial_tint = {
        m_profile.facial_color.r / 255.f,
        m_profile.facial_color.g / 255.f,
        m_profile.facial_color.b / 255.f,
        1.f
    };

    // Helper: draw one body-part sprite tinted with skin colour
    auto draw_part = [&](const char* part_name) {
        std::string path = std::string(body_base) + part_name + "_s.png";
        auto it = m_sprites.find(path);
        if (it != m_sprites.end() && it->second)
            m_ui.image_tinted(pos, {SPR, SPR}, it->second, skin);
    };

    // Layer order (back → front): r_arm, chest, l_arm, legs, hands, head
    // This matches the TG SS13 appearance layer stack order.
    draw_part("human_r_arm");
    draw_part(male ? "human_chest_m" : "human_chest_f");
    draw_part("human_l_arm");
    draw_part("human_r_leg");
    draw_part("human_l_leg");
    draw_part("human_r_hand");
    draw_part("human_l_hand");
    draw_part(male ? "human_head_m" : "human_head_f");

    // Facial hair (male only, on top of head)
    if (male && m_profile.facial_idx > 0 &&
        m_profile.facial_idx < k_facial_count)
    {
        const char* ffile = k_facial_styles[m_profile.facial_idx].file;
        if (ffile && ffile[0]) {
            std::string fpath = std::string(face_base) + ffile + "_s.png";
            auto it = m_sprites.find(fpath);
            if (it != m_sprites.end() && it->second)
                m_ui.image_tinted(pos, {SPR, SPR}, it->second, facial_tint);
        }
    }

    // Hair
    if (m_profile.hair_idx > 0 && m_profile.hair_idx < k_hair_count) {
        const char* hfile = k_hair_styles[m_profile.hair_idx].file;
        if (hfile && hfile[0]) {
            std::string hpath = std::string(face_base) + hfile + "_s.png";
            auto it = m_sprites.find(hpath);
            if (it != m_sprites.end() && it->second)
                m_ui.image_tinted(pos, {SPR, SPR}, it->second, hair_tint);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
CharacterCreator::Result CharacterCreator::draw(glm::vec2 cursor, bool lmb)
{
    Result result;

    const float fw = static_cast<float>(m_ui.fb_width());
    const float fh = static_cast<float>(m_ui.fb_height());

    // ── Full-screen background ──────────────────────────────────────────────
    m_ui.rect({0.f, 0.f},       {fw, fh},          CC_BG);
    m_ui.rect({0.f, fh * 0.5f}, {fw, fh * 0.5f},   CC_BG_BOT);

    // Faint scanlines
    for (float y = 0.f; y < fh; y += 4.f)
        m_ui.rect({0.f, y}, {fw, 1.f}, {0.f, 0.f, 0.f, 0.06f});

    // ── Main panel ──────────────────────────────────────────────────────────
    constexpr float PW = 840.f;
    constexpr float PH = 560.f;
    float px = (fw - PW) * 0.5f;
    float py = (fh - PH) * 0.5f;

    draw_panel_box(m_ui, {px, py}, {PW, PH}, CC_PANEL, CC_BORDER, 10.f);

    // Title row
    {
        const char* title = "CHARACTER SETUP";
        float tw = static_cast<float>(std::strlen(title)) * 12.5f;
        m_ui.text({px + (PW - tw) * 0.5f + 1.f, py + 18.f},
                  title, {0.15f, 0.40f, 0.80f, 0.55f}, 22.f);
        m_ui.text({px + (PW - tw) * 0.5f, py + 17.f},
                  title, CC_TITLE, 22.f);
    }

    // Separator under title
    m_ui.rect({px + 20.f, py + 50.f}, {PW - 40.f, 1.5f}, CC_SEP);

    // ── Left column: preview ────────────────────────────────────────────────
    constexpr float LEFT_W  = 240.f;
    float lx = px + 20.f;
    float ly = py + 58.f;
    float left_content_h = PH - 58.f - 70.f;  // 70px reserved for bottom buttons

    draw_panel_box(m_ui, {lx, ly}, {LEFT_W, left_content_h}, CC_PANEL_L, CC_BORDER, 6.f);

    // Preview box label
    {
        const char* lbl = "PREVIEW";
        float tw = static_cast<float>(std::strlen(lbl)) * 7.f;
        m_ui.text({lx + (LEFT_W - tw) * 0.5f, ly + 8.f}, lbl, CC_LABEL, 13.f);
    }
    m_ui.rect({lx + 10.f, ly + 28.f}, {LEFT_W - 20.f, 1.f}, CC_SEP);

    // Dark checkerboard-style preview background (2×2 checkers)
    {
        float bx = lx + (LEFT_W - 128.f) * 0.5f;
        float by = ly + 40.f;
        m_ui.rect({bx, by}, {128.f, 128.f}, {0.05f, 0.05f, 0.08f, 1.0f}, 4.f);
        // Light grid lines every 32px for SS13 tile aesthetic
        for (int i = 1; i < 4; ++i) {
            m_ui.rect({bx + i * 32.f, by}, {1.f, 128.f}, {1.f, 1.f, 1.f, 0.06f});
            m_ui.rect({bx, by + i * 32.f}, {128.f, 1.f}, {1.f, 1.f, 1.f, 0.06f});
        }
        // Draw the composited sprite layers at 4× scale
        draw_preview({bx, by}, 4.f);
    }

    // Show character name below preview
    {
        float name_y = ly + 180.f;
        float tw = static_cast<float>(m_profile.name.size()) * 7.5f;
        m_ui.text({lx + (LEFT_W - tw) * 0.5f, name_y},
                  m_profile.name, CC_VALUE, 14.f);
    }

    // Show species / gender info
    {
        const char* info = m_profile.is_male ? "Human (Male)" : "Human (Female)";
        float tw = static_cast<float>(std::strlen(info)) * 6.5f;
        m_ui.text({lx + (LEFT_W - tw) * 0.5f, ly + 198.f},
                  info, CC_LABEL, 12.f);
    }

    // Divider between left/right columns
    m_ui.rect({lx + LEFT_W + 10.f, py + 58.f}, {1.5f, PH - 128.f}, CC_SEP);

    // ── Right column: options ───────────────────────────────────────────────
    float rx    = lx + LEFT_W + 26.f;
    float ry    = py + 58.f;
    float right_w = PW - LEFT_W - 66.f;  // remaining width for options

    // ── NAME ────────────────────────────────────────────────────────────────
    {
        float field_y = ry + 6.f;
        m_ui.text({rx, field_y}, "Name", CC_LABEL, 13.f);
        draw_name_field({rx, field_y + 18.f}, right_w, 26.f, cursor, lmb);
    }

    // ── GENDER ──────────────────────────────────────────────────────────────
    {
        float gy = ry + 60.f;
        m_ui.text({rx, gy}, "Gender", CC_LABEL, 13.f);

        float btn_w = (right_w - 6.f) * 0.5f;
        bool want_male  = draw_button({rx, gy + 18.f}, btn_w, 26.f,
                                       "Male",   cursor, lmb,
                                       m_profile.is_male  ? CC_BTN_ACCEPT : CC_BTN,
                                       m_profile.is_male  ? CC_BTN_ACCEPTH : CC_BTN_HOV);
        bool want_female = draw_button({rx + btn_w + 6.f, gy + 18.f}, btn_w, 26.f,
                                        "Female", cursor, lmb,
                                        !m_profile.is_male ? CC_BTN_ACCEPT : CC_BTN,
                                        !m_profile.is_male ? CC_BTN_ACCEPTH : CC_BTN_HOV);
        if (want_male)   m_profile.is_male = true;
        if (want_female) m_profile.is_male = false;
    }

    // ── SKIN TONE ────────────────────────────────────────────────────────────
    {
        float sy = ry + 114.f;
        m_ui.text({rx, sy}, "Skin Tone", CC_LABEL, 13.f);
        int clicked = draw_color_row({rx, sy + 18.f}, k_skin_colors, k_skin_count,
                                      m_profile.skin_idx, cursor, lmb);
        if (clicked >= 0) {
            m_profile.skin_idx   = clicked;
            m_profile.skin_color = k_skin_colors[clicked];
        }
    }

    // ── HAIR STYLE ───────────────────────────────────────────────────────────
    {
        float hy = ry + 158.f;
        m_ui.text({rx, hy}, "Hair Style", CC_LABEL, 13.f);
        const char* label = k_hair_styles[m_profile.hair_idx].label;
        int delta = draw_cycler({rx, hy + 18.f}, right_w, 26.f, label, cursor, lmb);
        if (delta != 0) {
            m_profile.hair_idx = (m_profile.hair_idx + delta + k_hair_count) % k_hair_count;
            m_profile.hair_file = (m_profile.hair_idx > 0)
                                  ? k_hair_styles[m_profile.hair_idx].file : "";
        }
    }

    // ── HAIR COLOUR ──────────────────────────────────────────────────────────
    {
        float hcy = ry + 202.f;
        m_ui.text({rx, hcy}, "Hair Color", CC_LABEL, 13.f);
        int clicked = draw_color_row({rx, hcy + 18.f}, k_hair_colors, k_hair_color_count,
                                      m_profile.hair_col_idx, cursor, lmb);
        if (clicked >= 0) {
            m_profile.hair_col_idx = clicked;
            m_profile.hair_color   = k_hair_colors[clicked];
        }
    }

    // ── FACIAL HAIR (male only) ───────────────────────────────────────────────
    float facial_bottom_y = ry + 246.f;
    if (m_profile.is_male) {
        m_ui.text({rx, facial_bottom_y}, "Facial Hair", CC_LABEL, 13.f);
        const char* label = k_facial_styles[m_profile.facial_idx].label;
        int delta = draw_cycler({rx, facial_bottom_y + 18.f}, right_w, 26.f,
                                 label, cursor, lmb);
        if (delta != 0) {
            m_profile.facial_idx = (m_profile.facial_idx + delta + k_facial_count)
                                    % k_facial_count;
            m_profile.facial_file = (m_profile.facial_idx > 0)
                                    ? k_facial_styles[m_profile.facial_idx].file : "";
        }
        facial_bottom_y += 44.f;

        // Facial hair color (only when has facial hair)
        if (m_profile.facial_idx > 0) {
            m_ui.text({rx, facial_bottom_y}, "Facial Hair Color", CC_LABEL, 13.f);
            int clicked = draw_color_row({rx, facial_bottom_y + 18.f},
                                          k_hair_colors, k_hair_color_count,
                                          m_profile.facial_col_idx, cursor, lmb);
            if (clicked >= 0) {
                m_profile.facial_col_idx = clicked;
                m_profile.facial_color   = k_hair_colors[clicked];
            }
        }
    }

    // ── Bottom buttons ────────────────────────────────────────────────────────
    constexpr float BTN_H  = 42.f;
    constexpr float BTN_GAP = 12.f;
    constexpr float BTN_W  = 160.f;
    float btn_y = py + PH - BTN_H - 18.f;
    float btn_total_w = BTN_W * 3.f + BTN_GAP * 2.f;
    float btn_x = px + (PW - btn_total_w) * 0.5f;

    if (draw_button({btn_x, btn_y}, BTN_W, BTN_H,
                    "Back", cursor, lmb, CC_BTN_BACK, CC_BTN_BACKH))
    {
        if (m_name_edit) { m_name_edit = false; SDL_StopTextInput(m_window); }
        result.back = true;
    }

    if (draw_button({btn_x + BTN_W + BTN_GAP, btn_y}, BTN_W, BTN_H,
                    "Randomize", cursor, lmb, CC_BTN_RAND, CC_BTN_RANDH))
    {
        randomise();
    }

    if (draw_button({btn_x + (BTN_W + BTN_GAP) * 2.f, btn_y}, BTN_W, BTN_H,
                    "Accept", cursor, lmb, CC_BTN_ACCEPT, CC_BTN_ACCEPTH))
    {
        if (m_name_edit) { m_name_edit = false; SDL_StopTextInput(m_window); }
        if (m_profile.name.empty()) m_profile.name = "Unknown";
        result.accepted = true;
    }

    // Separator above buttons
    m_ui.rect({px + 20.f, btn_y - 14.f}, {PW - 40.f, 1.5f}, CC_SEP);

    return result;
}
