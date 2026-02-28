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
    job.chunk_pos  = chunk_pos;
    job.generation = m_generation.load(std::memory_order_relaxed);

    // Take a snapshot of the chunk's voxel data
    const Chunk* chunk = const_cast<World&>(world).get_chunk(chunk_pos);
    if (!chunk) return;
    for (int z = 0; z < CHUNK_SIZE; ++z)
        for (int y = 0; y < CHUNK_SIZE; ++y)
            for (int x = 0; x < CHUNK_SIZE; ++x)
                job.voxels[z * CHUNK_SIZE * CHUNK_SIZE + y * CHUNK_SIZE + x]
                    = chunk->get(x, y, z);

    // Snapshot the 4 horizontal neighbours so cross-chunk boundary lookups
    // work correctly (e.g. doors sitting on a chunk seam).
    static const glm::ivec3 k_nb_off[4] = {
        { 1,0,0},{-1,0,0},{0,0, 1},{0,0,-1}
    };
    for (int ni = 0; ni < 4; ++ni) {
        const Chunk* nb = const_cast<World&>(world).get_chunk(chunk_pos + k_nb_off[ni]);
        if (nb) {
            job.neighbours[ni].present = true;
            for (int nz = 0; nz < CHUNK_SIZE; ++nz)
                for (int ny = 0; ny < CHUNK_SIZE; ++ny)
                    for (int nx = 0; nx < CHUNK_SIZE; ++nx)
                        job.neighbours[ni].voxels[nz*CHUNK_SIZE*CHUNK_SIZE + ny*CHUNK_SIZE + nx]
                            = nb->get(nx, ny, nz);
        }
    }

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
    const uint64_t cur_gen = m_generation.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(m_finished_mutex);
    std::vector<ChunkMesh> result;
    for (auto& m : m_finished)
        if (m.generation == cur_gen)
            result.push_back(std::move(m));
    m_finished.clear();
    return result;
}

void ChunkMesher::flush()
{
    // Advance generation so any in-flight worker results are discarded
    // by collect_finished() once they land.
    m_generation.fetch_add(1, std::memory_order_relaxed);
    // Discard jobs still waiting in the queue (no point meshing the old world).
    {
        std::lock_guard<std::mutex> lk(m_queue_mutex);
        while (!m_job_queue.empty()) m_job_queue.pop();
    }
    // Drain any results that already completed before the generation bump.
    {
        std::lock_guard<std::mutex> lk(m_finished_mutex);
        m_finished.clear();
    }
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

        // Cross-chunk voxel sampler — handles ±1 steps outside the chunk in
        // X and Z by redirecting to the pre-snapshotted neighbour data.
        // Returns a static air voxel for anything out of reach.
        static const Voxel k_air_voxel{};
        auto sample = [&](int sx, int sy, int sz) -> const Voxel& {
            if (sy < 0 || sy >= CHUNK_SIZE) return k_air_voxel;
            if (sx >= 0 && sx < CHUNK_SIZE && sz >= 0 && sz < CHUNK_SIZE)
                return job.voxels[coord(sx, sy, sz)];
            // One step into a horizontal neighbour
            if (sx == -1) {
                const auto& nb = job.neighbours[1]; // NegX
                return nb.present ? nb.voxels[coord(CHUNK_SIZE-1, sy, sz)] : k_air_voxel;
            }
            if (sx == CHUNK_SIZE) {
                const auto& nb = job.neighbours[0]; // PosX
                return nb.present ? nb.voxels[coord(0, sy, sz)] : k_air_voxel;
            }
            if (sz == -1) {
                const auto& nb = job.neighbours[3]; // NegZ
                return nb.present ? nb.voxels[coord(sx, sy, CHUNK_SIZE-1)] : k_air_voxel;
            }
            if (sz == CHUNK_SIZE) {
                const auto& nb = job.neighbours[2]; // PosZ
                return nb.present ? nb.voxels[coord(sx, sy, 0)] : k_air_voxel;
            }
            return k_air_voxel;
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
        // NegX face (f==1) has its y-vertices in reverse order relative to all
        // other side faces, causing the texture to appear upside down.  Flip V
        // for that face only so every cube wall shares the same orientation.
        static const bool k_face_flip_v[6] = {
            false, // PosX
            true,  // NegX — fix upside-down texture
            false, // PosY
            false, // NegY
            false, // PosZ
            false, // NegZ
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

            // ── Auto-oriented door (VFLAG_VERT_PLANE_Z) ─────────────────────
            // A door covers the open (non-wall) sides of its cube hitbox with
            // full-face panels sitting flush on the cube boundary — exactly
            // where the face would be if it were a solid cube, but only for
            // the directions that face air (no solid opaque wall there).
            //
            // Each panel is rendered double-sided (two back-to-back quads) so
            // it is visible from both inside and outside the door cell.
            //
            // Examples:
            //   Corridor along Z, walls on ±X  → NegZ + PosZ panels
            //   Corridor along X, walls on ±Z  → NegX + PosX panels
            //   4-way intersection (all air)   → all four panels
            if (v.flags & VFLAG_VERT_PLANE_Z) {
                // A neighbour hides a panel when it is:
                //   • a solid opaque cube (wall), OR
                //   • another door voxel (adjacent doors share a face boundary)
                // Uses the cross-chunk `sample` so doors on a chunk seam see
                // the correct neighbour even when it is in a different chunk.
                auto is_wall_nb = [&](int nx_, int ny_, int nz_) -> bool {
                    const Voxel& nb = sample(nx_, ny_, nz_);
                    if (nb.type_id == 0) return false;
                    // Adjacent door — shared face, hide it
                    if (nb.flags & VFLAG_VERT_PLANE_Z) return true;
                    // Solid opaque cube (wall / reinforced wall / etc.)
                    return (nb.flags & VFLAG_OPAQUE)
                        && !(nb.flags & (VFLAG_FLAT_PLANE | VFLAG_FLAT_TOP));
                };

                const bool wall_px = is_wall_nb(x+1, y, z);
                const bool wall_nx = is_wall_nb(x-1, y, z);
                const bool wall_pz = is_wall_nb(x, y, z+1);
                const bool wall_nz = is_wall_nb(x, y, z-1);

                // Each entry: outward-normal quad, reversed-normal quad (for
                // double-sidedness), atlas direction, and whether to emit.
                struct DoorPanel {
                    FaceGeo front;   // CCW from outside
                    FaceGeo back;    // CCW from inside (same plane, flipped winding)
                    FaceDir atlas_dir;
                    bool    emit;
                };

                // Face positions sit flush on the cube boundary.
                // Vertex winding puts y=1 (top) at UV.v=0, matching the door
                // texture orientation established by the original door code.
                DoorPanel panels[4] = {
                    // NegZ face (z=0): outward normal -Z, inward normal +Z
                    {
                        { { {0,1,0},{1,1,0},{1,0,0},{0,0,0} }, { 0, 0,-1} },
                        { { {1,1,0},{0,1,0},{0,0,0},{1,0,0} }, { 0, 0, 1} },
                        FaceDir::NegZ, !wall_nz
                    },
                    // PosZ face (z=1): outward normal +Z, inward normal -Z
                    {
                        { { {1,1,1},{0,1,1},{0,0,1},{1,0,1} }, { 0, 0, 1} },
                        { { {0,1,1},{1,1,1},{1,0,1},{0,0,1} }, { 0, 0,-1} },
                        FaceDir::PosZ, !wall_pz
                    },
                    // NegX face (x=0): outward normal -X, inward normal +X
                    {
                        { { {0,1,1},{0,1,0},{0,0,0},{0,0,1} }, {-1, 0, 0} },
                        { { {0,1,0},{0,1,1},{0,0,1},{0,0,0} }, { 1, 0, 0} },
                        FaceDir::NegX, !wall_nx
                    },
                    // PosX face (x=1): outward normal +X, inward normal -X
                    {
                        { { {1,1,0},{1,1,1},{1,0,1},{1,0,0} }, { 1, 0, 0} },
                        { { {1,1,1},{1,1,0},{1,0,0},{1,0,1} }, {-1, 0, 0} },
                        FaceDir::PosX, !wall_px
                    },
                };

                // Fallback: if somehow fully enclosed, show ±Z so door is
                // never invisible.
                const bool any_open = !wall_nz || !wall_pz || !wall_nx || !wall_px;
                if (!any_open) { panels[0].emit = true; panels[1].emit = true; }

                auto emit_quad = [&](const FaceGeo& fg, float tex_idx) {
                    uint32_t base_d =
                        static_cast<uint32_t>(mesh.vertices.size() / 9);
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
                };

                for (const auto& panel : panels) {
                    if (!panel.emit) continue;
                    const float tex_idx =
                        atlas_idx(v.type_id, static_cast<int>(panel.atlas_dir));
                    emit_quad(panel.front, tex_idx);
                    emit_quad(panel.back,  tex_idx);
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
                const bool  flip_v  = k_face_flip_v[f];
                uint32_t base = static_cast<uint32_t>(mesh.vertices.size() / 9);

                for (int vi = 0; vi < 4; ++vi) {
                    mesh.vertices.push_back(fx + fg.v[vi][0]);
                    mesh.vertices.push_back(fy + fg.v[vi][1]);
                    mesh.vertices.push_back(fz + fg.v[vi][2]);
                    mesh.vertices.push_back(fg.n[0]);
                    mesh.vertices.push_back(fg.n[1]);
                    mesh.vertices.push_back(fg.n[2]);
                    mesh.vertices.push_back(k_uvs[vi][0]);                        // u
                    mesh.vertices.push_back(flip_v ? 1.0f - k_uvs[vi][1]
                                                   : k_uvs[vi][1]);               // v
                    mesh.vertices.push_back(tex_idx);                              // texIndex (= type_id)
                }                mesh.indices.insert(mesh.indices.end(), {
                    base, base+1, base+2,
                    base, base+2, base+3
                });
            }
        }

        {
            std::lock_guard<std::mutex> lk(m_finished_mutex);
            mesh.generation = job.generation;
            m_finished.push_back(std::move(mesh));
        }
    }
}

ChunkMesh ChunkMesher::build_mesh(glm::ivec3 /*chunk_pos*/, const World& /*world*/)
{
    // Called from main thread path (synchronous). Worker path uses the Job snapshot.
    return {};
}
