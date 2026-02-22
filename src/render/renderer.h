#pragma once
#include "core/world.h"
#include "data/voxel_registry.h"
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
    // Call BEFORE begin_frame so geometry is uploaded with no lag.
    void queue_highlight(const RayHit& hit);
    void draw_face_highlight(const RayHit& hit);
    void draw_viewmodel(uint16_t item_type_id);
    void end_frame();

    // Load tile textures from disk and upload a GPU 2D-array texture.
    // Must be called after init(), before the first draw_world().
    bool load_tile_textures(const VoxelRegistry& reg, const char* texture_dir);

    // CPU mesh management
    ChunkMesh& get_or_create_mesh(glm::ivec3 chunk_pos);
    void       upload_mesh(ChunkMesh& mesh);   // copies to GPU
    void       free_mesh(glm::ivec3 chunk_pos);

    SDL_Window*          window()  const { return m_window; }
    SDL_GPUDevice*        gpu()     const { return m_gpu; }
    SDL_GPUCommandBuffer* cmd_buf() const { return m_cmd_buf; }
    int width()  const { return m_width; }
    int height() const { return m_height; }

private:
    // ── Window / device ──────────────────────────────────────────────────────
    SDL_Window*    m_window = nullptr;
    SDL_GPUDevice* m_gpu    = nullptr;
    int m_width  = 0;
    int m_height = 0;

    // ── Pipeline ─────────────────────────────────────────────────────────────
    SDL_GPUGraphicsPipeline* m_world_pipeline     = nullptr;
    SDL_GPUShader*           m_vert_shader         = nullptr;
    SDL_GPUShader*           m_frag_shader         = nullptr;
    SDL_GPUTexture*          m_depth_tex           = nullptr;
    SDL_GPUTextureFormat     m_depth_fmt           = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTexture*          m_tile_array          = nullptr;  // 2D array of tile textures
    SDL_GPUSampler*          m_tile_sampler        = nullptr;

    // ── Highlight pipeline ────────────────────────────────────────────────────
    SDL_GPUGraphicsPipeline* m_highlight_pipeline  = nullptr;
    SDL_GPUBuffer*           m_highlight_vbuf      = nullptr;  // 4 vertices * pos(3f) = 48 B
    SDL_GPUBuffer*           m_highlight_ibuf      = nullptr;  // 6 indices  * u32    = 24 B
    // CPU-side highlight geometry queued for upload each frame
    float                    m_hl_verts[4 * 3]     = {};       // 4 x vec3
    bool                     m_hl_pending          = false;   // upload needed?
    bool                     m_hl_valid            = false;   // draw this frame?

    // ── Per-frame transient ───────────────────────────────────────────────────
    SDL_GPUCommandBuffer* m_cmd_buf     = nullptr;
    SDL_GPURenderPass*    m_render_pass = nullptr;
    glm::mat4             m_current_mvp{};  // stored by draw_world, reused by highlight

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
    bool create_highlight_pipeline();
    void upload_highlight_geometry();   // copy pass before render pass
    void release_gpu_mesh(GPUMesh& gm);
    static bool aabb_in_frustum(const glm::mat4& mvp, glm::vec3 mn, glm::vec3 mx);
};
