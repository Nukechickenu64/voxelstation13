#include "render/renderer.h"
#include "render/shaders/chunk_vert_spv.h"
#include "render/shaders/chunk_frag_spv.h"

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

    if (!create_depth_texture()) return false;
    if (!create_pipeline())      return false;

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
    vi.num_uniform_buffers = 0;   // push constant – not counted here

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
    fi.num_uniform_buffers = 0;

    m_frag_shader = SDL_CreateGPUShader(m_gpu, &fi);
    if (!m_frag_shader) {
        SDL_Log("SDL_CreateGPUShader (frag) failed: %s", SDL_GetError());
        return false;
    }

    // ── Vertex layout: pos(3f) + normal(3f), stride 24 ───────────────────────
    SDL_GPUVertexBufferDescription vbuf_desc{};
    vbuf_desc.slot              = 0;
    vbuf_desc.pitch             = 24;   // 6 floats × 4 bytes
    vbuf_desc.input_rate        = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vbuf_desc.instance_step_rate = 0;

    SDL_GPUVertexAttribute vattrs[2]{};
    vattrs[0] = { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,  0 }; // location 0 = pos
    vattrs[1] = { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12 }; // location 1 = normal

    // ── Colour target format (matches the swapchain) ──────────────────────────
    SDL_GPUColorTargetDescription ctd{};
    ctd.format = SDL_GetGPUSwapchainTextureFormat(m_gpu, m_window);

    // ── Build pipeline ────────────────────────────────────────────────────────
    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader   = m_vert_shader;
    pci.fragment_shader = m_frag_shader;

    pci.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
    pci.vertex_input_state.num_vertex_buffers         = 1;
    pci.vertex_input_state.vertex_attributes          = vattrs;
    pci.vertex_input_state.num_vertex_attributes      = 2;

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

        if (m_world_pipeline) { SDL_ReleaseGPUGraphicsPipeline(m_gpu, m_world_pipeline); m_world_pipeline = nullptr; }
        if (m_vert_shader)    { SDL_ReleaseGPUShader(m_gpu, m_vert_shader);  m_vert_shader = nullptr; }
        if (m_frag_shader)    { SDL_ReleaseGPUShader(m_gpu, m_frag_shader);  m_frag_shader = nullptr; }
        if (m_depth_tex)      { SDL_ReleaseGPUTexture(m_gpu, m_depth_tex);   m_depth_tex   = nullptr; }

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

    SDL_GPUTexture* swapchain_tex = nullptr;
    Uint32 sw_w = 0, sw_h = 0;
    SDL_AcquireGPUSwapchainTexture(m_cmd_buf, m_window,
                                   &swapchain_tex, &sw_w, &sw_h);
    if (!swapchain_tex) {
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

    // ── Begin render pass ─────────────────────────────────────────────────────
    SDL_GPUColorTargetInfo color_info{};
    color_info.texture     = swapchain_tex;
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
    // Flip Y – Vulkan's clip-space Y is top-down, GLM assumes OpenGL convention
    proj[1][1] *= -1.f;
    glm::mat4 mvp = proj * view;

    // Push MVP as vertex push-constant slot 0
    SDL_PushGPUVertexUniformData(m_cmd_buf, 0, &mvp[0][0], sizeof(glm::mat4));

    SDL_BindGPUGraphicsPipeline(m_render_pass, m_world_pipeline);

    // ── Draw each uploaded GPU mesh ──────────────────────────────────────────
    for (auto& [chunk_pos, gm] : m_gpu_meshes) {
        if (!gm.vbuf || !gm.ibuf || gm.num_indices == 0) continue;

        SDL_GPUBufferBinding vb{ gm.vbuf, 0 };
        SDL_GPUBufferBinding ib{ gm.ibuf, 0 };
        SDL_BindGPUVertexBuffers(m_render_pass, 0, &vb, 1);
        SDL_BindGPUIndexBuffer(m_render_pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(m_render_pass, gm.num_indices, 1, 0, 0, 0);
    }
}

void Renderer::draw_face_highlight(const RayHit& hit)
{
    if (!hit.valid) return;
    // TODO: render thin outline quad over selected face (needs line pipeline)
    (void)hit;
}

void Renderer::draw_viewmodel(uint16_t /*item_type_id*/)
{
    // TODO: render held item mesh in view-model pass
}

void Renderer::end_frame()
{
    if (m_render_pass) {
        SDL_EndGPURenderPass(m_render_pass);
        m_render_pass = nullptr;
    }
    if (m_cmd_buf) {
        SDL_SubmitGPUCommandBuffer(m_cmd_buf);
        m_cmd_buf = nullptr;
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
