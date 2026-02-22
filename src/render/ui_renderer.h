#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <SDL3/SDL.h>

// Immediate-mode 2-D renderer layered over the 3-D world pass.
// Usage per frame:
//   begin()          ← reset draw list
//   rect/text/image  ← buffer draw calls
//   end(cmd, sc, w, h) ← upload + render pass on swapchain texture
class UIRenderer {
public:
    explicit UIRenderer(SDL_GPUDevice* gpu);
    ~UIRenderer();

    bool init(SDL_Window* window, int fb_width, int fb_height);
    void shutdown();
    void on_resize(int w, int h);

    void begin();
    // Must be called AFTER Renderer::end_world_pass() so the swapchain texture
    // is free to be used in a new render pass.
    void end(SDL_GPUCommandBuffer* cmd_buf,
             SDL_GPUTexture*       swapchain_tex,
             int                   fb_w,
             int                   fb_h);

    // ── Draw calls (buffered between begin/end) ────────────────────────────
    void rect (glm::vec2 pos, glm::vec2 size, glm::vec4 color,
               float corner_radius = 0.f);
    void text (glm::vec2 pos, const std::string& str,
               glm::vec4 color, float font_size = 16.f);
    void icon (glm::vec2 pos, glm::vec2 size,
               uint16_t atlas_index, float alpha = 1.f);
    void line (glm::vec2 a, glm::vec2 b,
               glm::vec4 color, float thickness = 1.f);

    // Load a PNG → SDL_GPUTexture (owned by UIRenderer, freed in shutdown).
    SDL_GPUTexture* load_texture(const char* path);

    // Draw a loaded texture. flip_x mirrors horizontally (e.g. left hand).
    void image(glm::vec2 pos, glm::vec2 size, SDL_GPUTexture* tex,
               float alpha = 1.f, bool flip_x = false);

    bool hit_test(glm::vec2 screen_pos) const;

    int fb_width()  const { return m_fb_w; }
    int fb_height() const { return m_fb_h; }

private:
    // ── GPU objects ────────────────────────────────────────────────────────
    SDL_GPUDevice*           m_gpu      = nullptr;
    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;
    SDL_GPUSampler*          m_sampler  = nullptr;
    SDL_GPUTexture*          m_white_tex = nullptr;  // 1×1 opaque white

    // Dynamic geometry (pre-allocated, remapped each frame)
    SDL_GPUBuffer*         m_vbuf      = nullptr;
    SDL_GPUBuffer*         m_ibuf      = nullptr;
    SDL_GPUTransferBuffer* m_vert_tbuf = nullptr;
    SDL_GPUTransferBuffer* m_idx_tbuf  = nullptr;

    static constexpr uint32_t k_max_verts   = 16384;  // 4096 quads
    static constexpr uint32_t k_max_indices = 24576;  // 4096 × 6

    // ── Per-frame CPU staging ──────────────────────────────────────────────
    struct UIVertex { float x, y, u, v, r, g, b, a; };
    UIVertex* m_vert_ptr   = nullptr;
    uint32_t* m_idx_ptr    = nullptr;
    uint32_t  m_vert_count = 0;
    uint32_t  m_idx_count  = 0;

    // ── Batch list (grouped by texture) ───────────────────────────────────
    struct DrawBatch { SDL_GPUTexture* tex; uint32_t first, count; };
    std::vector<DrawBatch> m_batches;

    // ── Loaded textures (freed on shutdown) ────────────────────────────────
    std::vector<SDL_GPUTexture*> m_owned_textures;

    int m_fb_w = 0, m_fb_h = 0;

    // ── Helpers ────────────────────────────────────────────────────────────
    bool create_pipeline(SDL_Window* window);
    bool create_white_texture();
    // Push one axis-aligned quad. Returns false if buffer is full.
    bool push_quad(glm::vec2 pos, glm::vec2 size,
                   float u0, float v0, float u1, float v1,
                   glm::vec4 color, SDL_GPUTexture* tex);
};
