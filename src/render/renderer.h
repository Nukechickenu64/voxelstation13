#pragma once
#include "core/world.h"
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <unordered_map>

// GPU mesh for one rendered chunk (CPU side).
// Vertex layout: pos.xyz (3f) + normal.xyz (3f) = 24 bytes per vertex.
struct ChunkMesh {
    glm::ivec3            chunk_pos{};
    std::vector<float>    vertices;   // 6 floats / vertex
    std::vector<uint32_t> indices;
    bool transparent = false;
    bool dirty       = true;
};

// Holds all SDL3 GPU state and drives the render loop.
class Renderer {
public:
    Renderer();
    ~Renderer();

    bool init(const char* title, int width, int height);
    void shutdown();

    // Called each frame.  alpha = interpolation between simulation ticks.
    void begin_frame(double alpha);
    void draw_world(const World& world, glm::vec3 cam_pos, float yaw, float pitch);
    void draw_face_highlight(const RayHit& hit);
    void draw_viewmodel(uint16_t item_type_id);
    void end_frame();

    // CPU mesh management
    ChunkMesh& get_or_create_mesh(glm::ivec3 chunk_pos);
    void       upload_mesh(ChunkMesh& mesh);   // copies to GPU
    void       free_mesh(glm::ivec3 chunk_pos);

    SDL_Window*    window() const { return m_window; }
    SDL_GPUDevice* gpu()    const { return m_gpu; }
    int width()  const { return m_width; }
    int height() const { return m_height; }

private:
    // ── Window / device ──────────────────────────────────────────────────────
    SDL_Window*    m_window = nullptr;
    SDL_GPUDevice* m_gpu    = nullptr;
    int m_width  = 0;
    int m_height = 0;

    // ── Pipeline ─────────────────────────────────────────────────────────────
    SDL_GPUGraphicsPipeline* m_world_pipeline = nullptr;
    SDL_GPUShader*           m_vert_shader    = nullptr;
    SDL_GPUShader*           m_frag_shader    = nullptr;
    SDL_GPUTexture*          m_depth_tex      = nullptr;
    SDL_GPUTextureFormat     m_depth_fmt      = SDL_GPU_TEXTUREFORMAT_INVALID;

    // ── Per-frame transient ───────────────────────────────────────────────────
    SDL_GPUCommandBuffer* m_cmd_buf     = nullptr;
    SDL_GPURenderPass*    m_render_pass = nullptr;

    // ── CPU mesh map ─────────────────────────────────────────────────────────
    std::unordered_map<glm::ivec3, ChunkMesh> m_meshes;

    // ── GPU mesh map ─────────────────────────────────────────────────────────
    struct GPUMesh {
        SDL_GPUBuffer* vbuf = nullptr;
        SDL_GPUBuffer* ibuf = nullptr;
        uint32_t       num_indices = 0;
    };
    std::unordered_map<glm::ivec3, GPUMesh> m_gpu_meshes;

    // ── Helpers ───────────────────────────────────────────────────────────────
    bool create_depth_texture();
    bool create_pipeline();
    void release_gpu_mesh(GPUMesh& gm);
};
