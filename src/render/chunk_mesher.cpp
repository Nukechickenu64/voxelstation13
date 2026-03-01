#include "render/chunk_mesher.h"
#include <cstring>

ChunkMesher::ChunkMesher()  = default;
ChunkMesher::~ChunkMesher() { stop(); }

void ChunkMesher::set_registry(const VoxelRegistry* reg)
{
    m_registry = reg;
    m_cached_atlas.reset(); // will be rebuilt lazily on next enqueue
    m_cached_bitmask_atlas.reset();
    if (m_registry) rebuild_atlas();
}

void ChunkMesher::rebuild_atlas()
{
    if (!m_registry) {
        m_cached_atlas.reset();
        m_cached_bitmask_atlas.reset();
        return;
    }
    const auto& all = m_registry->all();
    uint16_t max_id = 0;
    for (const auto& [id, def] : all)
        if (id > max_id) max_id = id;
    auto atlas = std::make_shared<AtlasTable>(
        static_cast<size_t>(max_id) + 1,
        std::array<uint16_t, static_cast<int>(FaceDir::COUNT)>{});
    for (const auto& [id, def] : all)
        (*atlas)[id] = def.atlas_indices;
    m_cached_atlas = std::move(atlas);

    // Build per-type bitmask connectivity atlas (base layer for VFLAG_BITMASK_FLAT types).
    auto bm = std::make_shared<BitmaskAtlasTable>(static_cast<size_t>(max_id) + 1, 0u);
    for (const auto& [id, def] : all)
        if (def.bitmask_count > 0 && def.bitmask_atlas_base > 0)
            (*bm)[id] = def.bitmask_atlas_base;
    m_cached_bitmask_atlas = std::move(bm);

    // Rebuild the per-type emissive flag table (true = voxel emits light).
    auto emit = std::make_shared<EmitTable>(static_cast<size_t>(max_id) + 1, false);
    for (const auto& [id, def] : all)
        if (def.emit_light > 0)
            (*emit)[id] = true;
    m_emit_table = std::move(emit);
}

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

    // Reuse the cached atlas (built once in set_registry) — pointer copy only.
    job.type_atlas    = m_cached_atlas;
    job.bitmask_atlas = m_cached_bitmask_atlas;
    job.emit_table    = m_emit_table;

    // Take a fresh LightMap snapshot for this single-chunk enqueue.
    if (m_lighting) {
        job.light_colors = std::make_shared<LightColors>(m_lighting->light_map().data());
    }

    {
        std::lock_guard<std::mutex> lk(m_queue_mutex);
        // Skip if this chunk is already queued — the existing snapshot will be
        // processed soon enough, and a second copy wastes both memory and CPU.
        if (m_pending.count(chunk_pos)) return;
        m_pending.insert(chunk_pos);
        m_job_queue.push(std::move(job));
    }
    m_cv.notify_one();
}

void ChunkMesher::enqueue_batch(const std::vector<Chunk*>& chunks, const World& world)
{
    if (chunks.empty()) return;

    // One atlas pointer (already cached), one LightMap snapshot shared by all jobs.
    auto shared_lightmap = m_lighting
        ? std::make_shared<LightColors>(m_lighting->light_map().data())
        : std::shared_ptr<LightColors>{};

    static const glm::ivec3 k_nb_off[4] = {
        { 1,0,0},{-1,0,0},{0,0, 1},{0,0,-1}
    };

    const uint64_t gen = m_generation.load(std::memory_order_relaxed);
    int notified = 0;

    for (Chunk* chunk : chunks) {
        if (!chunk) continue;
        glm::ivec3 chunk_pos = chunk->chunk_pos();

        Job job;
        job.chunk_pos  = chunk_pos;
        job.generation = gen;
        job.type_atlas    = m_cached_atlas;   // shared — pointer copy
        job.bitmask_atlas = m_cached_bitmask_atlas; // shared — pointer copy
        job.light_colors  = shared_lightmap;  // shared — pointer copy
        job.emit_table    = m_emit_table;     // shared — pointer copy

        for (int z = 0; z < CHUNK_SIZE; ++z)
            for (int y = 0; y < CHUNK_SIZE; ++y)
                for (int x = 0; x < CHUNK_SIZE; ++x)
                    job.voxels[z*CHUNK_SIZE*CHUNK_SIZE + y*CHUNK_SIZE + x]
                        = chunk->get(x, y, z);

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

        {
            std::lock_guard<std::mutex> lk(m_queue_mutex);
            if (m_pending.count(chunk_pos)) continue;
            m_pending.insert(chunk_pos);
            m_job_queue.push(std::move(job));
        }
        m_cv.notify_one();
        ++notified;
    }
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
        m_pending.clear();
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
            // Free the pending slot so a new snapshot can be enqueued if the
            // chunk changes again while we are processing this one.
            m_pending.erase(job.chunk_pos);
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
            // Diagonal corner: both X and Z out-of-bounds — treat as air
            {
                int oob_xz = (sx < 0 || sx >= CHUNK_SIZE ? 1 : 0)
                           + (sz < 0 || sz >= CHUNK_SIZE ? 1 : 0);
                if (oob_xz > 1) return k_air_voxel;
            }
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
            if (job.type_atlas && !job.type_atlas->empty() &&
                static_cast<size_t>(type_id) < job.type_atlas->size())
                return static_cast<float>((*job.type_atlas)[type_id][face_dir]);
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
        // PosX, PosZ, NegZ faces start their vertex list at y=0 (bottom of the
        // voxel), which maps to v=0 (top of the texture), making the texture
        // appear upside down.  NegX uniquely starts at y=1 and is already
        // correct.  Flip V for the three affected side faces.
        static const bool k_face_flip_v[6] = {
            true,  // PosX — fix upside-down texture
            false, // NegX — already correct (starts at y=1)
            false, // PosY
            false, // NegY
            true,  // PosZ — fix upside-down texture
            true,  // NegZ — fix upside-down texture
        };

        // ── Vertex ambient occlusion ──────────────────────────────────────────
        // Each face has an outer direction and two tangent axes.
        // 3 samples around each vertex corner determine how occluded it is.
        struct AoFaceInfo { int outer[3]; int t1, t2; };
        static const AoFaceInfo k_ao_faces[6] = {
            { { 1, 0, 0}, 1, 2 },  // PosX: outer +X, tangents Y,Z
            { {-1, 0, 0}, 1, 2 },  // NegX: outer -X, tangents Y,Z
            { { 0, 1, 0}, 0, 2 },  // PosY: outer +Y, tangents X,Z
            { { 0,-1, 0}, 0, 2 },  // NegY: outer -Y, tangents X,Z
            { { 0, 0, 1}, 0, 1 },  // PosZ: outer +Z, tangents X,Y
            { { 0, 0,-1}, 0, 1 },  // NegZ: outer -Z, tangents X,Y
        };

        auto ao_opaque = [&](int sx, int sy, int sz) -> bool {
            if (sy < 0 || sy >= CHUNK_SIZE) return false;
            int oob_xz = (sx < 0 || sx >= CHUNK_SIZE ? 1 : 0)
                       + (sz < 0 || sz >= CHUNK_SIZE ? 1 : 0);
            if (oob_xz > 1) return false;
            const Voxel& vox = sample(sx, sy, sz);
            return (vox.type_id != 0)
                && (vox.flags & VFLAG_OPAQUE)
                && !(vox.flags & (VFLAG_FLAT_PLANE | VFLAG_FLAT_TOP | VFLAG_VERT_PLANE_Z));
        };

        auto vertex_ao = [&](int face_idx, const int verts[3], int ox, int oy, int oz) -> float {
            const AoFaceInfo& af = k_ao_faces[face_idx];
            int base[3] = { ox + af.outer[0], oy + af.outer[1], oz + af.outer[2] };
            int step1 = verts[af.t1] == 1 ? 1 : -1;
            int step2 = verts[af.t2] == 1 ? 1 : -1;
            int s1[3] = { base[0], base[1], base[2] }; s1[af.t1] += step1;
            int s2[3] = { base[0], base[1], base[2] }; s2[af.t2] += step2;
            int  c[3] = { base[0], base[1], base[2] };  c[af.t1] += step1; c[af.t2] += step2;
            int blocked = (int)ao_opaque(s1[0],s1[1],s1[2])
                        + (int)ao_opaque(s2[0],s2[1],s2[2])
                        + (int)ao_opaque( c[0], c[1], c[2]);
            return 1.0f - blocked * 0.25f;
        };

        // Per-vertex smooth colored light: sample the 4 corners around a vertex
        // position, average their light_level and color.  This gives smooth
        // gradient lighting (Minecraft-style) with full color tinting.
        auto vertex_light_color = [&](int face_idx, const int verts[3],
                                       int ox, int oy, int oz) -> glm::vec3 {
            const AoFaceInfo& af = k_ao_faces[face_idx];
            // Base position in the exposed neighbor cell (one step in outer dir)
            int bx = ox + af.outer[0];
            int by = oy + af.outer[1];
            int bz = oz + af.outer[2];
            int step1 = verts[af.t1] == 1 ? 1 : -1;
            int step2 = verts[af.t2] == 1 ? 1 : -1;
            // Four cells around this vertex corner: base, +t1, +t2, +t1+t2
            int sx0 = bx, sy0 = by, sz0 = bz;
            int sx1 = bx, sy1 = by, sz1 = bz; sx1 += (af.t1==0?step1:0); sy1 += (af.t1==1?step1:0); sz1 += (af.t1==2?step1:0);
            int sx2 = bx, sy2 = by, sz2 = bz; sx2 += (af.t2==0?step2:0); sy2 += (af.t2==1?step2:0); sz2 += (af.t2==2?step2:0);
            int sx3 = sx1, sy3 = sy1, sz3 = sz1; sx3 += (af.t2==0?step2:0); sy3 += (af.t2==1?step2:0); sz3 += (af.t2==2?step2:0);

            // Sample light_level at each corner
            auto lv = [&](int sx, int sy, int sz) -> float {
                return sample(sx, sy, sz).light_level / 15.0f;
            };
            // Look up color for a world-space position
            glm::ivec3 cp3 = job.chunk_pos * CHUNK_SIZE;
            auto lc = [&](int sx, int sy, int sz) -> glm::vec3 {
                glm::ivec3 wp = cp3 + glm::ivec3(sx, sy, sz);
                if (job.light_colors) {
                    auto it = job.light_colors->find(wp);
                    if (it != job.light_colors->end()) {
                        return { it->second.r / 255.0f, it->second.g / 255.0f, it->second.b / 255.0f };
                    }
                }
                return { 1.0f, 1.0f, 1.0f };
            };

            float l0 = lv(sx0,sy0,sz0), l1 = lv(sx1,sy1,sz1);
            float l2 = lv(sx2,sy2,sz2), l3 = lv(sx3,sy3,sz3);
            float lsum = l0 + l1 + l2 + l3;
            if (lsum < 0.0001f) return glm::vec3(0.0f);  // fully dark

            // Weighted average of colors by brightness
            glm::vec3 c0 = lc(sx0,sy0,sz0) * l0;
            glm::vec3 c1 = lc(sx1,sy1,sz1) * l1;
            glm::vec3 c2 = lc(sx2,sy2,sz2) * l2;
            glm::vec3 c3 = lc(sx3,sy3,sz3) * l3;
            glm::vec3 col = (c0 + c1 + c2 + c3) / lsum;

            // Average brightness for the vertex
            float avgL = lsum * 0.25f;
            return col * avgL;
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

            // Emissive voxels are rendered fullbright: light color is overridden to
            // 2.0 (which beats the minimum diffuse factor of 0.55, guaranteeing the
            // output is clamped to full brightness) and AO is suppressed.
            const bool is_emissive = job.emit_table
                && v.type_id < static_cast<uint16_t>(job.emit_table->size())
                && (*job.emit_table)[v.type_id];

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
                    // Flat planes are horizontal — use PosY (top) or NegY (bottom) atlas slot.
                    // VFLAG_BITMASK_FLAT types (wires, pipes) pick sprite by local connectivity
                    // bitmask (1=S, 2=N, 4=E, 8=W) sampled at render time.
                    const float tex_idx = [&]() -> float {
                        if (job.bitmask_atlas
                            && v.type_id < static_cast<uint16_t>(job.bitmask_atlas->size())
                            && (*job.bitmask_atlas)[v.type_id] != 0) {
                            uint8_t mask = 0;
                            auto connects_to_same = [&](int sx, int sy, int sz) -> bool {
                                const Voxel& nb = sample(sx, sy, sz);
                                return nb.type_id == v.type_id;
                            };
                            if (connects_to_same(x,     y, z + 1)) mask |= 1; // south
                            if (connects_to_same(x,     y, z - 1)) mask |= 2; // north
                            if (connects_to_same(x + 1, y, z    )) mask |= 4; // east
                            if (connects_to_same(x - 1, y, z    )) mask |= 8; // west
                            return static_cast<float>(
                                (*job.bitmask_atlas)[v.type_id] + mask);
                        }
                        return atlas_idx(v.type_id,
                            fi == 0 ? static_cast<int>(FaceDir::PosY)
                                    : static_cast<int>(FaceDir::NegY));
                    }();
                    uint32_t base2 = static_cast<uint32_t>(mesh.vertices.size() / 13);
                    for (int vi = 0; vi < 4; ++vi) {
                        const int flat_verts[3] = {
                            static_cast<int>(fg.v[vi][0]),
                            static_cast<int>(fg.v[vi][1]),
                            static_cast<int>(fg.v[vi][2])
                        };
                        glm::vec3 lc = vertex_light_color(fi == 0 ? 2 : 3, flat_verts, x, y, z);
                        if (is_emissive) lc = glm::vec3(2.0f);  // fullbright
                        mesh.vertices.push_back(fx + fg.v[vi][0]);
                        mesh.vertices.push_back(fy + fg.v[vi][1] + plane_off);
                        mesh.vertices.push_back(fz + fg.v[vi][2]);
                        mesh.vertices.push_back(fg.n[0]);
                        mesh.vertices.push_back(fg.n[1]);
                        mesh.vertices.push_back(fg.n[2]);
                        mesh.vertices.push_back(k_uvs[vi][0]);
                        mesh.vertices.push_back(k_uvs[vi][1]);
                        mesh.vertices.push_back(tex_idx);
                        mesh.vertices.push_back(lc.r);  // lightR
                        mesh.vertices.push_back(lc.g);  // lightG
                        mesh.vertices.push_back(lc.b);  // lightB
                        mesh.vertices.push_back(1.0f);  // AO: flat planes get full brightness
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
                        static_cast<uint32_t>(mesh.vertices.size() / 13);
                    // Door cells are solid so the lighting system never floods light
                    // into them — sampling at(x,y,z).light_level always yields 0
                    // (pitch black).  Instead take the brightest neighbour cell so
                    // the door inherits the ambient light of whichever side is lit.
                    glm::vec3 door_lc;
                    {
                        static const glm::ivec3 k_nb6[6] = {
                            {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
                        };
                        float best_lvl = 0.0f;
                        glm::vec3 best_col(1.0f);
                        glm::ivec3 cp3 = job.chunk_pos * CHUNK_SIZE;
                        for (const auto& d : k_nb6) {
                            float lvl = sample(x+d.x, y+d.y, z+d.z).light_level / 15.0f;
                            if (lvl > best_lvl) {
                                best_lvl = lvl;
                                glm::ivec3 wp = cp3 + glm::ivec3(x+d.x, y+d.y, z+d.z);
                                if (job.light_colors) {
                                    auto it = job.light_colors->find(wp);
                                    if (it != job.light_colors->end())
                                        best_col = glm::vec3(it->second.r/255.0f,
                                                             it->second.g/255.0f,
                                                             it->second.b/255.0f);
                                    else
                                        best_col = glm::vec3(1.0f);
                                } else {
                                    best_col = glm::vec3(1.0f);
                                }
                            }
                        }
                        door_lc = best_col * best_lvl;
                        if (is_emissive) door_lc = glm::vec3(2.0f);  // fullbright
                    }
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
                        mesh.vertices.push_back(door_lc.r);  // lightR
                        mesh.vertices.push_back(door_lc.g);  // lightG
                        mesh.vertices.push_back(door_lc.b);  // lightB
                        mesh.vertices.push_back(1.0f);       // AO: doors get full brightness
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
                // Use sample() so cross-chunk boundary neighbours are checked
                // correctly (fixes internal faces at chunk seams).
                // Flat-plane and door neighbours don't occlude cube faces.
                const Voxel& nb_vox = sample(nx, ny, nz);
                bool neighbour_solid = (nb_vox.type_id != 0)
                                       && (nb_vox.flags & VFLAG_OPAQUE)         // transparent neighbours never cull
                                       && !(nb_vox.flags & (VFLAG_FLAT_PLANE | VFLAG_FLAT_TOP | VFLAG_VERT_PLANE_Z));
                if (neighbour_solid) continue;  // face hidden

                const FaceGeo& fg = k_faces[f];
                // 13 floats per vertex: pos(3)+normal(3)+uv(2)+texIndex(1)+lightRGB(3)+ao(1)
                // Standard quad UVs: v0=(0,0), v1=(1,0), v2=(1,1), v3=(0,1)
                const float tex_idx = atlas_idx(v.type_id, f);
                const bool  flip_v  = k_face_flip_v[f];
                uint32_t base = static_cast<uint32_t>(mesh.vertices.size() / 13);

                for (int vi = 0; vi < 4; ++vi) {
                    // Compute per-vertex AO using 3 neighbour samples
                    const int verts[3] = {
                        static_cast<int>(fg.v[vi][0]),
                        static_cast<int>(fg.v[vi][1]),
                        static_cast<int>(fg.v[vi][2])
                    };
                    float ao_val = vertex_ao(f, verts, x, y, z);
                    glm::vec3 lc = vertex_light_color(f, verts, x, y, z);
                    if (is_emissive) { lc = glm::vec3(2.0f); ao_val = 1.0f; }  // fullbright

                    mesh.vertices.push_back(fx + fg.v[vi][0]);
                    mesh.vertices.push_back(fy + fg.v[vi][1]);
                    mesh.vertices.push_back(fz + fg.v[vi][2]);
                    mesh.vertices.push_back(fg.n[0]);
                    mesh.vertices.push_back(fg.n[1]);
                    mesh.vertices.push_back(fg.n[2]);
                    mesh.vertices.push_back(k_uvs[vi][0]);                        // u
                    mesh.vertices.push_back(flip_v ? 1.0f - k_uvs[vi][1]
                                                   : k_uvs[vi][1]);               // v
                    mesh.vertices.push_back(tex_idx);                              // texIndex
                    mesh.vertices.push_back(lc.r);                                 // lightR
                    mesh.vertices.push_back(lc.g);                                 // lightG
                    mesh.vertices.push_back(lc.b);                                 // lightB
                    mesh.vertices.push_back(ao_val);                               // ambient occlusion
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
