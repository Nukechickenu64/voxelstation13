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
            { { {1,0,0},{1,0,1},{1,1,1},{1,1,0} }, { 1, 0, 0} },
            // NegX (left face, at x):   normal -X
            { { {0,0,1},{0,0,0},{0,1,0},{0,1,1} }, {-1, 0, 0} },
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

        for (int z = 0; z < CHUNK_SIZE; ++z)
        for (int y = 0; y < CHUNK_SIZE; ++y)
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            const Voxel& v = at(x, y, z);
            if (v.type_id == 0) continue;

            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);
            const float fz = static_cast<float>(z);

            for (int f = 0; f < 6; ++f) {
                int nx = x + k_offsets[f][0];
                int ny = y + k_offsets[f][1];
                int nz = z + k_offsets[f][2];
                bool neighbour_solid = in_bounds(nx, ny, nz)
                                       && (at(nx, ny, nz).type_id != 0);
                if (neighbour_solid) continue;  // face hidden

                const FaceGeo& fg = k_faces[f];
                // 6 floats per vertex: pos(3) + normal(3)
                uint32_t base = static_cast<uint32_t>(mesh.vertices.size() / 6);

                for (int vi = 0; vi < 4; ++vi) {
                    mesh.vertices.push_back(fx + fg.v[vi][0]);
                    mesh.vertices.push_back(fy + fg.v[vi][1]);
                    mesh.vertices.push_back(fz + fg.v[vi][2]);
                    mesh.vertices.push_back(fg.n[0]);
                    mesh.vertices.push_back(fg.n[1]);
                    mesh.vertices.push_back(fg.n[2]);
                }
                mesh.indices.insert(mesh.indices.end(), {
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
