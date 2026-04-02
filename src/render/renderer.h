#pragma once
#include "core/world.h"
#include "core/entity_manager.h"
#include "data/voxel_registry.h"
#include "inventory/item_registry.h"
#include "simulation/world_items.h"
#include "simulation/mob_system.h"
#include "render/model_loader.h"
#include "render/lighting.h"
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <unordered_map>

// GPU mesh for one rendered chunk (CPU side).
// Vertex layout: pos.xyz (3f) + normal.xyz (3f) = 24 bytes per vertex.
struct ChunkMesh {
    glm::ivec3            chunk_pos{};
    std::vector<float>    vertices;   // 13 floats / vertex (opaque geometry)
    std::vector<uint32_t> indices;
    // Semi-transparent overlay geometry (e.g. glass on doors).
    // Uploaded to a separate GPU buffer and drawn with depth-write disabled.
    std::vector<float>    overlay_vertices;
    std::vector<uint32_t> overlay_indices;
    bool     transparent = false;
    bool     dirty       = true;
    uint64_t generation  = 0; // stamp set by ChunkMesher; stale results are discarded
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
    void draw_world(const World& world, glm::vec3 cam_pos, float yaw, float pitch, float roll = 0.f);
    // Call BEFORE begin_frame so geometry is uploaded with no lag.
    void queue_highlight(const RayHit& hit);
    void draw_face_highlight(const RayHit& hit);

    // Collect world-item quads into CPU buffer.  Call before begin_frame().
    // Flat-resting items are laid along their face; floating items billboard
    // toward the camera.  hovered_item gets a bright outline tint.
    // world_offset is added to all item positions (use for vehicle-local items).
    // Set clear_first=false to append vehicle items after the main world call.
    void queue_world_items(EntityManager& entities, EntityID hovered_item,
                           glm::vec3 cam_pos, float yaw, float pitch,
                           glm::vec3 world_offset = {0.f, 0.f, 0.f},
                           bool clear_first = true);

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
    // Populates VoxelTypeDef::atlas_indices for all registered types so the
    // chunk mesher can pick the correct layer per face direction.
    // Must be called after init(), before the first draw_world().
    bool load_tile_textures(VoxelRegistry& reg, const char* texture_dir);

    // Load item textures from disk and upload a GPU 2D-array texture.
    // Must be called after init(), before the first draw_world_items().
    bool load_item_textures(const ItemRegistry& reg, const char* texture_dir);

    // Load mob sprite textures from textures/mobs/<species>/<variant>/.
    // Expects files whose names contain: front, back, left, right.
    // Must be called after init(), before the first queue_mobs().
    bool load_mob_textures(const char* texture_dir);

    // Load extracted BYOND bodypart PNGs into CPU memory for overlay assembly.
    // extracted_dir   – path to legacysets/extracted (must contain mob/human/bodyparts/).
    // Call after init(), before the first queue_mobs().  Non-fatal if missing.
    bool load_human_bodyparts(const char* extracted_dir);

    // Load door opening GIF frames into CPU memory (scaled to 32×32).
    // door_anim_type_id is the tile atlas layer that will be patched in-place
    // each tick to show the current frame.  Call after load_tile_textures().
    bool load_door_anim(const char* gif_path, uint16_t door_anim_type_id);

    // Load the glass overlay color GIF and the fill/coverage mask GIF for
    // the door animation.  Both must have the same frame count as the base
    // door GIF.  Call after load_tile_textures().
    bool load_door_overlay_anim(const char* gif_path);
    bool load_door_fill_anim(const char* gif_path);

    // Overwrite one layer of the tile 2D-array texture in-place on the GPU.
    // rgba32x32_pixels must point to exactly 32*32*4 = 4096 bytes (RGBA8).
    // Submits a one-shot copy command; safe to call outside a render pass.
    void update_tile_layer(uint16_t type_id, const uint8_t* rgba32x32_pixels);

    // Same as update_tile_layer but targets an explicit atlas layer index
    // (used for overlay layers whose index differs from the type_id).
    void update_tile_layer_at_index(uint16_t layer_idx, const uint8_t* rgba32x32_pixels);

    // Composites overlay frame `frm` with its fill mask and uploads the result
    // to atlas layer `overlay_layer_idx`.  Uses overlay RGB + fill alpha so
    // the glass strip is always semi-transparent regardless of GIF limitations.
    // overlay_alpha is the target alpha for fully-present glass pixels [0..255].
    void composite_door_overlay_frame(int frm, uint16_t overlay_layer_idx,
                                      uint8_t overlay_alpha = 140);

    // Door animation accessors (valid after load_door_anim succeeds).
    int            door_anim_frame_count()           const { return static_cast<int>(m_door_anim_frames.size()); }
    int            door_anim_frame_delay_ms(int frm) const;
    const uint8_t* door_anim_frame_pixels  (int frm) const;
    // Returns true when both overlay and fill GIFs have been loaded successfully.
    bool           door_overlay_anim_loaded()        const { return !m_door_overlay_anim_frames.empty() && !m_door_fill_anim_frames.empty(); }

    // Queue Doom-style billboard sprites for all MobComponent entities.
    // Selects one of 4 rotation sprites based on camera-to-mob angle.
    // Call BEFORE begin_frame().
    // local_player_eid: the local player's entity ID; its head will be hidden (body-only)
    //   and it will not be back-face culled (always visible when looking down).
    // world_offset: added to all mob positions (use for vehicle-local entities).
    // Set clear_first=false to append vehicle mobs after the main world call.
    void queue_mobs(EntityManager& entities, glm::vec3 cam_pos, float cam_yaw,
                    EntityID local_player_eid = NULL_ENTITY,
                    glm::vec3 world_offset = {0.f, 0.f, 0.f},
                    bool clear_first = true);

    // Draw the queued mob sprites (call inside the render pass, after draw_world()).
    void draw_mobs();

    // Return the 32×32 RGBA 2-D texture updated by queue_mobs() for the local
    // player's front-facing assembled sprite.  Used by the HUD mirror panel.
    // Returns nullptr until queue_mobs() is called with a valid HumanAppearance player.
    SDL_GPUTexture* player_mirror_tex() const { return m_player_mirror_tex; }

    // ── Static 3-D model rendering ─────────────────────────────────────────────
    // Load a .mesh file (written by tools/fbx_to_mesh.py) and an optional
    // RGBA texture into GPU memory.  'name' is an arbitrary key reused by
    // queue_model / draw_models.  Must be called after init().
    bool load_model(const char* name,
                    const char* mesh_path,
                    const char* tex_path = nullptr);

    // Add a model instance to the per-frame draw queue.
    // pos  – world-space translation (bottom-centre of the model)
    // yaw  – rotation around Y axis in degrees
    // scale – uniform scale factor
    // Call BEFORE begin_frame() or anywhere before draw_models().
    void queue_model(const char* name,
                     glm::vec3   pos,
                     float       yaw   = 0.f,
                     float       scale = 1.f);

    // Draw all queued model instances (call inside the render pass, after draw_world()).
    void draw_models();

    // Query the local-space AABB of a loaded model (returns false if not found).
    bool model_local_aabb(const char* name,
                          glm::vec3& out_min, glm::vec3& out_max) const;

    // Queue the animated Earth / space background for the current frame.
    // Must be called BEFORE begin_frame() (like queue_world_items).
    void queue_earth_background(glm::vec3 cam_pos, float yaw, float pitch);

    // Draw the space background (call FIRST inside the render pass, before draw_world).
    void draw_space_background();

    // CPU mesh management
    ChunkMesh& get_or_create_mesh(glm::ivec3 chunk_pos);
    void       upload_mesh(ChunkMesh& mesh);   // copies to GPU
    void       free_mesh(glm::ivec3 chunk_pos);
    // Release ALL chunk GPU meshes (call after world.clear_all() on map reload).
    void       clear_all_meshes();
    // Upload all finished meshes in a single GPU command buffer (call once per frame).
    void       upload_meshes_batch(std::vector<ChunkMesh>& meshes);

    // ── Vehicle voxel-grid rendering ──────────────────────────────────────────
    // Each vehicle has its own CPU/GPU chunk-mesh storage keyed by vehicle_id.
    // Typical usage:
    //   1. vehicle_mesher.collect_finished() → pass to upload_vehicle_meshes_batch
    //   2. draw_vehicle(id, vehicle->world_pos_f()) inside the render pass
    ChunkMesh& get_or_create_vehicle_mesh(uint32_t vehicle_id, glm::ivec3 chunk_pos);
    void       upload_vehicle_meshes_batch(uint32_t vehicle_id, std::vector<ChunkMesh>& meshes);
    // Draw vehicle chunks offset by world_offset (call after draw_world).
    // m_current_mvp must be valid (i.e. draw_world must have been called this frame).
    void       draw_vehicle(uint32_t vehicle_id, glm::vec3 world_offset);
    void       clear_vehicle_meshes(uint32_t vehicle_id);

    SDL_Window*          window()       const { return m_window; }
    SDL_GPUDevice*        gpu()          const { return m_gpu; }
    SDL_GPUCommandBuffer* cmd_buf()      const { return m_cmd_buf; }
    SDL_GPUTexture*       swapchain_tex() const { return m_swapchain_tex; }
    int width()  const { return m_width; }
    int height() const { return m_height; }

    // Verbose logging: when enabled, logs detailed per-frame render stats.
    // Toggle with F8 at runtime.
    void toggle_verbose_logging() {
        m_verbose_logging = !m_verbose_logging;
        SDL_Log("Verbose logging: %s", m_verbose_logging ? "ON" : "OFF");
    }
    bool verbose_logging() const { return m_verbose_logging; }

    // ── Lighting / AO ─────────────────────────────────────────────────────────
    // Toggle fullbright mode (overrides all dynamic lighting).
    void set_fullbright(bool on) { m_fullbright = on; }
    // Set ambient-occlusion blend (0 = AO off, 1 = full AO effect).
    void set_ao_mix(float mix)  { m_ao_mix  = mix; }
    // Set minimum ambient light level (0-1, visible even in total darkness).
    void set_ambient(float a)   { m_ambient = a;   }

    // Toggle wireframe rendering (draws triangle edges instead of filled faces).
    void set_wireframe(bool on) { m_wireframe = on; }
    bool wireframe() const      { return m_wireframe; }

    // PSX vertex wobble: snap clip-space vertices to a low-precision grid.
    // snap_res is the grid resolution (cells per half-NDC).  160 gives PS1-like wobble.
    // Setting to 0 disables the effect (snap_res=0 is passed to the shader and treated
    // as the "high precision" fallback).
    void  set_psx_wobble(bool on)     { m_psx_wobble = on; }
    bool  psx_wobble()   const        { return m_psx_wobble; }
    void  set_psx_snap_res(float r)   { m_psx_snap_res = r; }
    float psx_snap_res() const        { return m_psx_snap_res; }

    // PSX y-shear: Build-engine style linear horizon pan (Duke3D look).
    // clip.y += y_shear * clip.w  →  NDC.y += y_shear (uniform screen-space shift).
    void  set_psx_yshear(float v)     { m_psx_yshear = v; }
    float psx_yshear()   const        { return m_psx_yshear; }

    // Affine (PS1-style non-perspective-correct) texture mapping blend [0..1].
    // 0 = fully perspective-correct, 1 = fully affine.
    void  set_affine_mix(float m)     { m_affine_mix = m; }
    float affine_mix()   const        { return m_affine_mix; }

    // Set the vertical field of view in degrees [60, 120].  Default 90.
    void  set_fov(float deg)       { m_fov_degrees = deg; }
    float fov_degrees() const      { return m_fov_degrees; }

    // Provide world + light map for per-entity lighting (call once after LightingSystem init).
    void set_lighting(const World* world, const LightMap* light_map) {
        m_world     = world;
        m_light_map = light_map;
    }

private:
    // Compute an RGB light tint for an entity at 'pos' from the voxel light grid.
    // Returns {1,1,1} in fullbright mode or when no world is set.
    glm::vec3 entity_light_tint(glm::vec3 pos) const;
    // ── Window / device ──────────────────────────────────────────────────────
    SDL_Window*    m_window = nullptr;
    SDL_GPUDevice* m_gpu    = nullptr;
    int m_width  = 0;
    int m_height = 0;

    // ── Debug / logging ───────────────────────────────────────────────────────
    bool m_verbose_logging = false;

    // ── Lighting state (pushed to fragment UBO each draw_world call) ─────────
    bool  m_fullbright = false;
    float m_ao_mix     = 0.f;   // 0 = AO disabled, 1 = AO enabled
    float m_ambient    = 0.3f;  // ambient floor (visible in darkness)

    // ── Wireframe mode ───────────────────────────────────────────────────────
    bool  m_wireframe  = false;

    // ── PSX vertex wobble ─────────────────────────────────────────────────────
    bool  m_psx_wobble    = false;
    float m_psx_snap_res  = 160.f;  // PS1: ~160 grid cells per half-NDC axis
    float m_psx_yshear    = 0.f;    // y-shear amount (tan(pitch)*strength; 0 = off)
    // ── Affine texture mapping ─────────────────────────────────────────
    float m_affine_mix    = 0.f;    // 0 = perspective-correct, 1 = fully affine
    // ── Field of view ────────────────────────────────────────────────────────
    float m_fov_degrees = 90.f;

    // ── World/lightmap pointers for entity lighting ───────────────────────────
    const World*    m_world      = nullptr;
    const LightMap* m_light_map  = nullptr;

    // ── Pipeline ─────────────────────────────────────────────────────────────
    SDL_GPUGraphicsPipeline* m_world_pipeline         = nullptr;
    SDL_GPUGraphicsPipeline* m_wireframe_pipeline     = nullptr;  // LINE fill variant
    SDL_GPUGraphicsPipeline* m_world_overlay_pipeline = nullptr;  // depth-write=off for transparent quads
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

    // ── Human overlay assembly cache ──────────────────────────────────────────
    // CPU-side pixel store for extracted bodypart PNGs.
    // Key format: "{sub_dir}/{stem}"  e.g. "bodyparts/default_human_chest_s"
    // For clothing/inhands keys are relative to mob_dir, e.g. "clothing/suits/spacesuit/space_s"
    // Value: 32×32 RGBA (4096 bytes).
    std::unordered_map<std::string, std::vector<uint8_t>> m_bodypart_pixels;

    // Base path (legacysets/extracted/mob) used for lazy clothing/inhand loading.
    std::string m_extracted_mob_dir;

    // Per-direction assembled-appearance cache.
    // Key encodes the full appearance + direction; value = GPU layer index.
    std::unordered_map<std::string, uint32_t> m_assembly_cache;

    // Pre-allocated GPU texture array for assembled sprites.
    // Layer 0 = transparent/white fallback.
    static constexpr uint32_t k_assembly_w          = 32;
    static constexpr uint32_t k_assembly_h          = 32;
    static constexpr uint32_t k_max_assembly_layers = 512;
    uint32_t                  m_assembly_used        = 1;   // next free layer
    SDL_GPUTexture*           m_assembly_tex         = nullptr;
    SDL_GPUSampler*           m_assembly_sampler     = nullptr;

    // Single 2-D (non-array) texture updated every frame with the local player's
    // front-facing assembled sprite for use by the HUD mirror panel.
    SDL_GPUTexture* m_player_mirror_tex = nullptr;
    std::string     m_player_mirror_key;   // last-used cache key; skip upload when unchanged

    // Assembled-mob quad staging (separate from legacy mob quads).
    static constexpr uint32_t k_max_asm_quads = 64;
    std::vector<ItemVert>     m_asm_verts;
    std::vector<uint32_t>     m_asm_indices;
    bool                      m_asm_pending   = false;
    SDL_GPUBuffer*            m_asm_vbuf      = nullptr;
    SDL_GPUBuffer*            m_asm_ibuf      = nullptr;

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
        uint32_t       num_indices   = 0;
        Uint32         vbuf_capacity = 0;  // allocated byte size of vbuf
        Uint32         ibuf_capacity = 0;  // allocated byte size of ibuf
        // Overlay (semi-transparent, depth-write=off) buffers
        SDL_GPUBuffer* ov_vbuf = nullptr;
        SDL_GPUBuffer* ov_ibuf = nullptr;
        uint32_t       ov_num_indices   = 0;
        Uint32         ov_vbuf_capacity = 0;
        Uint32         ov_ibuf_capacity = 0;
    };
    std::unordered_map<glm::ivec3, GPUMesh> m_gpu_meshes;

    // ── Vehicle chunk mesh storage ────────────────────────────────────────────
    // Separate CPU + GPU mesh maps per vehicle_id.  Populated by
    // upload_vehicle_meshes_batch(); rendered by draw_vehicle().
    std::unordered_map<uint32_t, std::unordered_map<glm::ivec3, ChunkMesh>> m_vehicle_cpu_meshes;
    std::unordered_map<uint32_t, std::unordered_map<glm::ivec3, GPUMesh>>   m_vehicle_gpu_meshes;

    // ── Helpers ───────────────────────────────────────────────────────────────
    bool create_depth_texture();
    bool create_pipeline();
    bool create_highlight_pipeline();
    bool create_item_pipeline();
    bool create_mob_buffers();
    bool create_assembly_texture();               // pre-allocate assembly GPU tex array
    bool create_sky_pipeline();          // background Earth-orbit pass
    bool load_earth_gif(const char* path);
    bool create_sky_util_texture();      // 2-layer: [0]=1×1 white, [1]=64×64 soft-circle cloud
    bool build_star_geometry();          // generate + upload static star quads
    bool build_cloud_geometry();         // generate + upload static nebula quads
    void upload_highlight_geometry();   // copy pass before render pass
    void upload_item_geometry();        // copy pass before render pass
    void upload_mob_geometry();         // copy pass before render pass
    void upload_asm_geometry();         // copy pass before render pass (assembled mobs)
    void upload_sky_geometry();         // copy pass before render pass (earth verts only)
    void release_gpu_mesh(GPUMesh& gm);
    static bool aabb_in_frustum(const glm::mat4& mvp, glm::vec3 mn, glm::vec3 mx);

    // Shared upload helper: transfers a batch of CPU ChunkMesh objects into
    // an arbitrary GPU mesh map.  Used by both upload_meshes_batch (→ m_gpu_meshes)
    // and upload_vehicle_meshes_batch (→ m_vehicle_gpu_meshes[id]).
    void upload_meshes_to(std::vector<ChunkMesh>& meshes,
                          std::unordered_map<glm::ivec3, GPUMesh>& gpu_map);

    // Shared draw helper: renders a GPU mesh set with an additional world-space
    // offset baked into the per-chunk model matrix.  Used by draw_world (offset=0)
    // and draw_vehicle (offset=vehicle world position).
    void draw_gpu_mesh_set(std::unordered_map<glm::ivec3, GPUMesh>& gpu_meshes,
                           const glm::mat4& view_proj, glm::vec3 world_offset);

    // Upload one 32×32 RGBA image into m_assembly_tex at the given layer index.
    // Submits a one-shot copy command (safe outside a render pass).
    void upload_assembly_layer(uint32_t layer_idx, const uint8_t* rgba32x32);

    // Lazily load a clothing/inhand overlay sprite on first use.
    // key is relative to m_extracted_mob_dir without extension, forward slashes.
    // Returns pointer into m_bodypart_pixels (nullptr if file not found).
    const std::vector<uint8_t>* load_overlay_on_demand(const std::string& key);

    // Return (cached) assembly texture layer for the given HumanAppearance + direction.
    // Composites all HumanOverlay layers on the CPU if not already cached, then
    // uploads the result to GPU and returns the layer index.  Returns 0 on failure.
    // dir: 0=front(s) 1=back(n) 2=left(e) 3=right(w).
    uint32_t get_or_assemble_human(HumanAppearance& app, int dir);

    // Compose 32×32 RGBA canvas for the given HumanAppearance + direction without
    // uploading to the assembly array texture.  Returns an empty vector on failure.
    std::vector<uint8_t> compose_human_canvas(const HumanAppearance& app, int dir);

    // Upload the player's front-facing assembled sprite to m_player_mirror_tex.
    // Skips re-upload when the appearance hasn't changed since last call.
    void refresh_player_mirror(HumanAppearance& app);

    // ── Sky / space background (Earth orbit, stars, nebulae) ─────────────────
    SDL_GPUGraphicsPipeline* m_sky_pipeline    = nullptr;

    // Earth GIF
    SDL_GPUBuffer*           m_sky_vbuf        = nullptr;  // 4 verts × ItemVert (Earth quad)
    SDL_GPUBuffer*           m_sky_ibuf        = nullptr;  // 6 uint32 indices (Earth quad)
    SDL_GPUTexture*          m_earth_tex_array = nullptr;  // GIF frames as 2D-array layers
    SDL_GPUSampler*          m_earth_sampler   = nullptr;
    uint32_t                 m_earth_num_frames = 0;
    uint32_t                 m_earth_frame      = 0;
    std::vector<int>         m_earth_delays;
    uint32_t                 m_earth_accum_ms   = 0;
    ItemVert                 m_sky_verts[4]     = {};      // Earth quad, updated per-frame
    bool                     m_earth_geo_pending= false;   // earth verts need upload

    // Utility texture: layer 0 = white 1×1, layer 1 = 64×64 soft-circle alpha
    SDL_GPUTexture*          m_sky_util_tex     = nullptr;
    SDL_GPUSampler*          m_sky_util_sampler = nullptr;

    // Stars (static, generated once at init)
    SDL_GPUBuffer*           m_star_vbuf        = nullptr;
    SDL_GPUBuffer*           m_star_ibuf        = nullptr;
    uint32_t                 m_star_index_count = 0;

    // Nebula dust clouds (static, generated once at init)
    SDL_GPUBuffer*           m_cloud_vbuf       = nullptr;
    SDL_GPUBuffer*           m_cloud_ibuf       = nullptr;
    uint32_t                 m_cloud_index_count= 0;

    glm::mat4                m_sky_mvp          = glm::mat4(1.f); // rotation-only VP
    bool                     m_sky_mvp_ready    = false;   // MVP valid this frame
    bool                     m_sky_pending      = false;   // legacy alias (earth geo)

    // ── Door GIF animation (CPU-side frames) ─────────────────────────────────
    std::vector<std::vector<uint8_t>> m_door_anim_frames;         // [frame][32*32*4 bytes]
    std::vector<int>                  m_door_anim_delays;         // ms per frame
    std::vector<std::vector<uint8_t>> m_door_overlay_anim_frames; // glass color overlay frames
    std::vector<std::vector<uint8_t>> m_door_fill_anim_frames;    // fill/coverage mask frames

    // ── Static 3-D models ────────────────────────────────────────────────────
    struct ModelGPU {
        SDL_GPUBuffer*  vbuf        = nullptr;
        SDL_GPUBuffer*  ibuf        = nullptr;
        uint32_t        num_indices = 0;
        SDL_GPUTexture* tex         = nullptr;  // 1-layer 2D-array (or nullptr)
        SDL_GPUSampler* sampler     = nullptr;
        // Local-space AABB from the .mesh file
        glm::vec3       local_min{0.f, 0.f, 0.f};
        glm::vec3       local_max{0.f, 0.f, 0.f};
    };
    struct ModelInstance {
        std::string name;
        glm::vec3   pos;
        float       yaw;
        float       scale;
    };
    std::unordered_map<std::string, ModelGPU> m_models;
    std::vector<ModelInstance>                m_model_queue;
};
