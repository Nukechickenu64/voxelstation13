#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <glm/glm.hpp>

// Vertex layout matching the world pipeline:
//   pos   (3×f32, offset  0)
//   normal(3×f32, offset 12)
//   uv    (2×f32, offset 24)
//   texIdx(1×f32, offset 32) ← always 0.0 for models
// stride = 36 bytes
struct ModelVertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
    float tex_idx;  // always 0.0 for single-texture models
};

struct LoadedMesh {
    std::vector<ModelVertex> vertices;
    std::vector<uint32_t>    indices;
    // Local-space axis-aligned bounding box (computed by load_mesh_file)
    glm::vec3 local_min{ 0.f, 0.f, 0.f };
    glm::vec3 local_max{ 0.f, 0.f, 0.f };
};

// Load a .mesh file written by tools/fbx_to_mesh.py.
// Returns true on success, populates 'out'.
bool load_mesh_file(const char* path, LoadedMesh& out);
