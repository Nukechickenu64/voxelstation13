#include "render/renderer.h"
#include <stdexcept>
#include <cmath>

Renderer::Renderer()  = default;
Renderer::~Renderer() { shutdown(); }

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

    m_gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV |
                                SDL_GPU_SHADERFORMAT_DXIL  |
                                SDL_GPU_SHADERFORMAT_MSL,
                                false, nullptr);
    if (!m_gpu) {
        SDL_Log("SDL_CreateGPUDevice failed: %s", SDL_GetError());
        return false;
    }

    if (!SDL_ClaimWindowForGPUDevice(m_gpu, m_window)) {
        SDL_Log("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        return false;
    }

    return true;
}

void Renderer::shutdown()
{
    if (m_gpu && m_window) SDL_ReleaseWindowFromGPUDevice(m_gpu, m_window);
    if (m_gpu)    SDL_DestroyGPUDevice(m_gpu);
    if (m_window) SDL_DestroyWindow(m_window);
    m_gpu    = nullptr;
    m_window = nullptr;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void Renderer::begin_frame(double /*alpha*/)
{
    // Begin GPU command buffer and clear pass would go here.
    // Full pipeline construction omitted for scaffold — see render/shaders/.
}

void Renderer::draw_world(const World& /*world*/, glm::vec3 /*cam_pos*/,
                          float /*yaw*/, float /*pitch*/)
{
    // Submit chunk mesh draw calls built from m_meshes.
    // TODO: bind world pipeline, set uniforms (view/proj matrix), issue draw indexed.
}

void Renderer::draw_face_highlight(const RayHit& hit)
{
    if (!hit.valid) return;
    // TODO: render thin outline quad over the selected face in a separate pass.
    (void)hit;
}

void Renderer::draw_viewmodel(uint16_t /*item_type_id*/)
{
    // TODO: render held item mesh in view-model pass (fixed near-plane).
}

void Renderer::end_frame()
{
    // TODO: submit command buffer, present swap chain.
}

ChunkMesh& Renderer::get_or_create_mesh(glm::ivec3 chunk_pos)
{
    auto& mesh = m_meshes[chunk_pos];
    mesh.chunk_pos = chunk_pos;
    return mesh;
}

void Renderer::upload_mesh(ChunkMesh& mesh)
{
    // TODO: create/upload SDL_GPUBuffer for mesh.vertices and mesh.indices.
    mesh.dirty = false;
    (void)mesh;
}

void Renderer::free_mesh(glm::ivec3 chunk_pos)
{
    m_meshes.erase(chunk_pos);
}
