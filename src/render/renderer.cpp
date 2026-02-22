#include "render/renderer.h"
#include "render/shaders/chunk_vert_spv.h"
#include "render/shaders/chunk_frag_spv.h"
#include "render/shaders/highlight_vert_spv.h"
#include "render/shaders/highlight_frag_spv.h"
#include "core/world.h"
#include "data/voxel_registry.h"
#include "stb_image.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

#include <cstring>
#include <cmath>

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
        if (m_highlight_vbuf)     { SDL_ReleaseGPUBuffer(m_gpu, m_highlight_vbuf);               m_highlight_vbuf     = nullptr; }
        if (m_highlight_ibuf)     { SDL_ReleaseGPUBuffer(m_gpu, m_highlight_ibuf);               m_highlight_ibuf     = nullptr; }
        if (m_vert_shader)        { SDL_ReleaseGPUShader(m_gpu, m_vert_shader);                  m_vert_shader        = nullptr; }
        if (m_frag_shader)        { SDL_ReleaseGPUShader(m_gpu, m_frag_shader);                  m_frag_shader        = nullptr; }
        if (m_tile_array)         { SDL_ReleaseGPUTexture(m_gpu, m_tile_array);                  m_tile_array         = nullptr; }
        if (m_tile_sampler)       { SDL_ReleaseGPUSampler(m_gpu, m_tile_sampler);                m_tile_sampler       = nullptr; }
        if (m_depth_tex)          { SDL_ReleaseGPUTexture(m_gpu, m_depth_tex);                   m_depth_tex          = nullptr; }

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

    // ── Begin render pass ─────────────────────────────────────────────────────
    SDL_GPUColorTargetInfo color_info{};
    color_info.texture     = m_swapchain_tex;
    color_info.clear_color = { 0.53f, 0.81f, 0.98f, 1.0f };  // sky blue
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
