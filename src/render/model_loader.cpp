#include "render/model_loader.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <cfloat>

bool load_mesh_file(const char* path, LoadedMesh& out)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        SDL_Log("load_mesh_file: cannot open '%s'", path);
        return false;
    }

    // Magic
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "MESH", 4) != 0) {
        SDL_Log("load_mesh_file: bad magic in '%s'", path);
        fclose(f);
        return false;
    }

    uint32_t num_verts = 0, num_idx = 0;
    if (fread(&num_verts, 4, 1, f) != 1 || fread(&num_idx, 4, 1, f) != 1) {
        SDL_Log("load_mesh_file: truncated header in '%s'", path);
        fclose(f);
        return false;
    }

    out.vertices.resize(num_verts);
    out.indices.resize(num_idx);

    if (fread(out.vertices.data(), sizeof(ModelVertex), num_verts, f) != num_verts) {
        SDL_Log("load_mesh_file: truncated vertex data in '%s'", path);
        out.vertices.clear();
        fclose(f);
        return false;
    }

    if (fread(out.indices.data(), sizeof(uint32_t), num_idx, f) != num_idx) {
        SDL_Log("load_mesh_file: truncated index data in '%s'", path);
        out.vertices.clear();
        out.indices.clear();
        fclose(f);
        return false;
    }

    fclose(f);

    // Compute local-space AABB
    if (!out.vertices.empty()) {
        out.local_min = { FLT_MAX,  FLT_MAX,  FLT_MAX };
        out.local_max = {-FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (const auto& v : out.vertices) {
            if (v.x < out.local_min.x) out.local_min.x = v.x;
            if (v.y < out.local_min.y) out.local_min.y = v.y;
            if (v.z < out.local_min.z) out.local_min.z = v.z;
            if (v.x > out.local_max.x) out.local_max.x = v.x;
            if (v.y > out.local_max.y) out.local_max.y = v.y;
            if (v.z > out.local_max.z) out.local_max.z = v.z;
        }
    }

    SDL_Log("load_mesh_file: '%s' \u2014 %u verts, %u indices  aabb (%.2f %.2f %.2f)-(%.2f %.2f %.2f)",
            path, num_verts, num_idx,
            out.local_min.x, out.local_min.y, out.local_min.z,
            out.local_max.x, out.local_max.y, out.local_max.z);
    return true;
}
