#include "render/chunk_mesher.h"
#include <cstring>

ChunkMesher::ChunkMesher()  = default;
ChunkMesher::~ChunkMesher() { stop(); }

void ChunkMesher::start(int num_workers)
{
    m_running = true;
    for (int i = 0; i < num_workers; ++i)
        m_workers.emplace_back([this] { worker_loop(); });
}

void ChunkMesher::stop()
{
    {
        std::lock_guard<std::mutex> lk(m_queue_mutex);
        m_running = false;
    }
    m_cv.notify_all();
    for (auto& t : m_workers) if (t.joinable()) t.join();
    m_workers.clear();
}

void ChunkMesher::enqueue(glm::ivec3 chunk_pos, const World& world)
{
    Job job;
    job.chunk_pos = chunk_pos;

    // Take a snapshot of the chunk's voxel data
    const Chunk* chunk = const_cast<World&>(world).get_chunk(chunk_pos);
    if (!chunk) return;
    for (int z = 0; z < CHUNK_SIZE; ++z)
        for (int y = 0; y < CHUNK_SIZE; ++y)
            for (int x = 0; x < CHUNK_SIZE; ++x)
                job.voxels[z * CHUNK_SIZE * CHUNK_SIZE + y * CHUNK_SIZE + x]
                    = chunk->get(x, y, z);

    // Snapshot per-face atlas indices so the worker thread can select the
    // correct texture layer without touching the registry.
    if (m_registry) {
        const auto& all = m_registry->all();
        uint16_t max_id = 0;
        for (const auto& [id, def] : all)
            if (id > max_id) max_id = id;
        job.type_atlas.assign(
            static_cast<size_t>(max_id) + 1,
            std::array<uint16_t, static_cast<int>(FaceDir::COUNT)>{});
        for (const auto& [id, def] : all)
            job.type_atlas[id] = def.atlas_indices;
    }

    {
        std::lock_guard<std::mutex> lk(m_queue_mutex);
        m_job_queue.push(std::move(job));
    }
    m_cv.notify_one();
}

std::vector<ChunkMesh> ChunkMesher::collect_finished()
{
    std::lock_guard<std::mutex> lk(m_finished_mutex);
    std::vector<ChunkMesh> result;
    result.swap(m_finished);
    return result;
}

void ChunkMesher::worker_loop()
{
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(m_queue_mutex);
            m_cv.wait(lk, [this] { return !m_running || !m_job_queue.empty(); });
            if (!m_running && m_job_queue.empty()) return;
            job = std::move(m_job_queue.front());
            m_job_queue.pop();
        }
        // Build mesh using a dummy world view — full World ref not available here.
        // In a full implementation build_mesh would use the snapshot in job.
        ChunkMesh mesh;
        mesh.chunk_pos = job.chunk_pos;
        mesh.dirty     = false;

        // ── Face geometry table ───────────────────────────────────────────────
        // Each entry: 4 vertex offsets (relative to voxel corner) + outward normal.
        // Vertex order is CCW when viewed from the face's outward normal direction
        // (with cull-mode BACK).
        struct FaceGeo {
            float v[4][3];  // four corners, relative to voxel (x,y,z)
            float n[3];     // outward unit normal
        };
        static const FaceGeo k_faces[6] = {
            // PosX (right face, at x+1): normal +X
            { { {1,0,1},{1,0,0},{1,1,0},{1,1,1} }, { 1, 0, 0} },
            // NegX (left face, at x):   normal -X
            { { {0,1,1},{0,1,0},{0,0,0},{0,0,1} }, {-1, 0, 0} },
            // PosY (top face, at y+1):  normal +Y
            { { {0,1,1},{1,1,1},{1,1,0},{0,1,0} }, { 0, 1, 0} },
            // NegY (bottom face, at y): normal -Y
            { { {0,0,0},{1,0,0},{1,0,1},{0,0,1} }, { 0,-1, 0} },
            // PosZ (front face, at z+1):normal +Z
            { { {0,0,1},{1,0,1},{1,1,1},{0,1,1} }, { 0, 0, 1} },
            // NegZ (back face, at z):   normal -Z
            { { {1,0,0},{0,0,0},{0,1,0},{1,1,0} }, { 0, 0,-1} },
        };
        // Neighbour sample offsets per face
        static const int k_offsets[6][3] = {
            { 1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
        };

        auto coord = [](int x, int y, int z) {
            return z * CHUNK_SIZE * CHUNK_SIZE + y * CHUNK_SIZE + x;
        };
        auto at = [&](int x, int y, int z) -> const Voxel& {
            return job.voxels[coord(x, y, z)];
        };
        auto in_bounds = [](int x, int y, int z) {
            return x >= 0 && x < CHUNK_SIZE &&
                   y >= 0 && y < CHUNK_SIZE &&
                   z >= 0 && z < CHUNK_SIZE;
        };

        // Helper: return the atlas layer index for a voxel face.
        // Falls back to type_id (the old behaviour) when no atlas table is available.
        auto atlas_idx = [&](uint16_t type_id, int face_dir) -> float {
            if (!job.type_atlas.empty() &&
                static_cast<size_t>(type_id) < job.type_atlas.size())
                return static_cast<float>(job.type_atlas[type_id][face_dir]);
            return static_cast<float>(type_id);
        };

        // Flat-plane geometry: top (+Y) and bottom (-Y) faces at the floor of the cell (y=0)
        // Used for catwalk, grating, etc.  Double-sided so visible from above and below.
        static const FaceGeo k_flat[2] = {
            // top  face at y=0, normal +Y (CCW from above)
            { { {0,0,1},{1,0,1},{1,0,0},{0,0,0} }, { 0, 1, 0} },
            // bottom face at y=0, normal -Y (CCW from below)
            { { {0,0,0},{1,0,0},{1,0,1},{0,0,1} }, { 0,-1, 0} },
        };
        static const int k_flat_n[2][3] = {{0,1,0},{0,-1,0}};

        static const float k_uvs[4][2] = {
            {0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}
        };

        for (int z = 0; z < CHUNK_SIZE; ++z)
        for (int y = 0; y < CHUNK_SIZE; ++y)
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            const Voxel& v = at(x, y, z);
            if (v.type_id == 0) continue;

            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);
            const float fz = static_cast<float>(z);
            const bool is_flat = (v.flags & (VFLAG_FLAT_PLANE | VFLAG_FLAT_TOP)) != 0;

            // ── Flat-plane voxel (catwalk / grating) ──────────────────────────
            if (is_flat) {
                // plane_off: 0 = bottom of cell (FLAT_PLANE), 1 = top of cell (FLAT_TOP)
                const float plane_off = (v.flags & VFLAG_FLAT_TOP) ? 1.f : 0.f;
                for (int fi = 0; fi < 2; ++fi) {
                    int nx2 = x + k_flat_n[fi][0];
                    int ny2 = y + k_flat_n[fi][1];
                    int nz2 = z + k_flat_n[fi][2];
                    // Cull only against solid cube neighbours (not flat planes or doors)
                    bool blocked = in_bounds(nx2, ny2, nz2)
                                && (at(nx2, ny2, nz2).type_id != 0)
                                && (at(nx2, ny2, nz2).flags & VFLAG_OPAQUE)         // transparent neighbours never cull
                                && !(at(nx2, ny2, nz2).flags & (VFLAG_FLAT_PLANE | VFLAG_FLAT_TOP | VFLAG_VERT_PLANE_Z));
                    if (blocked) continue;
                    const FaceGeo& fg = k_flat[fi];
                    // Flat planes are horizontal — use PosY (top) or NegY (bottom) atlas slot
                    const float tex_idx = atlas_idx(
                        v.type_id,
                        fi == 0 ? static_cast<int>(FaceDir::PosY)
                                : static_cast<int>(FaceDir::NegY));
                    uint32_t base2 = static_cast<uint32_t>(mesh.vertices.size() / 9);
                    for (int vi = 0; vi < 4; ++vi) {
                        mesh.vertices.push_back(fx + fg.v[vi][0]);
                        mesh.vertices.push_back(fy + fg.v[vi][1] + plane_off);
                        mesh.vertices.push_back(fz + fg.v[vi][2]);
                        mesh.vertices.push_back(fg.n[0]);
                        mesh.vertices.push_back(fg.n[1]);
                        mesh.vertices.push_back(fg.n[2]);
                        mesh.vertices.push_back(k_uvs[vi][0]);
                        mesh.vertices.push_back(k_uvs[vi][1]);
                        mesh.vertices.push_back(tex_idx);
                    }
                    mesh.indices.insert(mesh.indices.end(), {
                        base2, base2+1, base2+2,
                        base2, base2+2, base2+3
                    });
                }
                continue;  // skip cube meshing
            }

            // ── Vertical-plane door (VFLAG_VERT_PLANE_Z) ──────────────────────
            // Double-sided plane at z=0.5 of the cell, spanning full X and Y.
            // Visible from both +Z and -Z so the door shows from either side.
            if (v.flags & VFLAG_VERT_PLANE_Z) {
                // Door plane geometry: two faces at fz+0.5, spanning x=0..1, y=0..1
                // Vertex order places y=1 (top) at UV (0,0) so the texture is right-side-up.
                static const FaceGeo k_door[2] = {
                    // NegZ face: top-left=(0,1), top-right=(1,1), bot-right=(1,0), bot-left=(0,0)
                    // CCW from -Z view → normal -Z; UV (0,0)=top-left in texture space
                    { { {0,1,0.5f},{1,1,0.5f},{1,0,0.5f},{0,0,0.5f} }, { 0, 0,-1} },
                    // PosZ face: looking from +Z, left=+X; top-left=(1,1), top-right=(0,1)
                    { { {1,1,0.5f},{0,1,0.5f},{0,0,0.5f},{1,0,0.5f} }, { 0, 0, 1} },
                };
                for (int fi = 0; fi < 2; ++fi) {
                    const FaceGeo& fg = k_door[fi];
                    // Door is a Z-facing plane — use NegZ or PosZ atlas slot
                    const float tex_idx = atlas_idx(
                        v.type_id,
                        fi == 0 ? static_cast<int>(FaceDir::NegZ)
                                : static_cast<int>(FaceDir::PosZ));
                    uint32_t base_d = static_cast<uint32_t>(mesh.vertices.size() / 9);
                    for (int vi = 0; vi < 4; ++vi) {
                        mesh.vertices.push_back(fx + fg.v[vi][0]);
                        mesh.vertices.push_back(fy + fg.v[vi][1]);
                        mesh.vertices.push_back(fz + fg.v[vi][2]);
                        mesh.vertices.push_back(fg.n[0]);
                        mesh.vertices.push_back(fg.n[1]);
                        mesh.vertices.push_back(fg.n[2]);
                        mesh.vertices.push_back(k_uvs[vi][0]);
                        mesh.vertices.push_back(k_uvs[vi][1]);
                        mesh.vertices.push_back(tex_idx);
                    }
                    mesh.indices.insert(mesh.indices.end(), {
                        base_d, base_d+1, base_d+2,
                        base_d, base_d+2, base_d+3
                    });
                }
                continue;  // skip cube meshing
            }

            for (int f = 0; f < 6; ++f) {
                int nx = x + k_offsets[f][0];
                int ny = y + k_offsets[f][1];
                int nz = z + k_offsets[f][2];
                // Flat-plane and door neighbours don't occlude cube faces
                bool neighbour_solid = in_bounds(nx, ny, nz)
                                       && (at(nx, ny, nz).type_id != 0)
                                       && (at(nx, ny, nz).flags & VFLAG_OPAQUE)         // transparent neighbours never cull
                                       && !(at(nx, ny, nz).flags & (VFLAG_FLAT_PLANE | VFLAG_FLAT_TOP | VFLAG_VERT_PLANE_Z));
                if (neighbour_solid) continue;  // face hidden

                const FaceGeo& fg = k_faces[f];
                // 9 floats per vertex: pos(3) + normal(3) + uv(2) + texIndex(1)
                // Standard quad UVs: v0=(0,0), v1=(1,0), v2=(1,1), v3=(0,1)
                const float tex_idx = atlas_idx(v.type_id, f);
                uint32_t base = static_cast<uint32_t>(mesh.vertices.size() / 9);

                for (int vi = 0; vi < 4; ++vi) {
                    mesh.vertices.push_back(fx + fg.v[vi][0]);
                    mesh.vertices.push_back(fy + fg.v[vi][1]);
                    mesh.vertices.push_back(fz + fg.v[vi][2]);
                    mesh.vertices.push_back(fg.n[0]);
                    mesh.vertices.push_back(fg.n[1]);
                    mesh.vertices.push_back(fg.n[2]);
                    mesh.vertices.push_back(k_uvs[vi][0]);  // u
                    mesh.vertices.push_back(k_uvs[vi][1]);  // v
                    mesh.vertices.push_back(tex_idx);        // texIndex (= type_id)
                }                mesh.indices.insert(mesh.indices.end(), {
                    base, base+1, base+2,
                    base, base+2, base+3
                });
            }
        }

        {
            std::lock_guard<std::mutex> lk(m_finished_mutex);
            m_finished.push_back(std::move(mesh));
        }
    }
}

ChunkMesh ChunkMesher::build_mesh(glm::ivec3 /*chunk_pos*/, const World& /*world*/)
{
    // Called from main thread path (synchronous). Worker path uses the Job snapshot.
    return {};
}
