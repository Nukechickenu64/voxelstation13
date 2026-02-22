#include "render/renderer.h"
#include "render/shaders/chunk_vert_spv.h"
#include "render/shaders/chunk_frag_spv.h"
#include "render/shaders/highlight_vert_spv.h"
#include "render/shaders/highlight_frag_spv.h"
#include "render/shaders/item_vert_spv.h"
#include "render/shaders/item_frag_spv.h"
#include "core/world.h"
#include "data/voxel_registry.h"
#include "simulation/physics.h"
#include "stb_image.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

#include <cstring>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <cctype>
#include <algorithm>

Renderer::Renderer()  = default;
Renderer::~Renderer() { shutdown(); }

// ── Init / shutdown ───────────────────────────────────────────────────────────

bool Renderer::init(const char* title, int width, int height)
{
    m_width  = width;
    m_height = height;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    m_window = SDL_CreateWindow(title, width, height,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!m_window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    m_gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV,
                                false, "vulkan");
    if (!m_gpu) {
        // Vulkan not available; report clearly rather than silently failing
        SDL_Log("SDL_CreateGPUDevice (Vulkan/SPIRV) failed: %s", SDL_GetError());
        SDL_Log("Note: DXIL (D3D12) shaders are not yet provided. "
                "Install Vulkan drivers (https://vulkan.lunarg.com) or add DXIL shaders.");
        return false;
    }

    SDL_Log("GPU driver: %s", SDL_GetGPUDeviceDriver(m_gpu));

    if (!SDL_ClaimWindowForGPUDevice(m_gpu, m_window)) {
        SDL_Log("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        return false;
    }

    if (!create_depth_texture())       return false;
    if (!create_pipeline())            return false;
    if (!create_highlight_pipeline())  return false;
    if (!create_item_pipeline())       return false;
    if (!create_mob_buffers())         return false;
    if (!create_sky_pipeline())        return false;
    load_earth_gif("textures/decoration/earthspin.gif");  // non-fatal if missing

    return true;
}

bool Renderer::create_depth_texture()
{
    // Pick the best supported depth format
    const SDL_GPUTextureFormat candidates[] = {
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
        SDL_GPU_TEXTUREFORMAT_D24_UNORM,
        SDL_GPU_TEXTUREFORMAT_D16_UNORM,
    };
    m_depth_fmt = SDL_GPU_TEXTUREFORMAT_INVALID;
    for (auto fmt : candidates) {
        if (SDL_GPUTextureSupportsFormat(m_gpu, fmt,
                SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
            m_depth_fmt = fmt;
            break;
        }
    }
    if (m_depth_fmt == SDL_GPU_TEXTUREFORMAT_INVALID) {
        SDL_Log("No supported depth format found.");
        return false;
    }

    SDL_GPUTextureCreateInfo info{};
    info.type          = SDL_GPU_TEXTURETYPE_2D;
    info.format        = m_depth_fmt;
    info.usage         = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    info.width         = (Uint32)m_width;
    info.height        = (Uint32)m_height;
    info.layer_count_or_depth = 1;
    info.num_levels    = 1;
    info.sample_count  = SDL_GPU_SAMPLECOUNT_1;

    m_depth_tex = SDL_CreateGPUTexture(m_gpu, &info);
    if (!m_depth_tex) {
        SDL_Log("SDL_CreateGPUTexture (depth) failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

bool Renderer::create_pipeline()
{
    // ── Vertex shader ─────────────────────────────────────────────────────────
    SDL_GPUShaderCreateInfo vi{};
    vi.code               = reinterpret_cast<const Uint8*>(k_chunk_vert_spv);
    vi.code_size          = k_chunk_vert_spv_size;
    vi.entrypoint         = "main";
    vi.format             = SDL_GPU_SHADERFORMAT_SPIRV;
    vi.stage              = SDL_GPU_SHADERSTAGE_VERTEX;
    vi.num_uniform_buffers = 1;   // slot 0 = MVP (SDL_PushGPUVertexUniformData)

    m_vert_shader = SDL_CreateGPUShader(m_gpu, &vi);
    if (!m_vert_shader) {
        SDL_Log("SDL_CreateGPUShader (vert) failed: %s", SDL_GetError());
        return false;
    }

    // ── Fragment shader ───────────────────────────────────────────────────────
    SDL_GPUShaderCreateInfo fi{};
    fi.code               = reinterpret_cast<const Uint8*>(k_chunk_frag_spv);
    fi.code_size          = k_chunk_frag_spv_size;
    fi.entrypoint         = "main";
    fi.format             = SDL_GPU_SHADERFORMAT_SPIRV;
    fi.stage              = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fi.num_samplers       = 1;   // slot 0 = tile texture array (set=0, binding=0)

    m_frag_shader = SDL_CreateGPUShader(m_gpu, &fi);
    if (!m_frag_shader) {
        SDL_Log("SDL_CreateGPUShader (frag) failed: %s", SDL_GetError());
        return false;
    }

    // ── Vertex layout: pos(3f) + normal(3f) + uv(2f) + texIndex(1f), stride 36 ─────
    SDL_GPUVertexBufferDescription vbuf_desc{};
    vbuf_desc.slot              = 0;
    vbuf_desc.pitch             = 36;   // 9 floats × 4 bytes
    vbuf_desc.input_rate        = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vbuf_desc.instance_step_rate = 0;

    SDL_GPUVertexAttribute vattrs[4]{};
    vattrs[0] = { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,  0 }; // location 0 = pos
    vattrs[1] = { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12 }; // location 1 = normal
    vattrs[2] = { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 24 }; // location 2 = uv
    vattrs[3] = { 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT,  32 }; // location 3 = texIndex

    // ── Colour target format (matches the swapchain) ──────────────────────────
    SDL_GPUColorTargetDescription ctd{};
    ctd.format = SDL_GetGPUSwapchainTextureFormat(m_gpu, m_window);
    if (ctd.format == SDL_GPU_TEXTUREFORMAT_INVALID) {
        SDL_Log("create_pipeline: invalid swapchain format, aborting");
        return false;
    }

    // ── Build pipeline ────────────────────────────────────────────────────────
    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader   = m_vert_shader;
    pci.fragment_shader = m_frag_shader;

    pci.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
    pci.vertex_input_state.num_vertex_buffers         = 1;
    pci.vertex_input_state.vertex_attributes          = vattrs;
    pci.vertex_input_state.num_vertex_attributes      = 4;

    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    pci.rasterizer_state.fill_mode        = SDL_GPU_FILLMODE_FILL;
    pci.rasterizer_state.cull_mode        = SDL_GPU_CULLMODE_BACK;
    pci.rasterizer_state.front_face       = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    pci.depth_stencil_state.enable_depth_test  = true;
    pci.depth_stencil_state.enable_depth_write = true;
    pci.depth_stencil_state.compare_op         = SDL_GPU_COMPAREOP_LESS;

    pci.target_info.color_target_descriptions = &ctd;
    pci.target_info.num_color_targets         = 1;
    pci.target_info.has_depth_stencil_target  = true;
    pci.target_info.depth_stencil_format      = m_depth_fmt;

    SDL_Log("Renderer: world pipeline created");
    m_world_pipeline = SDL_CreateGPUGraphicsPipeline(m_gpu, &pci);
    if (!m_world_pipeline) {
        SDL_Log("SDL_CreateGPUGraphicsPipeline failed: %s", SDL_GetError());
        return false;
    }

    // Shaders no longer needed after pipeline creation
    SDL_ReleaseGPUShader(m_gpu, m_vert_shader); m_vert_shader = nullptr;
    SDL_ReleaseGPUShader(m_gpu, m_frag_shader); m_frag_shader = nullptr;

    return true;
}

void Renderer::shutdown()
{
    if (m_gpu) {
        // Wait for any in-flight work to complete
        SDL_WaitForGPUIdle(m_gpu);

        for (auto& [pos, gm] : m_gpu_meshes)
            release_gpu_mesh(gm);
        m_gpu_meshes.clear();

        if (m_world_pipeline)     { SDL_ReleaseGPUGraphicsPipeline(m_gpu, m_world_pipeline);     m_world_pipeline     = nullptr; }
        if (m_highlight_pipeline) { SDL_ReleaseGPUGraphicsPipeline(m_gpu, m_highlight_pipeline); m_highlight_pipeline = nullptr; }
        if (m_item_pipeline)      { SDL_ReleaseGPUGraphicsPipeline(m_gpu, m_item_pipeline);      m_item_pipeline      = nullptr; }
        if (m_highlight_vbuf)     { SDL_ReleaseGPUBuffer(m_gpu, m_highlight_vbuf);               m_highlight_vbuf     = nullptr; }
        if (m_highlight_ibuf)     { SDL_ReleaseGPUBuffer(m_gpu, m_highlight_ibuf);               m_highlight_ibuf     = nullptr; }
        if (m_item_vbuf)          { SDL_ReleaseGPUBuffer(m_gpu, m_item_vbuf);                    m_item_vbuf          = nullptr; }
        if (m_item_ibuf)          { SDL_ReleaseGPUBuffer(m_gpu, m_item_ibuf);                    m_item_ibuf          = nullptr; }
        if (m_mob_vbuf)           { SDL_ReleaseGPUBuffer(m_gpu, m_mob_vbuf);                     m_mob_vbuf           = nullptr; }
        if (m_mob_ibuf)           { SDL_ReleaseGPUBuffer(m_gpu, m_mob_ibuf);                     m_mob_ibuf           = nullptr; }
        if (m_mob_tex_array)      { SDL_ReleaseGPUTexture(m_gpu, m_mob_tex_array);               m_mob_tex_array      = nullptr; }
        if (m_mob_sampler)        { SDL_ReleaseGPUSampler(m_gpu, m_mob_sampler);                 m_mob_sampler        = nullptr; }
        if (m_vert_shader)        { SDL_ReleaseGPUShader(m_gpu, m_vert_shader);                  m_vert_shader        = nullptr; }
        if (m_frag_shader)        { SDL_ReleaseGPUShader(m_gpu, m_frag_shader);                  m_frag_shader        = nullptr; }
        if (m_tile_array)         { SDL_ReleaseGPUTexture(m_gpu, m_tile_array);                  m_tile_array         = nullptr; }
        if (m_tile_sampler)       { SDL_ReleaseGPUSampler(m_gpu, m_tile_sampler);                m_tile_sampler       = nullptr; }
        if (m_item_tex_array)     { SDL_ReleaseGPUTexture(m_gpu, m_item_tex_array);              m_item_tex_array     = nullptr; }
        if (m_item_sampler)       { SDL_ReleaseGPUSampler(m_gpu, m_item_sampler);                m_item_sampler       = nullptr; }
        if (m_depth_tex)          { SDL_ReleaseGPUTexture(m_gpu, m_depth_tex);                   m_depth_tex          = nullptr; }
        if (m_sky_pipeline)       { SDL_ReleaseGPUGraphicsPipeline(m_gpu, m_sky_pipeline);       m_sky_pipeline       = nullptr; }
        if (m_sky_vbuf)           { SDL_ReleaseGPUBuffer(m_gpu, m_sky_vbuf);                     m_sky_vbuf           = nullptr; }
        if (m_sky_ibuf)           { SDL_ReleaseGPUBuffer(m_gpu, m_sky_ibuf);                     m_sky_ibuf           = nullptr; }
        if (m_earth_tex_array)    { SDL_ReleaseGPUTexture(m_gpu, m_earth_tex_array);             m_earth_tex_array    = nullptr; }
        if (m_earth_sampler)      { SDL_ReleaseGPUSampler(m_gpu, m_earth_sampler);               m_earth_sampler      = nullptr; }

        if (m_window) SDL_ReleaseWindowFromGPUDevice(m_gpu, m_window);
        SDL_DestroyGPUDevice(m_gpu);
        m_gpu = nullptr;
    }
    if (m_window) { SDL_DestroyWindow(m_window); m_window = nullptr; }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void Renderer::release_gpu_mesh(GPUMesh& gm)
{
    if (gm.vbuf) { SDL_ReleaseGPUBuffer(m_gpu, gm.vbuf); gm.vbuf = nullptr; }
    if (gm.ibuf) { SDL_ReleaseGPUBuffer(m_gpu, gm.ibuf); gm.ibuf = nullptr; }
    gm.num_indices = 0;
}

// ── Per-frame render ──────────────────────────────────────────────────────────

void Renderer::begin_frame(double /*alpha*/)
{
    m_cmd_buf = SDL_AcquireGPUCommandBuffer(m_gpu);
    if (!m_cmd_buf) {
        SDL_Log("SDL_AcquireGPUCommandBuffer failed: %s", SDL_GetError());
        return;
    }

    m_swapchain_tex = nullptr;
    Uint32 sw_w = 0, sw_h = 0;
    SDL_AcquireGPUSwapchainTexture(m_cmd_buf, m_window,
                                   &m_swapchain_tex, &sw_w, &sw_h);
    if (!m_swapchain_tex) {
        // Window might be minimised – skip this frame
        SDL_SubmitGPUCommandBuffer(m_cmd_buf);
        m_cmd_buf = nullptr;
        return;
    }

    // Rebuild depth texture if the window was resized
    if (sw_w && sw_h && (sw_w != (Uint32)m_width || sw_h != (Uint32)m_height)) {
        m_width  = (int)sw_w;
        m_height = (int)sw_h;
        if (m_depth_tex) { SDL_ReleaseGPUTexture(m_gpu, m_depth_tex); m_depth_tex = nullptr; }
        create_depth_texture();
    }

    // ── Upload pending highlight geometry (copy pass before render pass) ─────
    upload_highlight_geometry();
    upload_item_geometry();
    upload_mob_geometry();

    // ── Upload pending sky/earth geometry (copy pass before render pass) ──────
    upload_sky_geometry();

    // ── Begin render pass ─────────────────────────────────────────────────────
    SDL_GPUColorTargetInfo color_info{};
    color_info.texture     = m_swapchain_tex;
    color_info.clear_color = { 0.0f, 0.0f, 0.02f, 1.0f };  // deep space black
    color_info.load_op     = SDL_GPU_LOADOP_CLEAR;
    color_info.store_op    = SDL_GPU_STOREOP_STORE;

    SDL_GPUDepthStencilTargetInfo depth_info{};
    depth_info.texture            = m_depth_tex;
    depth_info.clear_depth        = 1.0f;
    depth_info.load_op            = SDL_GPU_LOADOP_CLEAR;
    depth_info.store_op           = SDL_GPU_STOREOP_DONT_CARE;
    depth_info.stencil_load_op    = SDL_GPU_LOADOP_DONT_CARE;
    depth_info.stencil_store_op   = SDL_GPU_STOREOP_DONT_CARE;

    m_render_pass = SDL_BeginGPURenderPass(
        m_cmd_buf,
        &color_info, 1,
        m_depth_tex ? &depth_info : nullptr);
}

void Renderer::draw_world(const World& /*world*/,
                          glm::vec3 cam_pos, float yaw, float pitch)
{
    if (!m_render_pass || !m_world_pipeline) return;

    // ── Build view-projection matrix ─────────────────────────────────────────
    float yaw_r   = glm::radians(yaw);
    float pitch_r = glm::radians(pitch);
    glm::vec3 forward = {
        std::cos(pitch_r) * std::sin(yaw_r),
        std::sin(pitch_r),
       -std::cos(pitch_r) * std::cos(yaw_r)
    };
    glm::mat4 view = glm::lookAt(cam_pos, cam_pos + forward, {0.f, 1.f, 0.f});
    float aspect = (m_height > 0) ? float(m_width) / float(m_height) : 1.f;
    glm::mat4 proj = glm::perspective(glm::radians(90.f), aspect, 0.1f, 400.f);
    // No manual Y-flip: SDL3 GPU handles Vulkan clip-space internally
    glm::mat4 view_proj = proj * view;
    m_current_mvp = view_proj;  // save plain VP for the highlight pass (world-space verts)

    // ── Draw Earth / space background first (no depth, always behind everything) ──
    draw_space_background();

    SDL_BindGPUGraphicsPipeline(m_render_pass, m_world_pipeline);

    // Bind tile texture array to fragment sampler slot 0
    if (m_tile_array && m_tile_sampler) {
        SDL_GPUTextureSamplerBinding tsb{ m_tile_array, m_tile_sampler };
        SDL_BindGPUFragmentSamplers(m_render_pass, 0, &tsb, 1);
    }

    int drawn = 0;
    // ── Draw each uploaded GPU mesh (frustum-culled) ──────────────────────────
    for (auto& [chunk_pos, gm] : m_gpu_meshes) {
        if (!gm.vbuf || !gm.ibuf || gm.num_indices == 0) continue;

        // Chunk AABB in world space (for frustum culling)
        glm::vec3 mn = glm::vec3(chunk_pos)     * float(CHUNK_SIZE);
        glm::vec3 mx = glm::vec3(chunk_pos + 1) * float(CHUNK_SIZE);
        if (!aabb_in_frustum(view_proj, mn, mx)) continue;

        // Per-chunk MVP: translate local (0..CHUNK_SIZE) verts to world space
        glm::mat4 model = glm::translate(glm::mat4(1.f),
                                         glm::vec3(chunk_pos) * float(CHUNK_SIZE));
        glm::mat4 chunk_mvp = view_proj * model;
        SDL_PushGPUVertexUniformData(m_cmd_buf, 0, &chunk_mvp[0][0], sizeof(glm::mat4));

        SDL_GPUBufferBinding vb{ gm.vbuf, 0 };
        SDL_GPUBufferBinding ib{ gm.ibuf, 0 };
        SDL_BindGPUVertexBuffers(m_render_pass, 0, &vb, 1);
        SDL_BindGPUIndexBuffer(m_render_pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(m_render_pass, gm.num_indices, 1, 0, 0, 0);
        ++drawn;
    }
    if (drawn > 0 || !m_gpu_meshes.empty())
        SDL_Log("draw_world: %d/%d chunks drawn", drawn, (int)m_gpu_meshes.size());
}

void Renderer::draw_face_highlight(const RayHit& hit)
{
    if (!hit.valid || !m_hl_valid || !m_highlight_pipeline || !m_render_pass) return;

    SDL_BindGPUGraphicsPipeline(m_render_pass, m_highlight_pipeline);
    SDL_PushGPUVertexUniformData(m_cmd_buf, 0, &m_current_mvp[0][0], sizeof(glm::mat4));
    SDL_GPUBufferBinding vb{ m_highlight_vbuf, 0 };
    SDL_GPUBufferBinding ib{ m_highlight_ibuf, 0 };
    SDL_BindGPUVertexBuffers(m_render_pass, 0, &vb, 1);
    SDL_BindGPUIndexBuffer(m_render_pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(m_render_pass, 6, 1, 0, 0, 0);
}

void Renderer::draw_viewmodel(uint16_t /*item_type_id*/)
{
    // TODO: render held item mesh in view-model pass
}

void Renderer::end_world_pass()
{
    if (m_render_pass) {
        SDL_EndGPURenderPass(m_render_pass);
        m_render_pass = nullptr;
    }
}

void Renderer::end_frame()
{
    // End world render pass if end_world_pass() wasn't called explicitly
    end_world_pass();
    if (m_cmd_buf) {
        SDL_SubmitGPUCommandBuffer(m_cmd_buf);
        m_cmd_buf       = nullptr;
        m_swapchain_tex = nullptr;
    }
}

// ── Mesh management ───────────────────────────────────────────────────────────

ChunkMesh& Renderer::get_or_create_mesh(glm::ivec3 chunk_pos)
{
    auto& mesh = m_meshes[chunk_pos];
    mesh.chunk_pos = chunk_pos;
    return mesh;
}

void Renderer::upload_mesh(ChunkMesh& mesh)
{
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        // Nothing to upload – release any old GPU buffers
        auto it = m_gpu_meshes.find(mesh.chunk_pos);
        if (it != m_gpu_meshes.end()) { release_gpu_mesh(it->second); }
        mesh.dirty = false;
        return;
    }

    auto& gm = m_gpu_meshes[mesh.chunk_pos];
    release_gpu_mesh(gm);   // free old buffers if any

    const Uint32 vsize = (Uint32)(mesh.vertices.size() * sizeof(float));
    const Uint32 isize = (Uint32)(mesh.indices.size()  * sizeof(uint32_t));

    // ── Create GPU vertex / index buffers ─────────────────────────────────────
    SDL_GPUBufferCreateInfo vbi{};
    vbi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vbi.size  = vsize;
    gm.vbuf = SDL_CreateGPUBuffer(m_gpu, &vbi);

    SDL_GPUBufferCreateInfo ibi{};
    ibi.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    ibi.size  = isize;
    gm.ibuf = SDL_CreateGPUBuffer(m_gpu, &ibi);

    if (!gm.vbuf || !gm.ibuf) {
        SDL_Log("SDL_CreateGPUBuffer failed: %s", SDL_GetError());
        release_gpu_mesh(gm);
        return;
    }

    // ── Create staging (transfer) buffer ─────────────────────────────────────
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size  = vsize + isize;
    auto* tbuf = SDL_CreateGPUTransferBuffer(m_gpu, &tbi);
    if (!tbuf) {
        SDL_Log("SDL_CreateGPUTransferBuffer failed: %s", SDL_GetError());
        release_gpu_mesh(gm);
        return;
    }

    // ── Fill staging buffer ───────────────────────────────────────────────────
    auto* ptr = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(m_gpu, tbuf, false));
    std::memcpy(ptr,         mesh.vertices.data(), vsize);
    std::memcpy(ptr + vsize, mesh.indices.data(),  isize);
    SDL_UnmapGPUTransferBuffer(m_gpu, tbuf);

    // ── Issue GPU copy ────────────────────────────────────────────────────────
    auto* cmd       = SDL_AcquireGPUCommandBuffer(m_gpu);
    auto* copy_pass = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTransferBufferLocation src_v{ tbuf, 0 };
    SDL_GPUBufferRegion           dst_v{ gm.vbuf, 0, vsize };
    SDL_UploadToGPUBuffer(copy_pass, &src_v, &dst_v, false);

    SDL_GPUTransferBufferLocation src_i{ tbuf, vsize };
    SDL_GPUBufferRegion           dst_i{ gm.ibuf, 0, isize };
    SDL_UploadToGPUBuffer(copy_pass, &src_i, &dst_i, false);

    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(m_gpu, tbuf);

    gm.num_indices = (uint32_t)mesh.indices.size();
    mesh.dirty = false;
}

void Renderer::free_mesh(glm::ivec3 chunk_pos)
{
    auto it = m_gpu_meshes.find(chunk_pos);
    if (it != m_gpu_meshes.end()) { release_gpu_mesh(it->second); m_gpu_meshes.erase(it); }
    m_meshes.erase(chunk_pos);
}

// ── Tile texture array ─────────────────────────────────────────────────────────────────

bool Renderer::load_tile_textures(const VoxelRegistry& reg, const char* texture_dir)
{
    // Build a list of texture paths indexed by type_id.
    // type_id 0 = empty (never rendered) – use fallback.
    const auto& all = reg.all();  // unordered_map<uint16_t, VoxelTypeDef>

    // Find max type_id so we know the array size
    uint32_t max_id = 0;
    for (const auto& [id, def] : all)
        if (id > max_id) max_id = id;

    const uint32_t num_layers = max_id + 1;  // layers 0..max_id
    const uint32_t TEX_W      = 32;
    const uint32_t TEX_H      = 32;
    const uint32_t LAYER_BYTES = TEX_W * TEX_H * 4;  // RGBA8

    // Allocate CPU buffer: one RGBA layer per slot
    std::vector<uint8_t> pixels(num_layers * LAYER_BYTES, 0xFF);  // default white

    // Fallback texture path
    std::string fallback_path = std::string(texture_dir) + "/tiles/fallback.png";

    auto load_layer = [&](uint32_t layer, const std::string& path) {
        int w, h, ch;
        unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
        if (!data) {
            SDL_Log("load_tile_textures: stbi_load failed for %s: %s", path.c_str(), stbi_failure_reason());
            return;
        }
        uint8_t* dst = pixels.data() + layer * LAYER_BYTES;
        if (w == (int)TEX_W && h == (int)TEX_H) {
            std::memcpy(dst, data, LAYER_BYTES);
        } else {
            // Scale down / pad by nearest-neighbor to TEX_W x TEX_H
            for (uint32_t py = 0; py < TEX_H; ++py)
            for (uint32_t px = 0; px < TEX_W; ++px) {
                int sx = (int)(px * w / TEX_W);
                int sy = (int)(py * h / TEX_H);
                const unsigned char* src_px = data + (sy * w + sx) * 4;
                uint8_t* d = dst + (py * TEX_W + px) * 4;
                d[0] = src_px[0]; d[1] = src_px[1];
                d[2] = src_px[2]; d[3] = src_px[3];
            }
        }
        stbi_image_free(data);
    };

    // Layer 0 = fallback (type_id 0 = empty/unset, never rendered)
    load_layer(0, fallback_path);

    // Load a texture for each registered voxel type
    for (const auto& [id, def] : all) {
        if (def.icon.empty()) {
            load_layer(id, fallback_path);
        } else {
            std::string path = std::string(texture_dir) + "/" + def.icon + ".png";
            load_layer(id, path);
        }
    }

    // ── Create GPU 2D array texture ───────────────────────────────────────────
    SDL_GPUTextureCreateInfo tci{};
    tci.type                    = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    tci.format                  = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage                   = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width                   = TEX_W;
    tci.height                  = TEX_H;
    tci.layer_count_or_depth    = num_layers;
    tci.num_levels              = 1;
    tci.sample_count            = SDL_GPU_SAMPLECOUNT_1;
    m_tile_array = SDL_CreateGPUTexture(m_gpu, &tci);
    if (!m_tile_array) {
        SDL_Log("load_tile_textures: SDL_CreateGPUTexture failed: %s", SDL_GetError());
        return false;
    }

    // ── Upload via staging buffer ─────────────────────────────────────────────
    const Uint32 total_bytes = (Uint32)pixels.size();
    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size  = total_bytes;
    auto* tbuf = SDL_CreateGPUTransferBuffer(m_gpu, &tbci);
    if (!tbuf) {
        SDL_Log("load_tile_textures: SDL_CreateGPUTransferBuffer failed: %s", SDL_GetError());
        return false;
    }
    auto* ptr = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(m_gpu, tbuf, false));
    std::memcpy(ptr, pixels.data(), total_bytes);
    SDL_UnmapGPUTransferBuffer(m_gpu, tbuf);

    auto* cmd       = SDL_AcquireGPUCommandBuffer(m_gpu);
    auto* copy_pass = SDL_BeginGPUCopyPass(cmd);

    for (uint32_t layer = 0; layer < num_layers; ++layer) {
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = tbuf;
        src.offset          = layer * LAYER_BYTES;
        src.pixels_per_row  = TEX_W;
        src.rows_per_layer  = TEX_H;

        SDL_GPUTextureRegion dst{};
        dst.texture   = m_tile_array;
        dst.mip_level = 0;
        dst.layer     = layer;
        dst.x = dst.y = dst.z = 0;
        dst.w = TEX_W;
        dst.h = TEX_H;
        dst.d = 1;

        SDL_UploadToGPUTexture(copy_pass, &src, &dst, false);
    }

    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(m_gpu, tbuf);

    // ── Create sampler (nearest / repeat for pixel-art look) ──────────────────
    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter      = SDL_GPU_FILTER_NEAREST;
    sci.mag_filter      = SDL_GPU_FILTER_NEAREST;
    sci.mipmap_mode     = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sci.address_mode_u  = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sci.address_mode_v  = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sci.address_mode_w  = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    m_tile_sampler = SDL_CreateGPUSampler(m_gpu, &sci);
    if (!m_tile_sampler) {
        SDL_Log("load_tile_textures: SDL_CreateGPUSampler failed: %s", SDL_GetError());
        return false;
    }

    SDL_Log("load_tile_textures: loaded %u layers (%ux%u each)",
            num_layers, TEX_W, TEX_H);
    return true;
}

bool Renderer::load_item_textures(const ItemRegistry& reg, const char* texture_dir)
{
    const auto& all = reg.all();  // unordered_map<std::string, ItemTypeDef>

    const uint32_t TEX_W      = 32;
    const uint32_t TEX_H      = 32;
    const uint32_t LAYER_BYTES = TEX_W * TEX_H * 4;  // RGBA8

    // Layer 0 = white fallback (items without a texture keep full tint color)
    std::vector<uint8_t> pixels(LAYER_BYTES, 0xFF);

    m_item_tex_idx.clear();
    uint32_t next_layer = 1;

    struct PendingLayer { std::string path; uint32_t layer; };
    std::vector<PendingLayer> pending;
    pending.reserve(all.size());

    for (const auto& [id, def] : all) {
        if (def.icon.empty()) { m_item_tex_idx[id] = 0; continue; }
        m_item_tex_idx[id] = next_layer;
        pending.push_back({ std::string(texture_dir) + "/" + def.icon + ".png", next_layer });
        ++next_layer;
    }

    const uint32_t num_layers = next_layer;
    pixels.resize(num_layers * LAYER_BYTES, 0xFF);

    auto load_layer = [&](uint32_t layer, const std::string& path) {
        int w, h, ch;
        unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
        if (!data) { SDL_Log("load_item_textures: stbi_load failed for %s", path.c_str()); return; }
        uint8_t* dst = pixels.data() + layer * LAYER_BYTES;
        if (w == (int)TEX_W && h == (int)TEX_H) {
            std::memcpy(dst, data, LAYER_BYTES);
        } else {
            for (uint32_t py = 0; py < TEX_H; ++py)
            for (uint32_t px = 0; px < TEX_W; ++px) {
                int sx = (int)(px * w / TEX_W);
                int sy = (int)(py * h / TEX_H);
                const unsigned char* sp = data + (sy * w + sx) * 4;
                uint8_t* d = dst + (py * TEX_W + px) * 4;
                d[0]=sp[0]; d[1]=sp[1]; d[2]=sp[2]; d[3]=sp[3];
            }
        }
        stbi_image_free(data);
    };

    for (auto& pl : pending) load_layer(pl.layer, pl.path);

    SDL_GPUTextureCreateInfo tci{};
    tci.type                 = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    tci.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width                = TEX_W;
    tci.height               = TEX_H;
    tci.layer_count_or_depth = num_layers;
    tci.num_levels           = 1;
    tci.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    m_item_tex_array = SDL_CreateGPUTexture(m_gpu, &tci);
    if (!m_item_tex_array) {
        SDL_Log("load_item_textures: SDL_CreateGPUTexture failed: %s", SDL_GetError());
        return false;
    }

    const Uint32 total_bytes = (Uint32)pixels.size();
    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size  = total_bytes;
    auto* tbuf = SDL_CreateGPUTransferBuffer(m_gpu, &tbci);
    if (!tbuf) { SDL_Log("load_item_textures: transfer buf failed"); return false; }
    auto* ptr = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(m_gpu, tbuf, false));
    std::memcpy(ptr, pixels.data(), total_bytes);
    SDL_UnmapGPUTransferBuffer(m_gpu, tbuf);

    auto* cmd       = SDL_AcquireGPUCommandBuffer(m_gpu);
    auto* copy_pass = SDL_BeginGPUCopyPass(cmd);
    for (uint32_t layer = 0; layer < num_layers; ++layer) {
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = tbuf;
        src.offset          = layer * LAYER_BYTES;
        src.pixels_per_row  = TEX_W;
        src.rows_per_layer  = TEX_H;
        SDL_GPUTextureRegion dst{};
        dst.texture   = m_item_tex_array;
        dst.mip_level = 0;
        dst.layer     = layer;
        dst.x = dst.y = dst.z = 0;
        dst.w = TEX_W; dst.h = TEX_H; dst.d = 1;
        SDL_UploadToGPUTexture(copy_pass, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(m_gpu, tbuf);

    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter     = SDL_GPU_FILTER_NEAREST;
    sci.mag_filter     = SDL_GPU_FILTER_NEAREST;
    sci.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    m_item_sampler = SDL_CreateGPUSampler(m_gpu, &sci);
    if (!m_item_sampler) {
        SDL_Log("load_item_textures: SDL_CreateGPUSampler failed: %s", SDL_GetError());
        return false;
    }

    SDL_Log("load_item_textures: loaded %u layers (%ux%u each)", num_layers, TEX_W, TEX_H);
    return true;
}

// ── Face highlight ────────────────────────────────────────────────────────────

// Quad corners per face direction (matches chunk_mesher FaceGeo, CCW winding).
static const float k_face_quad[6][4][3] = {
    { {1,0,1},{1,0,0},{1,1,0},{1,1,1} },  // PosX
    { {0,1,1},{0,1,0},{0,0,0},{0,0,1} },  // NegX
    { {0,1,1},{1,1,1},{1,1,0},{0,1,0} },  // PosY
    { {0,0,0},{1,0,0},{1,0,1},{0,0,1} },  // NegY
    { {0,0,1},{1,0,1},{1,1,1},{0,1,1} },  // PosZ
    { {1,0,0},{0,0,0},{0,1,0},{1,1,0} },  // NegZ
};
static const float k_face_nrm[6][3] = {
    { 1,0,0},{-1,0,0},{ 0,1,0},{ 0,-1,0},{ 0,0,1},{ 0,0,-1}
};

void Renderer::queue_highlight(const RayHit& hit)
{
    if (!hit.valid) { m_hl_pending = false; m_hl_valid = false; return; }

    int   dir = static_cast<int>(hit.face);
    float ox  = float(hit.voxel.x);
    float oy  = float(hit.voxel.y);
    float oz  = float(hit.voxel.z);
    static constexpr float kEps = 0.004f;
    float nx  = k_face_nrm[dir][0] * kEps;
    float ny  = k_face_nrm[dir][1] * kEps;
    float nz  = k_face_nrm[dir][2] * kEps;
    for (int vi = 0; vi < 4; ++vi) {
        m_hl_verts[vi * 3 + 0] = ox + k_face_quad[dir][vi][0] + nx;
        m_hl_verts[vi * 3 + 1] = oy + k_face_quad[dir][vi][1] + ny;
        m_hl_verts[vi * 3 + 2] = oz + k_face_quad[dir][vi][2] + nz;
    }
    m_hl_pending = true;
}

void Renderer::upload_highlight_geometry()
{
    if (!m_hl_pending || !m_highlight_vbuf || !m_highlight_ibuf || !m_cmd_buf) return;

    const Uint32 vsize = 4 * 3 * sizeof(float);
    const Uint32 isize = 6 * sizeof(uint32_t);
    static const uint32_t k_idx[6] = {0, 1, 2, 0, 2, 3};

    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size  = vsize + isize;
    auto* tbuf = SDL_CreateGPUTransferBuffer(m_gpu, &tbi);
    if (!tbuf) return;

    auto* ptr = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(m_gpu, tbuf, false));
    std::memcpy(ptr,         m_hl_verts, vsize);
    std::memcpy(ptr + vsize, k_idx,      isize);
    SDL_UnmapGPUTransferBuffer(m_gpu, tbuf);

    auto* copy_pass = SDL_BeginGPUCopyPass(m_cmd_buf);
    SDL_GPUTransferBufferLocation sv{ tbuf, 0 };
    SDL_GPUBufferRegion           dv{ m_highlight_vbuf, 0, vsize };
    SDL_UploadToGPUBuffer(copy_pass, &sv, &dv, false);
    SDL_GPUTransferBufferLocation si{ tbuf, vsize };
    SDL_GPUBufferRegion           di{ m_highlight_ibuf, 0, isize };
    SDL_UploadToGPUBuffer(copy_pass, &si, &di, false);
    SDL_EndGPUCopyPass(copy_pass);
    SDL_ReleaseGPUTransferBuffer(m_gpu, tbuf);

    m_hl_pending = false;
    m_hl_valid   = true;
}

bool Renderer::create_highlight_pipeline()
{
    // ── Pre-allocate VBO / IBO ─────────────────────────────────────────────────

    SDL_GPUBufferCreateInfo vbi{};
    vbi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vbi.size  = 4 * 3 * sizeof(float);
    m_highlight_vbuf = SDL_CreateGPUBuffer(m_gpu, &vbi);

    SDL_GPUBufferCreateInfo ibi{};
    ibi.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    ibi.size  = 6 * sizeof(uint32_t);
    m_highlight_ibuf = SDL_CreateGPUBuffer(m_gpu, &ibi);

    if (!m_highlight_vbuf || !m_highlight_ibuf) {
        SDL_Log("create_highlight_pipeline: buffer alloc failed: %s", SDL_GetError());
        return false;
    }

    // ── Shaders ─────────────────────────────────────────────────────────────
    SDL_GPUShaderCreateInfo vi{};
    vi.code      = reinterpret_cast<const Uint8*>(k_highlight_vert_spv);
    vi.code_size = k_highlight_vert_spv_size;
    vi.entrypoint = "main";
    vi.format    = SDL_GPU_SHADERFORMAT_SPIRV;
    vi.stage     = SDL_GPU_SHADERSTAGE_VERTEX;
    vi.num_uniform_buffers = 1;   // slot 0 = MVP
    auto* hl_vert = SDL_CreateGPUShader(m_gpu, &vi);
    if (!hl_vert) { SDL_Log("highlight vert shader: %s", SDL_GetError()); return false; }

    SDL_GPUShaderCreateInfo fi{};
    fi.code      = reinterpret_cast<const Uint8*>(k_highlight_frag_spv);
    fi.code_size = k_highlight_frag_spv_size;
    fi.entrypoint = "main";
    fi.format    = SDL_GPU_SHADERFORMAT_SPIRV;
    fi.stage     = SDL_GPU_SHADERSTAGE_FRAGMENT;
    auto* hl_frag = SDL_CreateGPUShader(m_gpu, &fi);
    if (!hl_frag) {
        SDL_ReleaseGPUShader(m_gpu, hl_vert);
        SDL_Log("highlight frag shader: %s", SDL_GetError());
        return false;
    }

    // ── Vertex layout: pos(3f) only, stride 12 ────────────────────────────────
    SDL_GPUVertexBufferDescription vbuf_desc{};
    vbuf_desc.slot               = 0;
    vbuf_desc.pitch              = 12;
    vbuf_desc.input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vbuf_desc.instance_step_rate = 0;
    SDL_GPUVertexAttribute vattr{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 };

    // ── Alpha-blended colour target ───────────────────────────────────────────
    SDL_GPUColorTargetBlendState blend{};
    blend.enable_blend          = true;
    blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.color_blend_op        = SDL_GPU_BLENDOP_ADD;
    blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    blend.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

    SDL_GPUColorTargetDescription ctd{};
    ctd.format = SDL_GetGPUSwapchainTextureFormat(m_gpu, m_window);
    if (ctd.format == SDL_GPU_TEXTUREFORMAT_INVALID) {
        SDL_ReleaseGPUShader(m_gpu, hl_vert);
        SDL_ReleaseGPUShader(m_gpu, hl_frag);
        SDL_Log("create_highlight_pipeline: invalid swapchain format");
        return false;
    }
    ctd.blend_state = blend;

    // ── Build pipeline ────────────────────────────────────────────────────────
    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader   = hl_vert;
    pci.fragment_shader = hl_frag;
    pci.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
    pci.vertex_input_state.num_vertex_buffers         = 1;
    pci.vertex_input_state.vertex_attributes          = &vattr;
    pci.vertex_input_state.num_vertex_attributes      = 1;
    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pci.rasterizer_state.fill_mode  = SDL_GPU_FILLMODE_FILL;
    pci.rasterizer_state.cull_mode  = SDL_GPU_CULLMODE_NONE;
    pci.depth_stencil_state.enable_depth_test  = true;
    pci.depth_stencil_state.enable_depth_write = false;
    pci.depth_stencil_state.compare_op         = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    pci.target_info.color_target_descriptions  = &ctd;
    pci.target_info.num_color_targets          = 1;
    pci.target_info.has_depth_stencil_target   = true;
    pci.target_info.depth_stencil_format       = m_depth_fmt;

    SDL_Log("Renderer: highlight pipeline created");
    m_highlight_pipeline = SDL_CreateGPUGraphicsPipeline(m_gpu, &pci);
    SDL_ReleaseGPUShader(m_gpu, hl_vert);
    SDL_ReleaseGPUShader(m_gpu, hl_frag);

    if (!m_highlight_pipeline) {
        SDL_Log("highlight pipeline failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

// ── Frustum culling ───────────────────────────────────────────────────────────

// Returns true if AABB [mn, mx] intersects the view frustum defined by mvp.
// Uses Gribb/Hartmann plane extraction (GLM stores mat4 in column-major order).
bool Renderer::aabb_in_frustum(const glm::mat4& m, glm::vec3 mn, glm::vec3 mx)
{
    for (int i = 0; i < 3; ++i)
    for (int sign : {+1, -1}) {
        float s = float(sign);
        float a = m[0][3] + s * m[0][i];
        float b = m[1][3] + s * m[1][i];
        float c = m[2][3] + s * m[2][i];
        float d = m[3][3] + s * m[3][i];
        bool any_in = false;
        for (int cx = 0; cx < 2 && !any_in; ++cx)
        for (int cy = 0; cy < 2 && !any_in; ++cy)
        for (int cz = 0; cz < 2 && !any_in; ++cz) {
            float x = cx ? mx.x : mn.x;
            float y = cy ? mx.y : mn.y;
            float z = cz ? mx.z : mn.z;
            if (a*x + b*y + c*z + d >= 0.f) any_in = true;
        }
        if (!any_in) return false;
    }
    return true;
}

// ── Item pipeline ─────────────────────────────────────────────────────────────

bool Renderer::create_item_pipeline()
{
    // Pre-allocate GPU buffers for up to k_max_item_quads quads
    // Vertex: pos(3f) + color(4f) = 28 bytes × 4 verts × max_quads
    const Uint32 vbuf_sz = k_max_item_quads * 4 * sizeof(ItemVert);
    const Uint32 ibuf_sz = k_max_item_quads * 6 * sizeof(uint32_t);

    SDL_GPUBufferCreateInfo vbi{};
    vbi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vbi.size  = vbuf_sz;
    m_item_vbuf = SDL_CreateGPUBuffer(m_gpu, &vbi);

    SDL_GPUBufferCreateInfo ibi{};
    ibi.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    ibi.size  = ibuf_sz;
    m_item_ibuf = SDL_CreateGPUBuffer(m_gpu, &ibi);

    if (!m_item_vbuf || !m_item_ibuf) {
        SDL_Log("create_item_pipeline: buffer alloc failed: %s", SDL_GetError());
        return false;
    }

    // ── Shaders ───────────────────────────────────────────────────────────────
    SDL_GPUShaderCreateInfo vi{};
    vi.code               = reinterpret_cast<const Uint8*>(k_item_vert_spv);
    vi.code_size          = k_item_vert_spv_size;
    vi.entrypoint         = "main";
    vi.format             = SDL_GPU_SHADERFORMAT_SPIRV;
    vi.stage              = SDL_GPU_SHADERSTAGE_VERTEX;
    vi.num_uniform_buffers = 1;
    auto* it_vert = SDL_CreateGPUShader(m_gpu, &vi);
    if (!it_vert) { SDL_Log("item vert shader: %s", SDL_GetError()); return false; }

    SDL_GPUShaderCreateInfo fi{};
    fi.code         = reinterpret_cast<const Uint8*>(k_item_frag_spv);
    fi.code_size    = k_item_frag_spv_size;
    fi.entrypoint   = "main";
    fi.format       = SDL_GPU_SHADERFORMAT_SPIRV;
    fi.stage        = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fi.num_samplers = 1;   // slot 0 = item texture array (set=2, binding=0)
    auto* it_frag = SDL_CreateGPUShader(m_gpu, &fi);
    if (!it_frag) {
        SDL_ReleaseGPUShader(m_gpu, it_vert);
        SDL_Log("item frag shader: %s", SDL_GetError());
        return false;
    }

    // ── Vertex layout: pos(3f, offset 0) + color(4f, offset 12) + uv(2f, offset 28) + texIdx(1f, offset 36), stride 40 ──
    SDL_GPUVertexBufferDescription vbuf_desc{};
    vbuf_desc.slot               = 0;
    vbuf_desc.pitch              = sizeof(ItemVert);   // 40 bytes
    vbuf_desc.input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vbuf_desc.instance_step_rate = 0;

    SDL_GPUVertexAttribute vattrs[4]{};
    vattrs[0] = { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,  0  }; // pos
    vattrs[1] = { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 12  }; // color
    vattrs[2] = { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 28  }; // uv
    vattrs[3] = { 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT,  36  }; // texIdx

    // ── Alpha-blended colour target ───────────────────────────────────────────
    SDL_GPUColorTargetBlendState blend{};
    blend.enable_blend          = true;
    blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.color_blend_op        = SDL_GPU_BLENDOP_ADD;
    blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    blend.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

    SDL_GPUColorTargetDescription ctd{};
    ctd.format = SDL_GetGPUSwapchainTextureFormat(m_gpu, m_window);
    if (ctd.format == SDL_GPU_TEXTUREFORMAT_INVALID) {
        SDL_ReleaseGPUShader(m_gpu, it_vert);
        SDL_ReleaseGPUShader(m_gpu, it_frag);
        SDL_Log("create_item_pipeline: invalid swapchain format");
        return false;
    }
    ctd.blend_state = blend;

    // ── Pipeline ──────────────────────────────────────────────────────────────
    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader   = it_vert;
    pci.fragment_shader = it_frag;
    pci.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
    pci.vertex_input_state.num_vertex_buffers         = 1;
    pci.vertex_input_state.vertex_attributes          = vattrs;
    pci.vertex_input_state.num_vertex_attributes      = 4;
    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  // flat quads are double-sided
    // Depth-test ON but depth-write OFF (items sort behind walls but don't occlude each other)
    pci.depth_stencil_state.enable_depth_test  = true;
    pci.depth_stencil_state.enable_depth_write = false;
    pci.depth_stencil_state.compare_op         = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    pci.target_info.color_target_descriptions  = &ctd;
    pci.target_info.num_color_targets          = 1;
    pci.target_info.has_depth_stencil_target   = true;
    pci.target_info.depth_stencil_format       = m_depth_fmt;

    SDL_Log("Renderer: item pipeline created");
    m_item_pipeline = SDL_CreateGPUGraphicsPipeline(m_gpu, &pci);
    SDL_ReleaseGPUShader(m_gpu, it_vert);
    SDL_ReleaseGPUShader(m_gpu, it_frag);

    if (!m_item_pipeline) {
        SDL_Log("item pipeline failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

// ── queue_world_items ─────────────────────────────────────────────────────────

void Renderer::queue_world_items(EntityManager& entities, EntityID hovered_item,
                                  glm::vec3 cam_pos, float yaw, float pitch)
{
    m_item_verts.clear();
    m_item_indices.clear();
    m_item_pending = false;

    // Camera basis for billboard items
    float yaw_r   = glm::radians(yaw);
    float pitch_r = glm::radians(pitch);
    glm::vec3 fwd = {
        std::cos(pitch_r) * std::sin(yaw_r),
        std::sin(pitch_r),
       -std::cos(pitch_r) * std::cos(yaw_r)
    };
    glm::vec3 cam_right = glm::normalize(glm::cross(fwd, {0.f, 1.f, 0.f}));
    glm::vec3 cam_up    = glm::normalize(glm::cross(cam_right, fwd));

    // Per-face tangent / bitangent for flat placement
    static const glm::vec3 face_t[6] = {
        {0,0,-1},{0,0, 1},  // PosX, NegX
        {1,0, 0},{1,0, 0},  // PosY, NegY
        {1,0, 0},{-1,0,0},  // PosZ, NegZ
    };
    static const glm::vec3 face_b[6] = {
        {0,1,0},{0,1,0},
        {0,0,1},{0,0,1},
        {0,1,0},{0,1, 0},
    };

    static constexpr float HALF     = 0.22f;  // half-size of item quad in metres
    static constexpr float REST_EPS = 0.015f; // float above surface

    entities.each<WorldItemComponent>([&](EntityID eid, WorldItemComponent& wic) {
        if (m_item_verts.size() / 4 >= k_max_item_quads) return;

        auto* tr = entities.get_component<TransformComponent>(eid);
        if (!tr) return;

        glm::vec3 center = tr->pos;

        (void)hovered_item;
        glm::vec4 col = glm::vec4(1.f, 1.f, 1.f, 1.f);  // no tint, texture provides color

        glm::vec3 corners[4];
        if (wic.is_resting) {
            int fi = static_cast<int>(wic.rest_face);
            glm::vec3 t = face_t[fi];
            glm::vec3 b = face_b[fi];
            // Lift slightly off the surface
            glm::vec3 n = glm::vec3(face_normal(wic.rest_face)) * REST_EPS;
            corners[0] = center + n + (-t - b) * HALF;
            corners[1] = center + n + ( t - b) * HALF;
            corners[2] = center + n + ( t + b) * HALF;
            corners[3] = center + n + (-t + b) * HALF;
        } else {
            // Billboard — four corners computed from camera orientation
            corners[0] = center + (-cam_right - cam_up) * HALF;
            corners[1] = center + ( cam_right - cam_up) * HALF;
            corners[2] = center + ( cam_right + cam_up) * HALF;
            corners[3] = center + (-cam_right + cam_up) * HALF;
        }

        // Discard items behind the camera
        glm::vec3 to_item = center - cam_pos;
        if (glm::dot(to_item, fwd) < -0.5f) return;

        // Texture layer lookup
        uint32_t tex_layer = 0;
        if (wic.item.def) {
            auto it = m_item_tex_idx.find(wic.item.def->id);
            if (it != m_item_tex_idx.end()) tex_layer = it->second;
        }
        static const float k_uv[4][2] = {{0,0},{1,0},{1,1},{0,1}};

        auto base = static_cast<uint32_t>(m_item_verts.size());
        for (int i = 0; i < 4; ++i) {
            auto& c = corners[i];
            m_item_verts.push_back({c.x, c.y, c.z, col.r, col.g, col.b, col.a,
                                    k_uv[i][0], k_uv[i][1], static_cast<float>(tex_layer)});
        }

        m_item_indices.push_back(base+0); m_item_indices.push_back(base+1);
        m_item_indices.push_back(base+2); m_item_indices.push_back(base+0);
        m_item_indices.push_back(base+2); m_item_indices.push_back(base+3);
    });

    m_item_pending = !m_item_verts.empty();
}

void Renderer::upload_item_geometry()
{
    if (!m_item_pending || m_item_verts.empty() || !m_item_vbuf || !m_item_ibuf || !m_cmd_buf)
        return;

    const Uint32 vsize = static_cast<Uint32>(m_item_verts.size()   * sizeof(ItemVert));
    const Uint32 isize = static_cast<Uint32>(m_item_indices.size() * sizeof(uint32_t));

    // Guard against oversized batches
    const Uint32 vbuf_cap = k_max_item_quads * 4 * sizeof(ItemVert);
    const Uint32 ibuf_cap = k_max_item_quads * 6 * sizeof(uint32_t);
    if (vsize > vbuf_cap || isize > ibuf_cap) return;

    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size  = vsize + isize;
    auto* tbuf = SDL_CreateGPUTransferBuffer(m_gpu, &tbi);
    if (!tbuf) return;

    auto* ptr = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(m_gpu, tbuf, false));
    std::memcpy(ptr,         m_item_verts.data(),   vsize);
    std::memcpy(ptr + vsize, m_item_indices.data(), isize);
    SDL_UnmapGPUTransferBuffer(m_gpu, tbuf);

    auto* copy_pass = SDL_BeginGPUCopyPass(m_cmd_buf);
    SDL_GPUTransferBufferLocation sv{ tbuf, 0 };
    SDL_GPUBufferRegion           dv{ m_item_vbuf, 0, vsize };
    SDL_UploadToGPUBuffer(copy_pass, &sv, &dv, false);
    SDL_GPUTransferBufferLocation si{ tbuf, vsize };
    SDL_GPUBufferRegion           di{ m_item_ibuf, 0, isize };
    SDL_UploadToGPUBuffer(copy_pass, &si, &di, false);
    SDL_EndGPUCopyPass(copy_pass);
    SDL_ReleaseGPUTransferBuffer(m_gpu, tbuf);
}

void Renderer::draw_world_items()
{
    if (m_item_indices.empty() || !m_item_pipeline || !m_render_pass) return;

    SDL_BindGPUGraphicsPipeline(m_render_pass, m_item_pipeline);
    SDL_PushGPUVertexUniformData(m_cmd_buf, 0, &m_current_mvp[0][0], sizeof(glm::mat4));
    if (m_item_tex_array && m_item_sampler) {
        SDL_GPUTextureSamplerBinding tsb{ m_item_tex_array, m_item_sampler };
        SDL_BindGPUFragmentSamplers(m_render_pass, 0, &tsb, 1);
    }
    SDL_GPUBufferBinding vb{ m_item_vbuf, 0 };
    SDL_GPUBufferBinding ib{ m_item_ibuf, 0 };
    SDL_BindGPUVertexBuffers(m_render_pass, 0, &vb, 1);
    SDL_BindGPUIndexBuffer(m_render_pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(m_render_pass,
                                  static_cast<uint32_t>(m_item_indices.size()),
                                  1, 0, 0, 0);
}

// ── Mob sprite system ─────────────────────────────────────────────────────────

bool Renderer::create_mob_buffers()
{
    const Uint32 vbuf_sz = k_max_mob_quads * 4 * sizeof(ItemVert);
    const Uint32 ibuf_sz = k_max_mob_quads * 6 * sizeof(uint32_t);

    SDL_GPUBufferCreateInfo vbi{};
    vbi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vbi.size  = vbuf_sz;
    m_mob_vbuf = SDL_CreateGPUBuffer(m_gpu, &vbi);

    SDL_GPUBufferCreateInfo ibi{};
    ibi.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    ibi.size  = ibuf_sz;
    m_mob_ibuf = SDL_CreateGPUBuffer(m_gpu, &ibi);

    if (!m_mob_vbuf || !m_mob_ibuf) {
        SDL_Log("create_mob_buffers: buffer alloc failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

bool Renderer::load_mob_textures(const char* texture_dir)
{
    namespace fs = std::filesystem;

    // Mob sprites are taller than items; render at this resolution.
    const uint32_t TEX_W      = 64;
    const uint32_t TEX_H      = 128;
    const uint32_t LAYER_BYTES = TEX_W * TEX_H * 4;  // RGBA8

    // Direction names (indices: 0=front 1=back 2=left 3=right)
    static const char* const k_dir_keys[] = { "front", "back", "left", "right" };

    // Reserve layer 0 as white fallback
    std::vector<uint8_t> pixels(LAYER_BYTES, 0xFF);
    uint32_t next_layer = 1;

    m_mob_sprites.clear();

    std::string mobs_dir = std::string(texture_dir) + "/mobs";
    if (!fs::exists(mobs_dir)) {
        SDL_Log("load_mob_textures: directory not found: %s", mobs_dir.c_str());
        return true;  // non-fatal
    }

    // Helper to check if filename (lowercase) contains dir_key
    auto filename_has = [](const fs::path& p, const char* key) -> bool {
        std::string name = p.filename().string();
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        return name.find(key) != std::string::npos;
    };

    for (const auto& species_entry : fs::directory_iterator(mobs_dir)) {
        if (!species_entry.is_directory()) continue;
        std::string species = species_entry.path().filename().string();

        for (const auto& variant_entry : fs::directory_iterator(species_entry.path())) {
            if (!variant_entry.is_directory()) continue;
            std::string variant  = variant_entry.path().filename().string();
            std::string key      = species + "/" + variant;

            MobSpriteSet set{};

            for (int d = 0; d < 4; ++d) {
                const char* dir_key = k_dir_keys[d];
                std::string found_path;

                for (const auto& file : fs::directory_iterator(variant_entry.path())) {
                    if (!file.is_regular_file()) continue;
                    std::string ext = file.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext != ".png") continue;
                    if (filename_has(file.path(), dir_key)) {
                        found_path = file.path().string();
                        break;
                    }
                }

                if (found_path.empty()) {
                    set.layer[d] = 0;  // fallback
                    continue;
                }

                int w, h, ch;
                unsigned char* data = stbi_load(found_path.c_str(), &w, &h, &ch, 4);
                if (!data) {
                    SDL_Log("load_mob_textures: stbi_load failed for %s", found_path.c_str());
                    set.layer[d] = 0;
                    continue;
                }

                set.layer[d] = next_layer;
                pixels.resize((next_layer + 1) * LAYER_BYTES, 0xFF);
                uint8_t* dst = pixels.data() + next_layer * LAYER_BYTES;

                if (w == (int)TEX_W && h == (int)TEX_H) {
                    std::memcpy(dst, data, LAYER_BYTES);
                } else {
                    for (uint32_t py = 0; py < TEX_H; ++py)
                    for (uint32_t px = 0; px < TEX_W; ++px) {
                        int sx = (int)(px * w / TEX_W);
                        int sy = (int)(py * h / TEX_H);
                        const uint8_t* sp = data + (sy * w + sx) * 4;
                        uint8_t* dp       = dst  + (py * TEX_W + px) * 4;
                        dp[0]=sp[0]; dp[1]=sp[1]; dp[2]=sp[2]; dp[3]=sp[3];
                    }
                }
                stbi_image_free(data);
                ++next_layer;
            }

            m_mob_sprites[key] = set;
            SDL_Log("load_mob_textures: loaded %s (layers %u/%u/%u/%u)",
                    key.c_str(),
                    set.layer[0], set.layer[1], set.layer[2], set.layer[3]);
        }
    }

    m_mob_tex_layers = next_layer;
    if (next_layer == 1) {
        SDL_Log("load_mob_textures: no sprites found under %s", mobs_dir.c_str());
        return true;  // not fatal
    }

    // Upload to GPU 2D-array texture
    SDL_GPUTextureCreateInfo tci{};
    tci.type                 = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    tci.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width                = TEX_W;
    tci.height               = TEX_H;
    tci.layer_count_or_depth = next_layer;
    tci.num_levels           = 1;
    tci.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    m_mob_tex_array = SDL_CreateGPUTexture(m_gpu, &tci);
    if (!m_mob_tex_array) {
        SDL_Log("load_mob_textures: SDL_CreateGPUTexture failed: %s", SDL_GetError());
        return false;
    }

    const Uint32 total_bytes = (Uint32)pixels.size();
    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size  = total_bytes;
    auto* tbuf = SDL_CreateGPUTransferBuffer(m_gpu, &tbci);
    if (!tbuf) { SDL_Log("load_mob_textures: transfer buf failed"); return false; }

    auto* ptr = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(m_gpu, tbuf, false));
    std::memcpy(ptr, pixels.data(), total_bytes);
    SDL_UnmapGPUTransferBuffer(m_gpu, tbuf);

    auto* cmd       = SDL_AcquireGPUCommandBuffer(m_gpu);
    auto* copy_pass = SDL_BeginGPUCopyPass(cmd);
    for (uint32_t layer = 0; layer < next_layer; ++layer) {
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = tbuf;
        src.offset          = layer * LAYER_BYTES;
        src.pixels_per_row  = TEX_W;
        src.rows_per_layer  = TEX_H;
        SDL_GPUTextureRegion dst{};
        dst.texture   = m_mob_tex_array;
        dst.mip_level = 0;
        dst.layer     = layer;
        dst.x = dst.y = dst.z = 0;
        dst.w = TEX_W; dst.h = TEX_H; dst.d = 1;
        SDL_UploadToGPUTexture(copy_pass, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(m_gpu, tbuf);

    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter     = SDL_GPU_FILTER_NEAREST;
    sci.mag_filter     = SDL_GPU_FILTER_NEAREST;
    sci.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    m_mob_sampler = SDL_CreateGPUSampler(m_gpu, &sci);
    if (!m_mob_sampler) {
        SDL_Log("load_mob_textures: SDL_CreateGPUSampler failed: %s", SDL_GetError());
        return false;
    }

    SDL_Log("load_mob_textures: uploaded %u layers (%ux%u each)", next_layer, TEX_W, TEX_H);
    return true;
}

// Select sprite direction index (0=front 1=back 2=left 3=right)
// based on horizontal angle from mob's forward direction to the camera.
static int mob_sprite_dir(glm::vec3 mob_pos, float mob_yaw_deg, glm::vec3 cam_pos)
{
    glm::vec2 to_cam_xz = { cam_pos.x - mob_pos.x, cam_pos.z - mob_pos.z };
    float len = glm::length(to_cam_xz);
    if (len < 0.001f) return 0;  // camera on top of mob — show front
    to_cam_xz /= len;

    float yaw_rad = glm::radians(mob_yaw_deg);
    // Same convention as camera forward: (sin(yaw), -cos(yaw)) in XZ
    glm::vec2 mob_fwd = { std::sin(yaw_rad), -std::cos(yaw_rad) };

    // Signed angle from mob_fwd to to_cam using 2D cross/dot
    float cross_z = mob_fwd.x * to_cam_xz.y - mob_fwd.y * to_cam_xz.x;
    float dot     = mob_fwd.x * to_cam_xz.x + mob_fwd.y * to_cam_xz.y;
    float angle   = std::atan2(cross_z, dot);  // [-PI, PI]

    // Sectors (each 90°):
    //   |angle| < PI/4        → front (camera in front of mob)
    //   angle in [PI/4, 3PI/4]  → right
    //   |angle| > 3PI/4       → back
    //   angle in [-3PI/4,-PI/4] → left
    static constexpr float SECTOR = glm::pi<float>() / 4.f;
    float abs_a = std::abs(angle);
    if (abs_a < SECTOR)              return 0;  // front
    if (abs_a > 3.f * SECTOR)       return 1;  // back
    if (angle > 0.f)                 return 3;  // right
    return 2;                                   // left
}

void Renderer::queue_mobs(EntityManager& entities, glm::vec3 cam_pos, float cam_yaw)
{
    m_mob_verts.clear();
    m_mob_indices.clear();
    m_mob_pending = false;

    if (!m_mob_tex_array) return;  // textures not loaded yet

    // Camera forward (XZ only) for back-face culling
    float yaw_r = glm::radians(cam_yaw);
    glm::vec3 cam_fwd = { std::sin(yaw_r), 0.f, -std::cos(yaw_r) };

    static constexpr float MOB_HALF_W = 0.3f;   // half-width  matches player radius
    static constexpr float MOB_HEIGHT = .65f;   // full height matches player height

    entities.each<MobComponent>([&](EntityID eid, MobComponent& mob) {
        if (m_mob_verts.size() / 4 >= k_max_mob_quads) return;

        auto* tr = entities.get_component<TransformComponent>(eid);
        if (!tr) return;

        glm::vec3 feet = tr->pos;

        // Discard mobs behind the camera
        glm::vec3 to_mob = feet - cam_pos;
        if (glm::dot(to_mob, cam_fwd) < -0.5f) return;

        // Choose sprite direction
        std::string key = mob.species + "/" + mob.variant;
        auto it = m_mob_sprites.find(key);
        uint32_t tex_layer = 0;
        if (it != m_mob_sprites.end()) {
            int dir = mob_sprite_dir(feet, tr->yaw, cam_pos);
            tex_layer = it->second.layer[dir];
        }

        // Cylindrical billboard: vertical plane facing camera horizontally
        glm::vec3 to_cam_xz = glm::vec3(cam_pos.x - feet.x, 0.f, cam_pos.z - feet.z);
        float xz_len = glm::length(to_cam_xz);
        glm::vec3 right;
        if (xz_len > 0.001f) {
            glm::vec3 to_cam_dir = to_cam_xz / xz_len;
            right = glm::cross({0.f, 1.f, 0.f}, to_cam_dir);
        } else {
            // Camera directly above/below — use world right
            right = { 1.f, 0.f, 0.f };
        }

        glm::vec3 corners[4];
        corners[0] = feet + (-right) * MOB_HALF_W;                      // bottom-left
        corners[1] = feet + ( right) * MOB_HALF_W;                      // bottom-right
        corners[2] = feet + ( right) * MOB_HALF_W + glm::vec3(0, MOB_HEIGHT, 0); // top-right
        corners[3] = feet + (-right) * MOB_HALF_W + glm::vec3(0, MOB_HEIGHT, 0); // top-left

        static const float k_uv[4][2] = {{0,1},{1,1},{1,0},{0,0}};  // V flipped (Y-down textures)

        auto base = static_cast<uint32_t>(m_mob_verts.size());
        for (int i = 0; i < 4; ++i) {
            auto& c = corners[i];
            m_mob_verts.push_back({ c.x, c.y, c.z,
                                    1.f, 1.f, 1.f, 1.f,
                                    k_uv[i][0], k_uv[i][1],
                                    static_cast<float>(tex_layer) });
        }
        m_mob_indices.push_back(base+0); m_mob_indices.push_back(base+1);
        m_mob_indices.push_back(base+2); m_mob_indices.push_back(base+0);
        m_mob_indices.push_back(base+2); m_mob_indices.push_back(base+3);
    });

    m_mob_pending = !m_mob_verts.empty();
}

void Renderer::upload_mob_geometry()
{
    if (!m_mob_pending || m_mob_verts.empty() || !m_mob_vbuf || !m_mob_ibuf || !m_cmd_buf)
        return;

    const Uint32 vsize = static_cast<Uint32>(m_mob_verts.size()   * sizeof(ItemVert));
    const Uint32 isize = static_cast<Uint32>(m_mob_indices.size() * sizeof(uint32_t));

    const Uint32 vbuf_cap = k_max_mob_quads * 4 * sizeof(ItemVert);
    const Uint32 ibuf_cap = k_max_mob_quads * 6 * sizeof(uint32_t);
    if (vsize > vbuf_cap || isize > ibuf_cap) return;

    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size  = vsize + isize;
    auto* tbuf = SDL_CreateGPUTransferBuffer(m_gpu, &tbi);
    if (!tbuf) return;

    auto* ptr = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(m_gpu, tbuf, false));
    std::memcpy(ptr,         m_mob_verts.data(),   vsize);
    std::memcpy(ptr + vsize, m_mob_indices.data(), isize);
    SDL_UnmapGPUTransferBuffer(m_gpu, tbuf);

    auto* copy_pass = SDL_BeginGPUCopyPass(m_cmd_buf);
    SDL_GPUTransferBufferLocation sv{ tbuf, 0 };
    SDL_GPUBufferRegion           dv{ m_mob_vbuf, 0, vsize };
    SDL_UploadToGPUBuffer(copy_pass, &sv, &dv, false);
    SDL_GPUTransferBufferLocation si{ tbuf, vsize };
    SDL_GPUBufferRegion           di{ m_mob_ibuf, 0, isize };
    SDL_UploadToGPUBuffer(copy_pass, &si, &di, false);
    SDL_EndGPUCopyPass(copy_pass);
    SDL_ReleaseGPUTransferBuffer(m_gpu, tbuf);
}

void Renderer::draw_mobs()
{
    if (m_mob_indices.empty() || !m_item_pipeline || !m_render_pass) return;

    SDL_BindGPUGraphicsPipeline(m_render_pass, m_item_pipeline);
    SDL_PushGPUVertexUniformData(m_cmd_buf, 0, &m_current_mvp[0][0], sizeof(glm::mat4));
    if (m_mob_tex_array && m_mob_sampler) {
        SDL_GPUTextureSamplerBinding tsb{ m_mob_tex_array, m_mob_sampler };
        SDL_BindGPUFragmentSamplers(m_render_pass, 0, &tsb, 1);
    }
    SDL_GPUBufferBinding vb{ m_mob_vbuf, 0 };
    SDL_GPUBufferBinding ib{ m_mob_ibuf, 0 };
    SDL_BindGPUVertexBuffers(m_render_pass, 0, &vb, 1);
    SDL_BindGPUIndexBuffer(m_render_pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(m_render_pass,
                                  static_cast<uint32_t>(m_mob_indices.size()),
                                  1, 0, 0, 0);
}

// ── Space background (animated Earth orbit) ───────────────────────────────────

bool Renderer::create_sky_pipeline()
{
    // Pre-allocate GPU buffers for one Earth quad (4 verts, 6 indices).
    SDL_GPUBufferCreateInfo vbi{};
    vbi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vbi.size  = 4 * sizeof(ItemVert);
    m_sky_vbuf = SDL_CreateGPUBuffer(m_gpu, &vbi);

    SDL_GPUBufferCreateInfo ibi{};
    ibi.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    ibi.size  = 6 * sizeof(uint32_t);
    m_sky_ibuf = SDL_CreateGPUBuffer(m_gpu, &ibi);

    if (!m_sky_vbuf || !m_sky_ibuf) {
        SDL_Log("create_sky_pipeline: buffer alloc failed: %s", SDL_GetError());
        return false;
    }

    // ── Upload static index buffer once ───────────────────────────────────────
    static const uint32_t k_idx[6] = {0, 1, 2, 0, 2, 3};
    {
        SDL_GPUTransferBufferCreateInfo tbci{};
        tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbci.size  = sizeof(k_idx);
        auto* tbuf = SDL_CreateGPUTransferBuffer(m_gpu, &tbci);
        if (tbuf) {
            auto* ptr = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(m_gpu, tbuf, false));
            std::memcpy(ptr, k_idx, sizeof(k_idx));
            SDL_UnmapGPUTransferBuffer(m_gpu, tbuf);
            auto* cmd  = SDL_AcquireGPUCommandBuffer(m_gpu);
            auto* cp   = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTransferBufferLocation src{ tbuf, 0 };
            SDL_GPUBufferRegion           dst{ m_sky_ibuf, 0, sizeof(k_idx) };
            SDL_UploadToGPUBuffer(cp, &src, &dst, false);
            SDL_EndGPUCopyPass(cp);
            SDL_SubmitGPUCommandBuffer(cmd);
            SDL_ReleaseGPUTransferBuffer(m_gpu, tbuf);
        }
    }

    // ── Shaders: reuse item vert + frag (textured quad, 2D-array sampler) ─────
    SDL_GPUShaderCreateInfo vi{};
    vi.code               = reinterpret_cast<const Uint8*>(k_item_vert_spv);
    vi.code_size          = k_item_vert_spv_size;
    vi.entrypoint         = "main";
    vi.format             = SDL_GPU_SHADERFORMAT_SPIRV;
    vi.stage              = SDL_GPU_SHADERSTAGE_VERTEX;
    vi.num_uniform_buffers = 1;
    auto* sk_vert = SDL_CreateGPUShader(m_gpu, &vi);
    if (!sk_vert) { SDL_Log("sky vert shader: %s", SDL_GetError()); return false; }

    SDL_GPUShaderCreateInfo fi{};
    fi.code         = reinterpret_cast<const Uint8*>(k_item_frag_spv);
    fi.code_size    = k_item_frag_spv_size;
    fi.entrypoint   = "main";
    fi.format       = SDL_GPU_SHADERFORMAT_SPIRV;
    fi.stage        = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fi.num_samplers = 1;
    auto* sk_frag = SDL_CreateGPUShader(m_gpu, &fi);
    if (!sk_frag) {
        SDL_ReleaseGPUShader(m_gpu, sk_vert);
        SDL_Log("sky frag shader: %s", SDL_GetError());
        return false;
    }

    // ── Vertex layout: same as ItemVert ───────────────────────────────────────
    SDL_GPUVertexBufferDescription vbuf_desc{};
    vbuf_desc.slot               = 0;
    vbuf_desc.pitch              = sizeof(ItemVert);
    vbuf_desc.input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vbuf_desc.instance_step_rate = 0;

    SDL_GPUVertexAttribute vattrs[4]{};
    vattrs[0] = { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,  0  }; // pos
    vattrs[1] = { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 12  }; // color
    vattrs[2] = { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 28  }; // uv
    vattrs[3] = { 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT,  36  }; // texIdx

    SDL_GPUColorTargetDescription ctd{};
    ctd.format = SDL_GetGPUSwapchainTextureFormat(m_gpu, m_window);
    if (ctd.format == SDL_GPU_TEXTUREFORMAT_INVALID) {
        SDL_ReleaseGPUShader(m_gpu, sk_vert);
        SDL_ReleaseGPUShader(m_gpu, sk_frag);
        SDL_Log("create_sky_pipeline: invalid swapchain format");
        return false;
    }

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader   = sk_vert;
    pci.fragment_shader = sk_frag;
    pci.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
    pci.vertex_input_state.num_vertex_buffers         = 1;
    pci.vertex_input_state.vertex_attributes          = vattrs;
    pci.vertex_input_state.num_vertex_attributes      = 4;
    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    // No depth test or write: always renders as a background layer.
    pci.depth_stencil_state.enable_depth_test  = false;
    pci.depth_stencil_state.enable_depth_write = false;
    pci.target_info.color_target_descriptions  = &ctd;
    pci.target_info.num_color_targets          = 1;
    pci.target_info.has_depth_stencil_target   = true;
    pci.target_info.depth_stencil_format       = m_depth_fmt;

    m_sky_pipeline = SDL_CreateGPUGraphicsPipeline(m_gpu, &pci);
    SDL_ReleaseGPUShader(m_gpu, sk_vert);
    SDL_ReleaseGPUShader(m_gpu, sk_frag);
    if (!m_sky_pipeline) {
        SDL_Log("sky pipeline failed: %s", SDL_GetError());
        return false;
    }
    SDL_Log("Renderer: sky pipeline created");
    return true;
}

bool Renderer::load_earth_gif(const char* path)
{
    // Read the entire GIF file into memory.
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        SDL_Log("load_earth_gif: cannot open '%s' (non-fatal)", path);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long fsize = std::ftell(f);
    std::rewind(f);
    if (fsize <= 0) { std::fclose(f); return false; }

    std::vector<uint8_t> buf(static_cast<size_t>(fsize));
    std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);

    // Decode all frames via stb_image.
    int w = 0, h = 0, frame_count = 0, comp = 0;
    int* delays_raw = nullptr;
    stbi_uc* pixels = stbi_load_gif_from_memory(
        buf.data(), static_cast<int>(buf.size()),
        &delays_raw, &w, &h, &frame_count, &comp, 4);

    if (!pixels || frame_count <= 0 || w <= 0 || h <= 0) {
        SDL_Log("load_earth_gif: stbi_load_gif_from_memory failed for '%s': %s",
                path, stbi_failure_reason());
        if (pixels) stbi_image_free(pixels);
        if (delays_raw) stbi_image_free(delays_raw);
        return false;
    }

    m_earth_num_frames = static_cast<uint32_t>(frame_count);
    m_earth_delays.resize(frame_count);
    for (int i = 0; i < frame_count; ++i)
        m_earth_delays[i] = (delays_raw && delays_raw[i] > 0) ? delays_raw[i] : 100;
    if (delays_raw) stbi_image_free(delays_raw);

    // ── Create GPU 2D-array texture (one layer per frame) ────────────────────
    SDL_GPUTextureCreateInfo tci{};
    tci.type                 = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    tci.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width                = static_cast<Uint32>(w);
    tci.height               = static_cast<Uint32>(h);
    tci.layer_count_or_depth = m_earth_num_frames;
    tci.num_levels           = 1;
    tci.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    m_earth_tex_array = SDL_CreateGPUTexture(m_gpu, &tci);
    if (!m_earth_tex_array) {
        SDL_Log("load_earth_gif: SDL_CreateGPUTexture failed: %s", SDL_GetError());
        stbi_image_free(pixels);
        return false;
    }

    // ── Upload all frames via staging buffer ──────────────────────────────────
    const Uint32 layer_bytes  = static_cast<Uint32>(w * h * 4);
    const Uint32 total_bytes  = layer_bytes * m_earth_num_frames;

    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size  = total_bytes;
    auto* tbuf = SDL_CreateGPUTransferBuffer(m_gpu, &tbci);
    if (!tbuf) {
        SDL_Log("load_earth_gif: transfer buffer failed");
        stbi_image_free(pixels);
        return false;
    }
    auto* ptr = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(m_gpu, tbuf, false));
    std::memcpy(ptr, pixels, total_bytes);
    SDL_UnmapGPUTransferBuffer(m_gpu, tbuf);
    stbi_image_free(pixels);

    auto* cmd       = SDL_AcquireGPUCommandBuffer(m_gpu);
    auto* copy_pass = SDL_BeginGPUCopyPass(cmd);
    for (uint32_t frame = 0; frame < m_earth_num_frames; ++frame) {
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = tbuf;
        src.offset          = frame * layer_bytes;
        src.pixels_per_row  = static_cast<Uint32>(w);
        src.rows_per_layer  = static_cast<Uint32>(h);
        SDL_GPUTextureRegion dst{};
        dst.texture   = m_earth_tex_array;
        dst.mip_level = 0;
        dst.layer     = frame;
        dst.x = dst.y = dst.z = 0;
        dst.w = static_cast<Uint32>(w);
        dst.h = static_cast<Uint32>(h);
        dst.d = 1;
        SDL_UploadToGPUTexture(copy_pass, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(m_gpu, tbuf);

    // ── Create sampler (bilinear for smooth planet look) ─────────────────────
    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter      = SDL_GPU_FILTER_LINEAR;
    sci.mag_filter      = SDL_GPU_FILTER_LINEAR;
    sci.mipmap_mode     = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sci.address_mode_u  = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_v  = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_w  = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    m_earth_sampler = SDL_CreateGPUSampler(m_gpu, &sci);
    if (!m_earth_sampler) {
        SDL_Log("load_earth_gif: SDL_CreateGPUSampler failed: %s", SDL_GetError());
        return false;
    }

    SDL_Log("load_earth_gif: loaded %u frames (%dx%d) from '%s'",
            m_earth_num_frames, w, h, path);
    return true;
}

void Renderer::queue_earth_background(glm::vec3 /*cam_pos*/, float yaw, float pitch)
{
    m_sky_pending = false;
    if (!m_earth_tex_array || m_earth_num_frames == 0 || !m_sky_vbuf) return;

    // ── Advance animation frame based on elapsed wall time ───────────────────
    static uint32_t s_last_ticks = 0;
    uint32_t now = SDL_GetTicks();
    if (s_last_ticks == 0) s_last_ticks = now;
    uint32_t delta_ms = now - s_last_ticks;
    s_last_ticks = now;

    m_earth_accum_ms += delta_ms;
    int delay = (m_earth_frame < m_earth_delays.size())
                    ? m_earth_delays[m_earth_frame] : 100;
    if (delay <= 0) delay = 100;
    while (m_earth_accum_ms >= static_cast<uint32_t>(delay)) {
        m_earth_accum_ms -= static_cast<uint32_t>(delay);
        m_earth_frame = (m_earth_frame + 1) % m_earth_num_frames;
        delay = (m_earth_frame < m_earth_delays.size())
                    ? m_earth_delays[m_earth_frame] : 100;
        if (delay <= 0) delay = 100;
    }

    // ── Skybox-style rotation-only view-projection ────────────────────────────
    // Strip player translation: eye is fixed at origin so the Earth always
    // appears at the same screen direction regardless of world position.
    // This is pure client-side / cosmetic rendering – it never interacts with
    // any game geometry or depth values.
    float yaw_r   = glm::radians(yaw);
    float pitch_r = glm::radians(pitch);
    glm::vec3 forward = {
        std::cos(pitch_r) * std::sin(yaw_r),
        std::sin(pitch_r),
       -std::cos(pitch_r) * std::cos(yaw_r)
    };
    // lookAt with eye at origin: pure rotation, no translation
    glm::mat4 sky_view = glm::lookAt(glm::vec3(0.f), forward, {0.f, 1.f, 0.f});
    float aspect = (m_height > 0) ? float(m_width) / float(m_height) : 1.f;
    glm::mat4 sky_proj = glm::perspective(glm::radians(90.f), aspect, 0.1f, 1000.f);
    m_sky_mvp = sky_proj * sky_view;

    // ── Earth quad in skybox space ────────────────────────────────────────────
    // Fixed angular direction: slightly below and behind the forward axis so the
    // planet stays visible while orbiting (look down-forward to see it fill view).
    // DIST and HALF together determine apparent angular size:
    //   half-angle = atan(HALF / DIST).  At 90 deg FOV the screen half-height
    //   covers atan(1) = 45 deg, so HALF/DIST = 0.95 fills ~95% of the viewport.
    static constexpr float DIST      = 100.f;
    static constexpr float HALF      = 95.f;   // fills ~95% of viewport half-height
    // Earth sits slightly below the horizon in skybox space
    const glm::vec3 EARTH_DIR = glm::normalize(glm::vec3(0.f, -0.45f, -1.f));
    const glm::vec3 EARTH_CENTER = EARTH_DIR * DIST;

    // Build a stable up/right basis for the quad that doesn't depend on
    // camera orientation (we want the planet to appear upright)
    const glm::vec3 world_up = {0.f, 1.f, 0.f};
    glm::vec3 right, earth_up;
    float dot_up = std::abs(glm::dot(EARTH_DIR, world_up));
    if (dot_up < 0.99f)
        right = glm::normalize(glm::cross(EARTH_DIR, world_up));
    else
        right = {1.f, 0.f, 0.f};
    earth_up = glm::normalize(glm::cross(right, EARTH_DIR));

    glm::vec3 corners[4] = {
        EARTH_CENTER + (-right - earth_up) * HALF,  // bottom-left
        EARTH_CENTER + ( right - earth_up) * HALF,  // bottom-right
        EARTH_CENTER + ( right + earth_up) * HALF,  // top-right
        EARTH_CENTER + (-right + earth_up) * HALF,  // top-left
    };

    static const float k_uv[4][2] = {{0.f,1.f},{1.f,1.f},{1.f,0.f},{0.f,0.f}};
    float texIdx = static_cast<float>(m_earth_frame);
    for (int i = 0; i < 4; ++i) {
        m_sky_verts[i] = { corners[i].x, corners[i].y, corners[i].z,
                           1.f, 1.f, 1.f, 1.f,
                           k_uv[i][0], k_uv[i][1],
                           texIdx };
    }
    m_sky_pending = true;
}

void Renderer::upload_sky_geometry()
{
    if (!m_sky_pending || !m_sky_vbuf || !m_cmd_buf) return;

    const Uint32 vsize = 4 * sizeof(ItemVert);

    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size  = vsize;
    auto* tbuf = SDL_CreateGPUTransferBuffer(m_gpu, &tbci);
    if (!tbuf) return;

    auto* ptr = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(m_gpu, tbuf, false));
    std::memcpy(ptr, m_sky_verts, vsize);
    SDL_UnmapGPUTransferBuffer(m_gpu, tbuf);

    auto* copy_pass = SDL_BeginGPUCopyPass(m_cmd_buf);
    SDL_GPUTransferBufferLocation sv{ tbuf, 0 };
    SDL_GPUBufferRegion           dv{ m_sky_vbuf, 0, vsize };
    SDL_UploadToGPUBuffer(copy_pass, &sv, &dv, false);
    SDL_EndGPUCopyPass(copy_pass);
    SDL_ReleaseGPUTransferBuffer(m_gpu, tbuf);
}

void Renderer::draw_space_background()
{
    if (!m_sky_pending || !m_sky_pipeline || !m_earth_tex_array ||
        !m_earth_sampler || !m_sky_vbuf || !m_sky_ibuf ||
        !m_render_pass || !m_cmd_buf) return;

    SDL_BindGPUGraphicsPipeline(m_render_pass, m_sky_pipeline);
    // Use the rotation-only sky MVP: Earth is locked to camera direction,
    // never affected by player world-space translation (pure client-side render).
    SDL_PushGPUVertexUniformData(m_cmd_buf, 0, &m_sky_mvp[0][0], sizeof(glm::mat4));
    SDL_GPUTextureSamplerBinding tsb{ m_earth_tex_array, m_earth_sampler };
    SDL_BindGPUFragmentSamplers(m_render_pass, 0, &tsb, 1);
    SDL_GPUBufferBinding vb{ m_sky_vbuf, 0 };
    SDL_GPUBufferBinding ib{ m_sky_ibuf, 0 };
    SDL_BindGPUVertexBuffers(m_render_pass, 0, &vb, 1);
    SDL_BindGPUIndexBuffer(m_render_pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(m_render_pass, 6, 1, 0, 0, 0);
}
