#pragma once
#include "core/world.h"
#include "core/entity_manager.h"
#include "data/voxel_registry.h"
#include "inventory/item_registry.h"
#include "simulation/world_items.h"
#include "simulation/mob_system.h"
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

    // Collect world-item quads into CPU buffer.  Call before begin_frame().
    // Flat-resting items are laid along their face; floating items billboard
    // toward the camera.  hovered_item gets a bright outline tint.
    void queue_world_items(EntityManager& entities, EntityID hovered_item,
                           glm::vec3 cam_pos, float yaw, float pitch);

    // Draw the queued world-item quads (call inside render pass).
    void draw_world_items();

    void draw_viewmodel(uint16_t item_type_id);
    // End the world render pass without submitting (call before ui_renderer.end()).
    void end_world_pass();
    // Submit the command buffer (call after ui_renderer.end()).
    void end_frame();

    // Expose the view-projection matrix (valid after draw_world()).
    const glm::mat4& view_proj() const { return m_current_mvp; }

    // Load tile textures from disk and upload a GPU 2D-array texture.
    // Must be called after init(), before the first draw_world().
    bool load_tile_textures(const VoxelRegistry& reg, const char* texture_dir);

    // Load item textures from disk and upload a GPU 2D-array texture.
    // Must be called after init(), before the first draw_world_items().
    bool load_item_textures(const ItemRegistry& reg, const char* texture_dir);

    // Load mob sprite textures from textures/mobs/<species>/<variant>/.
    // Expects files whose names contain: front, back, left, right.
    // Must be called after init(), before the first queue_mobs().
    bool load_mob_textures(const char* texture_dir);

    // Queue Doom-style billboard sprites for all MobComponent entities.
    // Selects one of 4 rotation sprites based on camera-to-mob angle.
    // Call BEFORE begin_frame().
    void queue_mobs(EntityManager& entities, glm::vec3 cam_pos, float cam_yaw);

    // Draw the queued mob sprites (call inside the render pass, after draw_world()).
    void draw_mobs();

    // Queue the animated Earth / space background for the current frame.
    // Must be called BEFORE begin_frame() (like queue_world_items).
    void queue_earth_background(glm::vec3 cam_pos, float yaw, float pitch);

    // Draw the space background (call FIRST inside the render pass, before draw_world).
    void draw_space_background();

    // CPU mesh management
    ChunkMesh& get_or_create_mesh(glm::ivec3 chunk_pos);
    void       upload_mesh(ChunkMesh& mesh);   // copies to GPU
    void       free_mesh(glm::ivec3 chunk_pos);

    SDL_Window*          window()       const { return m_window; }
    SDL_GPUDevice*        gpu()          const { return m_gpu; }
    SDL_GPUCommandBuffer* cmd_buf()      const { return m_cmd_buf; }
    SDL_GPUTexture*       swapchain_tex() const { return m_swapchain_tex; }
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

    // ── Item pipeline (world-space flat/billboard quads) ──────────────────────
    // Vertex layout: pos(3f) + color(4f) + uv(2f) + texIdx(1f) = 40 bytes per vertex
    SDL_GPUGraphicsPipeline* m_item_pipeline   = nullptr;
    SDL_GPUBuffer*           m_item_vbuf       = nullptr;
    SDL_GPUBuffer*           m_item_ibuf       = nullptr;
    SDL_GPUTexture*          m_item_tex_array  = nullptr;
    SDL_GPUSampler*          m_item_sampler    = nullptr;
    std::unordered_map<std::string, uint32_t> m_item_tex_idx;
    static constexpr uint32_t k_max_item_quads = 256;
    // CPU staging for item quads, populated by queue_world_items()
    struct ItemVert { float x, y, z, r, g, b, a, u, v, tex_idx; };
    std::vector<ItemVert>    m_item_verts;
    std::vector<uint32_t>    m_item_indices;
    bool                     m_item_pending = false;

    // ── Mob sprite pipeline (Doom-style 4-rotation vertical billboards) ────────
    // Reuses the item pipeline (same vertex format).
    // Index layout in MobSpriteSet: 0=front, 1=back, 2=left, 3=right
    struct MobSpriteSet {
        uint32_t layer[4] = {0, 0, 0, 0};  // index into m_mob_tex_array layers
    };
    SDL_GPUBuffer*           m_mob_vbuf          = nullptr;
    SDL_GPUBuffer*           m_mob_ibuf          = nullptr;
    SDL_GPUTexture*          m_mob_tex_array     = nullptr;
    SDL_GPUSampler*          m_mob_sampler       = nullptr;
    uint32_t                 m_mob_tex_layers    = 0;       // total layers uploaded
    std::unordered_map<std::string, MobSpriteSet> m_mob_sprites; // key = "species/variant"
    static constexpr uint32_t k_max_mob_quads    = 64;
    std::vector<ItemVert>    m_mob_verts;
    std::vector<uint32_t>    m_mob_indices;
    bool                     m_mob_pending       = false;

    // ── Per-frame transient ───────────────────────────────────────────────────
    SDL_GPUCommandBuffer* m_cmd_buf       = nullptr;
    SDL_GPURenderPass*    m_render_pass   = nullptr;
    SDL_GPUTexture*       m_swapchain_tex = nullptr;  // valid between begin_frame/end_frame
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
    bool create_item_pipeline();
    bool create_mob_buffers();
    bool create_sky_pipeline();          // background Earth-orbit pass
    bool load_earth_gif(const char* path);
    void upload_highlight_geometry();   // copy pass before render pass
    void upload_item_geometry();        // copy pass before render pass
    void upload_mob_geometry();         // copy pass before render pass
    void upload_sky_geometry();         // copy pass before render pass
    void release_gpu_mesh(GPUMesh& gm);
    static bool aabb_in_frustum(const glm::mat4& mvp, glm::vec3 mn, glm::vec3 mx);

    // ── Sky / space background (Earth orbit) ─────────────────────────────────
    SDL_GPUGraphicsPipeline* m_sky_pipeline    = nullptr;
    SDL_GPUBuffer*           m_sky_vbuf        = nullptr;  // 4 verts × ItemVert
    SDL_GPUBuffer*           m_sky_ibuf        = nullptr;  // 6 uint32 indices
    SDL_GPUTexture*          m_earth_tex_array = nullptr;  // GIF frames as 2D-array layers
    SDL_GPUSampler*          m_earth_sampler   = nullptr;
    uint32_t                 m_earth_num_frames = 0;
    uint32_t                 m_earth_frame      = 0;       // current animation frame index
    std::vector<int>         m_earth_delays;               // ms per frame
    uint32_t                 m_earth_accum_ms   = 0;       // elapsed ms in current frame
    ItemVert                 m_sky_verts[4]     = {};      // CPU quad geometry, updated per-frame
    glm::mat4                m_sky_mvp          = glm::mat4(1.f); // rotation-only VP (no translation)
    bool                     m_sky_pending      = false;
};
