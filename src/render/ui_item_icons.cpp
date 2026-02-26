// ui_item_icons.cpp — implements UIRenderer::load_item_icons / item_icon.
// Kept in its own TU so that ui_renderer.cpp never has to include the heavy
// nlohmann/json.hpp that item_registry.h pulls in.
#include "render/ui_renderer.h"
#include "inventory/item_registry.h"
#include "stb_image.h"
#include <cstring>

void UIRenderer::load_item_icons(const ItemRegistry& reg, const char* texture_dir)
{
    m_item_icons.clear();

    // ── Phase 1: load pixels from disk + create GPU textures ─────────────────
    // We collect all pending uploads and flush them in a single command buffer
    // so we never exhaust the GPU's command-buffer pool when there are many icons.
    struct PendingIcon {
        std::string            id;
        SDL_GPUTexture*        tex  = nullptr;
        SDL_GPUTransferBuffer* tbuf = nullptr;
        int                    w = 0, h = 0;
    };
    std::vector<PendingIcon> pending;
    pending.reserve(reg.all().size());

    for (const auto& [id, def] : reg.all()) {
        if (def.icon.empty()) continue;
        std::string path = std::string(texture_dir) + "/" + def.icon + ".png";

        int w, h, ch;
        unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
        if (!data) {
            SDL_Log("UIRenderer::load_item_icons: stbi_load failed for %s", path.c_str());
            continue;
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
            SDL_Log("UIRenderer::load_item_icons: SDL_CreateGPUTexture failed for %s: %s",
                    path.c_str(), SDL_GetError());
            stbi_image_free(data);
            continue;
        }
        m_owned_textures.push_back(tex);

        Uint32 byte_size = (Uint32)(w * h * 4);
        SDL_GPUTransferBufferCreateInfo tbci{};
        tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbci.size  = byte_size;
        auto* tbuf = SDL_CreateGPUTransferBuffer(m_gpu, &tbci);
        if (!tbuf) {
            SDL_Log("UIRenderer::load_item_icons: transfer buffer failed for %s", path.c_str());
            stbi_image_free(data);
            continue;
        }
        void* ptr = SDL_MapGPUTransferBuffer(m_gpu, tbuf, false);
        std::memcpy(ptr, data, byte_size);
        SDL_UnmapGPUTransferBuffer(m_gpu, tbuf);
        stbi_image_free(data);

        pending.push_back({id, tex, tbuf, w, h});
    }

    // ── Phase 2: upload everything in one command buffer ──────────────────────
    if (!pending.empty()) {
        auto* cmd = SDL_AcquireGPUCommandBuffer(m_gpu);
        auto* cp  = SDL_BeginGPUCopyPass(cmd);
        for (auto& p : pending) {
            SDL_GPUTextureTransferInfo src{};
            src.transfer_buffer = p.tbuf;
            src.pixels_per_row  = (Uint32)p.w;
            src.rows_per_layer  = (Uint32)p.h;
            SDL_GPUTextureRegion dst{};
            dst.texture = p.tex;
            dst.w = (Uint32)p.w;
            dst.h = (Uint32)p.h;
            dst.d = 1;
            SDL_UploadToGPUTexture(cp, &src, &dst, false);
        }
        SDL_EndGPUCopyPass(cp);
        SDL_SubmitGPUCommandBuffer(cmd);
        for (auto& p : pending)
            SDL_ReleaseGPUTransferBuffer(m_gpu, p.tbuf);
    }

    // ── Phase 3: register icons ───────────────────────────────────────────────
    for (auto& p : pending)
        m_item_icons[p.id] = p.tex;

    SDL_Log("UIRenderer: cached %u item icons", (unsigned)m_item_icons.size());
}

SDL_GPUTexture* UIRenderer::item_icon(const std::string& item_id) const
{
    auto it = m_item_icons.find(item_id);
    return it != m_item_icons.end() ? it->second : nullptr;
}
