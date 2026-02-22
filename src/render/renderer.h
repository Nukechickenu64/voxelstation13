#pragma once
#include "core/world.h"
#include <SDL3/SDL.h>
#include <vector>
#include <string>

// GPU mesh for one rendered chunk
struct ChunkMesh {
    glm::ivec3 chunk_pos{};
    // Interleaved vertex data: pos(3f) uv(2f) layer_mask(1u) light(1f) normal(3f)
    std::vector<float>    vertices;
    std::vector<uint32_t> indices;
    bool transparent = false;   // true = transparency pass
    bool dirty       = true;
};

// Holds all SDL3 GPU state and drives the render loop.
class Renderer {
public:
    Renderer();
    ~Renderer();

    bool init(const char* title, int width, int height);
    void shutdown();

    // Called each frame. alpha = interpolation between simulation ticks.
    void begin_frame(double alpha);
    void draw_world(const World& world, glm::vec3 cam_pos, float yaw, float pitch);
    void draw_face_highlight(const RayHit& hit);
    void draw_viewmodel(uint16_t item_type_id);
    void end_frame();

    // Mesh management
    ChunkMesh& get_or_create_mesh(glm::ivec3 chunk_pos);
    void       upload_mesh(ChunkMesh& mesh);
    void       free_mesh(glm::ivec3 chunk_pos);

    SDL_Window*   window()   const { return m_window; }
    SDL_GPUDevice* gpu()     const { return m_gpu; }
    int width()  const { return m_width; }
    int height() const { return m_height; }

private:
    SDL_Window*    m_window  = nullptr;
    SDL_GPUDevice* m_gpu     = nullptr;
    int m_width = 0, m_height = 0;

    std::unordered_map<glm::ivec3, ChunkMesh> m_meshes;
};
