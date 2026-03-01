#pragma once
#include "render/ui_renderer.h"
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <SDL3/SDL.h>

// ─────────────────────────────────────────────────────────────────────────────
//  CharacterProfile — persistent character appearance (TG SS13 prefs analogue)
// ─────────────────────────────────────────────────────────────────────────────
struct CharacterProfile {
    std::string name             = "John Smith";
    bool        is_male          = true;
    glm::u8vec4 skin_color       = {205, 175, 149, 255}; // medium-light skin
    int         hair_idx         = 1;                    // index into k_hair_styles
    std::string hair_file        = "hair_messy";         // resolved sprite stem ("" = bald)
    glm::u8vec4 hair_color       = {89,  60,  30, 255};  // brown
    int         facial_idx       = 0;                    // 0 = no facial hair
    std::string facial_file      = "";                   // resolved sprite stem ("" = none)
    glm::u8vec4 facial_color     = {89,  60,  30, 255};  // brown
    int         skin_idx         = 2;                    // index into k_skin_colors
    int         hair_col_idx     = 1;                    // index into k_hair_colors
    int         facial_col_idx   = 1;
};

// ─────────────────────────────────────────────────────────────────────────────
//  CharacterCreator — TG SS13-style character setup screen.
//
//  Usage per-frame:
//    while (in_char_create) {
//      SDL_Event e; while (SDL_PollEvent(&e)) creator.process_event(e);
//      ui_renderer.begin();
//      auto r = creator.draw(cursor, lmb);
//      ui_renderer.end(...);
//      if (r.accepted) { in_char_create = false; profile = creator.profile(); }
//      if (r.back)     { in_char_create = false; }
//    }
// ─────────────────────────────────────────────────────────────────────────────
class CharacterCreator {
public:
    explicit CharacterCreator(UIRenderer& ui, SDL_Window* window,
                              const CharacterProfile& initial = {});

    struct Result {
        bool accepted = false;
        bool back     = false;
    };

    // Main per-frame call. Returns non-trivial Result on button press.
    Result draw(glm::vec2 cursor, bool lmb);

    // Feed SDL events for keyboard / text input.
    void process_event(const SDL_Event& e);

    // Advance animation timer.
    void tick(double dt) { m_time += dt; }

    const CharacterProfile& profile() const { return m_profile; }

    // Randomise all appearance fields.
    void randomise();

private:
    // ── Draw helpers ──────────────────────────────────────────────────────
    bool draw_button(glm::vec2 pos, float w, float h,
                     const char* label, glm::vec2 cursor, bool lmb,
                     glm::vec4 normal_col, glm::vec4 hover_col);

    // Arrow cycler:  < [label] >  — returns -1/0/+1 for prev/none/next
    int draw_cycler(glm::vec2 pos, float total_w, float row_h,
                    const char* value_label,
                    glm::vec2 cursor, bool lmb);

    // Row of colour swatches; returns index of clicked swatch (-1 = none)
    int draw_color_row(glm::vec2 pos, const glm::u8vec4* colors, int count,
                       int selected_idx, glm::vec2 cursor, bool lmb);

    // Name text-input field
    void draw_name_field(glm::vec2 pos, float w, float h,
                         glm::vec2 cursor, bool lmb);

    // Character sprite preview (all body-part layers composited)
    void draw_preview(glm::vec2 pos, float scale);

    // ── Sprite helpers ────────────────────────────────────────────────────
    SDL_GPUTexture* get_sprite(const std::string& path);
    void            preload_sprites();

    // ── State ─────────────────────────────────────────────────────────────
    UIRenderer&     m_ui;
    SDL_Window*     m_window;
    CharacterProfile m_profile;
    double          m_time       = 0.0;
    bool            m_name_edit  = false;  // name field focused?

    // Cached sprite textures (nullptr = not found / failed to load)
    std::unordered_map<std::string, SDL_GPUTexture*> m_sprites;
};
