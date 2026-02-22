#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <functional>
#include <SDL3/SDL.h>

// Immediate-mode 2-D draw command types
enum class DrawCmd { Rect, Text, Icon, Line };

struct DrawRect  { glm::vec2 pos, size; glm::vec4 color; float radius = 0; };
struct DrawText  { glm::vec2 pos; std::string text; glm::vec4 color; float size; };
struct DrawIcon  { glm::vec2 pos, size; uint16_t atlas_index; float alpha = 1.f; };

// Immediate-mode 2-D renderer layered over the 3-D world pass.
// All draw calls happen during a single ui_draw() each frame; no retained widgets.
class UIRenderer {
public:
    explicit UIRenderer(SDL_GPUDevice* gpu);
    ~UIRenderer();

    bool init(SDL_Window* window, int fb_width, int fb_height);
    void shutdown();
    void on_resize(int w, int h);

    // Begin/end a UI frame
    void begin();
    void end(SDL_GPUCommandBuffer* cmd_buf);

    // Primitive draw calls (buffer during begin/end)
    void rect(glm::vec2 pos, glm::vec2 size, glm::vec4 color, float corner_radius = 0.f);
    void text(glm::vec2 pos, const std::string& str, glm::vec4 color, float font_size = 16.f);
    void icon(glm::vec2 pos, glm::vec2 size, uint16_t atlas_index, float alpha = 1.f);
    void line(glm::vec2 a, glm::vec2 b, glm::vec4 color, float thickness = 1.f);

    // Hit-test: was the given screen point covered by any draw call last frame?
    bool hit_test(glm::vec2 screen_pos) const;

    int fb_width()  const { return m_fb_w; }
    int fb_height() const { return m_fb_h; }

private:
    SDL_GPUDevice* m_gpu = nullptr;
    int m_fb_w = 0, m_fb_h = 0;
};
