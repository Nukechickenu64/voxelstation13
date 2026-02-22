#include "render/ui_renderer.h"
#include "render/shaders/ui_vert_spv.h"
#include "render/shaders/ui_frag_spv.h"
#include "render/shaders/ui_text_frag_spv.h"
#include "render/shaders/roboto_atlas.h"
#include "stb_image.h"

#include <cstring>
#include <cmath>
#include <algorithm>

UIRenderer::UIRenderer(SDL_GPUDevice* gpu) : m_gpu(gpu) {}
UIRenderer::~UIRenderer() { shutdown(); }

// â”€â”€ Init / shutdown â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

bool UIRenderer::init(SDL_Window* window, int fb_width, int fb_height)
{
    m_fb_w = fb_width;
    m_fb_h = fb_height;

    if (!create_white_texture()) return false;
    if (!create_pipeline(window)) return false;
    if (!create_text_pipeline(window)) return false;

    // ── Load Roboto MTSDF font atlas ──────────────────────────────────────
    m_font_tex = load_texture("textures/font/roboto.png");
    if (!m_font_tex)
        SDL_Log("UIRenderer: font atlas not loaded -- text will be invisible");

    // â”€â”€ Sampler: linear + clamp â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter     = SDL_GPU_FILTER_LINEAR;
    sci.mag_filter     = SDL_GPU_FILTER_LINEAR;
    sci.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    m_sampler = SDL_CreateGPUSampler(m_gpu, &sci);
    if (!m_sampler) {
        SDL_Log("UIRenderer: SDL_CreateGPUSampler failed: %s", SDL_GetError());
        return false;
    }

    // â”€â”€ Pre-allocated GPU vertex / index buffers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        SDL_GPUBufferCreateInfo vbi{};
        vbi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        vbi.size  = k_max_verts * (Uint32)sizeof(UIVertex);
        m_vbuf = SDL_CreateGPUBuffer(m_gpu, &vbi);
    }
    {
        SDL_GPUBufferCreateInfo ibi{};
        ibi.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        ibi.size  = k_max_indices * (Uint32)sizeof(uint32_t);
        m_ibuf = SDL_CreateGPUBuffer(m_gpu, &ibi);
    }
    if (!m_vbuf || !m_ibuf) {
        SDL_Log("UIRenderer: failed to create geometry buffers: %s", SDL_GetError());
        return false;
    }

    // â”€â”€ Transfer buffers (remapped each frame in begin/end) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        SDL_GPUTransferBufferCreateInfo tbci{};
        tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbci.size  = k_max_verts * (Uint32)sizeof(UIVertex);
        m_vert_tbuf = SDL_CreateGPUTransferBuffer(m_gpu, &tbci);
    }
    {
        SDL_GPUTransferBufferCreateInfo tbci{};
        tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbci.size  = k_max_indices * (Uint32)sizeof(uint32_t);
        m_idx_tbuf = SDL_CreateGPUTransferBuffer(m_gpu, &tbci);
    }
    if (!m_vert_tbuf || !m_idx_tbuf) {
        SDL_Log("UIRenderer: failed to create transfer buffers: %s", SDL_GetError());
        return false;
    }

    return true;
}

bool UIRenderer::create_white_texture()
{
    SDL_GPUTextureCreateInfo tci{};
    tci.type                 = SDL_GPU_TEXTURETYPE_2D;
    tci.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width                = 1;
    tci.height               = 1;
    tci.layer_count_or_depth = 1;
    tci.num_levels           = 1;
    tci.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    m_white_tex = SDL_CreateGPUTexture(m_gpu, &tci);
    if (!m_white_tex) {
        SDL_Log("UIRenderer: failed to create white texture: %s", SDL_GetError());
        return false;
    }

    const uint8_t white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };

    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size  = 4;
    auto* tbuf = SDL_CreateGPUTransferBuffer(m_gpu, &tbci);
    void* ptr  = SDL_MapGPUTransferBuffer(m_gpu, tbuf, false);
    std::memcpy(ptr, white, 4);
    SDL_UnmapGPUTransferBuffer(m_gpu, tbuf);

    auto* cmd = SDL_AcquireGPUCommandBuffer(m_gpu);
    auto* cp  = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tbuf;
    src.pixels_per_row  = 1;
    src.rows_per_layer  = 1;

    SDL_GPUTextureRegion dst{};
    dst.texture = m_white_tex;
    dst.w = dst.h = dst.d = 1;

    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(m_gpu, tbuf);
    return true;
}

bool UIRenderer::create_pipeline(SDL_Window* window)
{
    SDL_GPUShaderCreateInfo vi{};
    vi.code                = reinterpret_cast<const Uint8*>(k_ui_vert_spv);
    vi.code_size           = k_ui_vert_spv_size;
    vi.entrypoint          = "main";
    vi.format              = SDL_GPU_SHADERFORMAT_SPIRV;
    vi.stage               = SDL_GPU_SHADERSTAGE_VERTEX;
    vi.num_uniform_buffers = 1;  // slot 0 = xform vec4

    auto* vs = SDL_CreateGPUShader(m_gpu, &vi);
    if (!vs) { SDL_Log("UIRenderer: vert shader: %s", SDL_GetError()); return false; }

    SDL_GPUShaderCreateInfo fi{};
    fi.code         = reinterpret_cast<const Uint8*>(k_ui_frag_spv);
    fi.code_size    = k_ui_frag_spv_size;
    fi.entrypoint   = "main";
    fi.format       = SDL_GPU_SHADERFORMAT_SPIRV;
    fi.stage        = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fi.num_samplers = 1;  // slot 0 = texture

    auto* fs = SDL_CreateGPUShader(m_gpu, &fi);
    if (!fs) {
        SDL_Log("UIRenderer: frag shader: %s", SDL_GetError());
        SDL_ReleaseGPUShader(m_gpu, vs);
        return false;
    }

    // Vertex layout: {x,y} + {u,v} + {r,g,b,a} = 32 bytes
    SDL_GPUVertexBufferDescription vbd{};
    vbd.slot              = 0;
    vbd.pitch             = (Uint32)sizeof(UIVertex);
    vbd.input_rate        = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vbd.instance_step_rate = 0;

    SDL_GPUVertexAttribute attrs[3]{};
    attrs[0] = { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,  0 };  // pos   (loc 0)
    attrs[1] = { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,  8 };  // uv    (loc 1)
    attrs[2] = { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16 };  // color (loc 2)

    // Premultiplied-alpha blend
    SDL_GPUColorTargetBlendState bs{};
    bs.enable_blend           = true;
    bs.color_blend_op         = SDL_GPU_BLENDOP_ADD;
    bs.alpha_blend_op         = SDL_GPU_BLENDOP_ADD;
    bs.src_color_blendfactor  = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    bs.dst_color_blendfactor  = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    bs.src_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_ONE;
    bs.dst_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

    SDL_GPUColorTargetDescription ctd{};
    ctd.format      = SDL_GetGPUSwapchainTextureFormat(m_gpu, window);
    ctd.blend_state = bs;

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader   = vs;
    pci.fragment_shader = fs;

    pci.vertex_input_state.vertex_buffer_descriptions = &vbd;
    pci.vertex_input_state.num_vertex_buffers         = 1;
    pci.vertex_input_state.vertex_attributes          = attrs;
    pci.vertex_input_state.num_vertex_attributes      = 3;

    pci.primitive_type                         = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pci.rasterizer_state.fill_mode             = SDL_GPU_FILLMODE_FILL;
    pci.rasterizer_state.cull_mode             = SDL_GPU_CULLMODE_NONE;
    pci.depth_stencil_state.enable_depth_test  = false;
    pci.depth_stencil_state.enable_depth_write = false;

    pci.target_info.color_target_descriptions = &ctd;
    pci.target_info.num_color_targets         = 1;

    m_pipeline = SDL_CreateGPUGraphicsPipeline(m_gpu, &pci);
    SDL_ReleaseGPUShader(m_gpu, vs);
    SDL_ReleaseGPUShader(m_gpu, fs);

    if (!m_pipeline) {
        SDL_Log("UIRenderer: pipeline: %s", SDL_GetError());
        return false;
    }
    return true;
}

bool UIRenderer::create_text_pipeline(SDL_Window* window)
{
    // Identical vertex shader; different fragment shader (MTSDF).
    SDL_GPUShaderCreateInfo vi{};
    vi.code                = reinterpret_cast<const Uint8*>(k_ui_vert_spv);
    vi.code_size           = k_ui_vert_spv_size;
    vi.entrypoint          = "main";
    vi.format              = SDL_GPU_SHADERFORMAT_SPIRV;
    vi.stage               = SDL_GPU_SHADERSTAGE_VERTEX;
    vi.num_uniform_buffers = 1;

    auto* vs = SDL_CreateGPUShader(m_gpu, &vi);
    if (!vs) { SDL_Log("UIRenderer: text vert shader: %s", SDL_GetError()); return false; }

    SDL_GPUShaderCreateInfo fi{};
    fi.code         = reinterpret_cast<const Uint8*>(k_ui_text_frag_spv);
    fi.code_size    = k_ui_text_frag_spv_size;
    fi.entrypoint   = "main";
    fi.format       = SDL_GPU_SHADERFORMAT_SPIRV;
    fi.stage        = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fi.num_samplers = 1;

    auto* fs = SDL_CreateGPUShader(m_gpu, &fi);
    if (!fs) {
        SDL_Log("UIRenderer: text frag shader: %s", SDL_GetError());
        SDL_ReleaseGPUShader(m_gpu, vs);
        return false;
    }

    SDL_GPUVertexBufferDescription vbd{};
    vbd.slot               = 0;
    vbd.pitch              = (Uint32)sizeof(UIVertex);
    vbd.input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vbd.instance_step_rate = 0;

    SDL_GPUVertexAttribute attrs[3]{};
    attrs[0] = { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,  0 };
    attrs[1] = { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,  8 };
    attrs[2] = { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16 };

    SDL_GPUColorTargetBlendState bs{};
    bs.enable_blend           = true;
    bs.color_blend_op         = SDL_GPU_BLENDOP_ADD;
    bs.alpha_blend_op         = SDL_GPU_BLENDOP_ADD;
    bs.src_color_blendfactor  = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    bs.dst_color_blendfactor  = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    bs.src_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_ONE;
    bs.dst_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

    SDL_GPUColorTargetDescription ctd{};
    ctd.format      = SDL_GetGPUSwapchainTextureFormat(m_gpu, window);
    ctd.blend_state = bs;

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader   = vs;
    pci.fragment_shader = fs;
    pci.vertex_input_state.vertex_buffer_descriptions = &vbd;
    pci.vertex_input_state.num_vertex_buffers         = 1;
    pci.vertex_input_state.vertex_attributes          = attrs;
    pci.vertex_input_state.num_vertex_attributes      = 3;
    pci.primitive_type                         = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pci.rasterizer_state.fill_mode             = SDL_GPU_FILLMODE_FILL;
    pci.rasterizer_state.cull_mode             = SDL_GPU_CULLMODE_NONE;
    pci.depth_stencil_state.enable_depth_test  = false;
    pci.depth_stencil_state.enable_depth_write = false;
    pci.target_info.color_target_descriptions  = &ctd;
    pci.target_info.num_color_targets          = 1;

    m_text_pipeline = SDL_CreateGPUGraphicsPipeline(m_gpu, &pci);
    SDL_ReleaseGPUShader(m_gpu, vs);
    SDL_ReleaseGPUShader(m_gpu, fs);

    if (!m_text_pipeline) {
        SDL_Log("UIRenderer: text pipeline: %s", SDL_GetError());
        return false;
    }
    return true;
}

void UIRenderer::shutdown()
{
    if (!m_gpu) return;
    SDL_WaitForGPUIdle(m_gpu);

    for (auto* t : m_owned_textures) if (t) SDL_ReleaseGPUTexture(m_gpu, t);
    m_owned_textures.clear();

    if (m_white_tex)       { SDL_ReleaseGPUTexture(m_gpu, m_white_tex);              m_white_tex      = nullptr; }
    if (m_sampler)         { SDL_ReleaseGPUSampler(m_gpu, m_sampler);                m_sampler        = nullptr; }
    if (m_pipeline)        { SDL_ReleaseGPUGraphicsPipeline(m_gpu, m_pipeline);      m_pipeline       = nullptr; }
    if (m_text_pipeline)   { SDL_ReleaseGPUGraphicsPipeline(m_gpu, m_text_pipeline); m_text_pipeline  = nullptr; }
    if (m_vbuf)            { SDL_ReleaseGPUBuffer(m_gpu, m_vbuf);                    m_vbuf           = nullptr; }
    if (m_ibuf)            { SDL_ReleaseGPUBuffer(m_gpu, m_ibuf);                    m_ibuf           = nullptr; }
    if (m_vert_tbuf)       { SDL_ReleaseGPUTransferBuffer(m_gpu, m_vert_tbuf);       m_vert_tbuf      = nullptr; }
    if (m_idx_tbuf)        { SDL_ReleaseGPUTransferBuffer(m_gpu, m_idx_tbuf);        m_idx_tbuf       = nullptr; }
    // m_font_tex is in m_owned_textures, already freed above
    m_font_tex = nullptr;
}

void UIRenderer::on_resize(int w, int h) { m_fb_w = w; m_fb_h = h; }

// â”€â”€ Per-frame begin / end â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void UIRenderer::begin()
{
    m_vert_count = 0;
    m_idx_count  = 0;
    m_batches.clear();

    m_vert_ptr = static_cast<UIVertex*>(
        SDL_MapGPUTransferBuffer(m_gpu, m_vert_tbuf, true));
    m_idx_ptr  = static_cast<uint32_t*>(
        SDL_MapGPUTransferBuffer(m_gpu, m_idx_tbuf,  true));
}

void UIRenderer::end(SDL_GPUCommandBuffer* cmd_buf,
                     SDL_GPUTexture*       swapchain_tex,
                     int                   fb_w,
                     int                   fb_h)
{
    m_fb_w = fb_w;
    m_fb_h = fb_h;

    SDL_UnmapGPUTransferBuffer(m_gpu, m_vert_tbuf);
    SDL_UnmapGPUTransferBuffer(m_gpu, m_idx_tbuf);
    m_vert_ptr = nullptr;
    m_idx_ptr  = nullptr;

    if (m_batches.empty() || !cmd_buf || !swapchain_tex || !m_pipeline) return;

    // â”€â”€ Copy pass: staging â†’ GPU buffers (must happen before render pass) â”€
    auto* cp = SDL_BeginGPUCopyPass(cmd_buf);
    {
        SDL_GPUTransferBufferLocation src{ m_vert_tbuf, 0 };
        SDL_GPUBufferRegion dst{ m_vbuf, 0,
            m_vert_count * (Uint32)sizeof(UIVertex) };
        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    }
    {
        SDL_GPUTransferBufferLocation src{ m_idx_tbuf, 0 };
        SDL_GPUBufferRegion dst{ m_ibuf, 0,
            m_idx_count * (Uint32)sizeof(uint32_t) };
        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(cp);

    // â”€â”€ UI render pass (LOAD so world pass is preserved) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    SDL_GPUColorTargetInfo cti{};
    cti.texture  = swapchain_tex;
    cti.load_op  = SDL_GPU_LOADOP_LOAD;
    cti.store_op = SDL_GPU_STOREOP_STORE;

    auto* rp = SDL_BeginGPURenderPass(cmd_buf, &cti, 1, nullptr);

    float xform[4] = {
         2.f / (float)fb_w,   //  scale_x
        -2.f / (float)fb_h,   //  scale_y (flip Y: top-left = NDC origin)
        -1.f,                  //  bias_x
         1.f                   //  bias_y
    };

    SDL_GPUBufferBinding vbind{ m_vbuf, 0 };
    SDL_GPUBufferBinding ibind{ m_ibuf, 0 };
    SDL_BindGPUVertexBuffers(rp, 0, &vbind, 1);
    SDL_BindGPUIndexBuffer(rp, &ibind, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    SDL_GPUGraphicsPipeline* cur_pl = nullptr;
    for (const auto& batch : m_batches) {
        SDL_GPUGraphicsPipeline* want = batch.is_text ? m_text_pipeline : m_pipeline;
        if (want && want != cur_pl) {
            SDL_BindGPUGraphicsPipeline(rp, want);
            // Re-push the xform uniform after every pipeline switch.
            SDL_PushGPUVertexUniformData(cmd_buf, 0, xform, sizeof(xform));
            cur_pl = want;
        }
        SDL_GPUTextureSamplerBinding tsb{ batch.tex, m_sampler };
        SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
        SDL_DrawGPUIndexedPrimitives(rp, batch.count, 1, batch.first, 0, 0);
    }

    SDL_EndGPURenderPass(rp);
}

// â”€â”€ Geometry helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

bool UIRenderer::push_quad(glm::vec2 pos, glm::vec2 size,
                            float u0, float v0, float u1, float v1,
                            glm::vec4 color, SDL_GPUTexture* tex,
                            bool is_text)
{
    if (!m_vert_ptr || !m_idx_ptr) return false;
    if (m_vert_count + 4 > k_max_verts ||
        m_idx_count  + 6 > k_max_indices) return false;

    float x0 = pos.x,          y0 = pos.y;
    float x1 = pos.x + size.x, y1 = pos.y + size.y;
    float r = color.r, g = color.g, b = color.b, a = color.a;

    UIVertex* v = m_vert_ptr + m_vert_count;
    v[0] = { x0, y0, u0, v0, r, g, b, a };
    v[1] = { x1, y0, u1, v0, r, g, b, a };
    v[2] = { x1, y1, u1, v1, r, g, b, a };
    v[3] = { x0, y1, u0, v1, r, g, b, a };

    uint32_t  base = m_vert_count;
    uint32_t* idx  = m_idx_ptr + m_idx_count;
    idx[0] = base+0; idx[1] = base+1; idx[2] = base+2;
    idx[3] = base+0; idx[4] = base+2; idx[5] = base+3;

    m_vert_count += 4;

    if (!m_batches.empty() && m_batches.back().tex == tex
                           && m_batches.back().is_text == is_text)
        m_batches.back().count += 6;
    else
        m_batches.push_back({ tex, m_idx_count, 6, is_text });

    m_idx_count += 6;
    return true;
}

// â”€â”€ Public draw API â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void UIRenderer::rect(glm::vec2 pos, glm::vec2 size, glm::vec4 color,
                       float /*corner_radius*/)
{
    push_quad(pos, size, 0.f, 0.f, 1.f, 1.f, color, m_white_tex);
}

void UIRenderer::text(glm::vec2 pos, const std::string& str,
                       glm::vec4 color, float font_size)
{
    if (str.empty() || !m_font_tex) return;

    // Scale glyphs so that the cap-height equals font_size pixels.
    const float scale = font_size / k_roboto_cap_height;
    float x = pos.x;

    for (unsigned char c : str) {
        if (c < 32 || c > 126) {
            // Unknown char: advance by a space width.
            x += k_roboto_glyphs[0].advance_uv * k_roboto_atlas_w * scale;
            continue;
        }
        const RoboGlyph& g = k_roboto_glyphs[c - 32];
        if (g.u1 > g.u0) {
            float gw    = (g.u1 - g.u0) * k_roboto_atlas_w * scale;
            float gh    = (g.v1 - g.v0) * k_roboto_atlas_h * scale;
            float y_off = g.bearing_y * scale;  // baseline-relative vertical offset
            push_quad({x, pos.y + y_off}, {gw, gh},
                      g.u0, g.v0, g.u1, g.v1,
                      color, m_font_tex, /*is_text=*/true);
        }
        x += g.advance_uv * k_roboto_atlas_w * scale;
    }
}

void UIRenderer::icon(glm::vec2 pos, glm::vec2 size,
                       uint16_t /*atlas_index*/, float alpha)
{
    push_quad(pos, size, 0.f, 0.f, 1.f, 1.f,
              { 1.f, 1.f, 1.f, alpha }, m_white_tex);
}

void UIRenderer::line(glm::vec2 a, glm::vec2 b, glm::vec4 color,
                       float thickness)
{
    if (!m_vert_ptr || !m_idx_ptr) return;
    if (m_vert_count + 4 > k_max_verts ||
        m_idx_count  + 6 > k_max_indices) return;

    glm::vec2 dir = b - a;
    float     len = std::sqrt(dir.x*dir.x + dir.y*dir.y);
    if (len < 0.001f) return;
    glm::vec2 n = { -dir.y / len, dir.x / len };
    glm::vec2 off = n * (thickness * 0.5f);

    float r = color.r, g = color.g, bl = color.b, al = color.a;
    UIVertex* v = m_vert_ptr + m_vert_count;
    v[0] = { a.x + off.x, a.y + off.y, 0.5f, 0.5f, r, g, bl, al };
    v[1] = { b.x + off.x, b.y + off.y, 0.5f, 0.5f, r, g, bl, al };
    v[2] = { b.x - off.x, b.y - off.y, 0.5f, 0.5f, r, g, bl, al };
    v[3] = { a.x - off.x, a.y - off.y, 0.5f, 0.5f, r, g, bl, al };

    uint32_t  base = m_vert_count;
    uint32_t* idx  = m_idx_ptr + m_idx_count;
    idx[0] = base+0; idx[1] = base+1; idx[2] = base+2;
    idx[3] = base+0; idx[4] = base+2; idx[5] = base+3;
    m_vert_count += 4;

    if (!m_batches.empty() && m_batches.back().tex == m_white_tex
                           && !m_batches.back().is_text)
        m_batches.back().count += 6;
    else
        m_batches.push_back({ m_white_tex, m_idx_count, 6, false });
    m_idx_count += 6;
}

SDL_GPUTexture* UIRenderer::load_texture(const char* path)
{
    int w, h, ch;
    unsigned char* data = stbi_load(path, &w, &h, &ch, 4);
    if (!data) {
        SDL_Log("UIRenderer::load_texture: stbi_load failed for %s: %s",
                path, stbi_failure_reason());
        return nullptr;
    }

    SDL_GPUTextureCreateInfo tci{};
    tci.type                 = SDL_GPU_TEXTURETYPE_2D;
    tci.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width                = (Uint32)w;
    tci.height               = (Uint32)h;
    tci.layer_count_or_depth = 1;
    tci.num_levels           = 1;
    tci.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    auto* tex = SDL_CreateGPUTexture(m_gpu, &tci);
    if (!tex) {
        SDL_Log("UIRenderer::load_texture: SDL_CreateGPUTexture failed: %s", SDL_GetError());
        stbi_image_free(data);
        return nullptr;
    }

    Uint32 byte_size = (Uint32)(w * h * 4);
    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size  = byte_size;
    auto* tbuf = SDL_CreateGPUTransferBuffer(m_gpu, &tbci);
    void* ptr  = SDL_MapGPUTransferBuffer(m_gpu, tbuf, false);
    std::memcpy(ptr, data, byte_size);
    SDL_UnmapGPUTransferBuffer(m_gpu, tbuf);
    stbi_image_free(data);

    auto* cmd = SDL_AcquireGPUCommandBuffer(m_gpu);
    auto* cp  = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tbuf;
    src.pixels_per_row  = (Uint32)w;
    src.rows_per_layer  = (Uint32)h;

    SDL_GPUTextureRegion dst{};
    dst.texture = tex;
    dst.w = (Uint32)w;
    dst.h = (Uint32)h;
    dst.d = 1;

    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(m_gpu, tbuf);

    m_owned_textures.push_back(tex);
    SDL_Log("UIRenderer::load_texture: loaded %s (%dx%d)", path, w, h);
    return tex;
}

void UIRenderer::image(glm::vec2 pos, glm::vec2 size, SDL_GPUTexture* tex,
                        float alpha, bool flip_x)
{
    if (!tex) return;
    float u0 = flip_x ? 1.f : 0.f;
    float u1 = flip_x ? 0.f : 1.f;
    push_quad(pos, size, u0, 0.f, u1, 1.f, { 1.f, 1.f, 1.f, alpha }, tex);
}

bool UIRenderer::hit_test(glm::vec2 /*screen_pos*/) const
{
    return false;
}

