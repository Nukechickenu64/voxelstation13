#include "render/ui_renderer.h"

UIRenderer::UIRenderer(SDL_GPUDevice* gpu)
    : m_gpu(gpu)
{}

UIRenderer::~UIRenderer() { shutdown(); }

bool UIRenderer::init(SDL_Window* /*window*/, int fb_width, int fb_height)
{
    m_fb_w = fb_width;
    m_fb_h = fb_height;
    // TODO: create SDL_GPUGraphicsPipeline for 2-D rect/text rendering.
    //       Load or embed a minimal font atlas.
    return true;
}

void UIRenderer::shutdown()
{
    // TODO: release GPU resources.
}

void UIRenderer::on_resize(int w, int h)
{
    m_fb_w = w;
    m_fb_h = h;
}

void UIRenderer::begin()
{
    // Reset per-frame draw list.
}

void UIRenderer::end(SDL_GPUCommandBuffer* /*cmd_buf*/)
{
    // Upload vertex buffer and issue draw calls for all buffered primitives.
    // TODO: bind 2-D pipeline, upload vertex data, draw.
}

void UIRenderer::rect(glm::vec2 pos, glm::vec2 size, glm::vec4 color,
                       float /*corner_radius*/)
{
    // Push a quad into the draw list.
    (void)pos; (void)size; (void)color;
}

void UIRenderer::text(glm::vec2 pos, const std::string& str,
                       glm::vec4 color, float font_size)
{
    // Push glyph quads from the embedded font atlas.
    (void)pos; (void)str; (void)color; (void)font_size;
}

void UIRenderer::icon(glm::vec2 pos, glm::vec2 size,
                       uint16_t atlas_index, float alpha)
{
    (void)pos; (void)size; (void)atlas_index; (void)alpha;
}

void UIRenderer::line(glm::vec2 a, glm::vec2 b, glm::vec4 color,
                       float thickness)
{
    (void)a; (void)b; (void)color; (void)thickness;
}

bool UIRenderer::hit_test(glm::vec2 /*screen_pos*/) const
{
    // TODO: check last frame's draw rects for containment.
    return false;
}
