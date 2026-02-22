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

        // --- Greedy meshing (simplified scaffold) ---
        // A production implementation iterates each of 6 face directions,
        // builds a 2-D occupancy mask, then sweeps for maximal rectangles.
        // This stub emits one quad per exposed face for correctness.
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

        const int offsets[6][3] = {
            { 1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
        };

        for (int z = 0; z < CHUNK_SIZE; ++z)
        for (int y = 0; y < CHUNK_SIZE; ++y)
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            const Voxel& v = at(x, y, z);
            if (v.type_id == 0) continue;

            for (int f = 0; f < 6; ++f) {
                int nx = x + offsets[f][0];
                int ny = y + offsets[f][1];
                int nz = z + offsets[f][2];
                bool neighbour_solid = in_bounds(nx, ny, nz) &&
                                       (at(nx, ny, nz).type_id != 0);
                if (neighbour_solid) continue; // face is hidden

                // Emit a unit quad — position only (UVs etc. omitted for scaffold)
                uint32_t base = static_cast<uint32_t>(mesh.vertices.size() / 3);
                float fx = static_cast<float>(x), fy = static_cast<float>(y),
                      fz = static_cast<float>(z);
                // Just push the 4 corners as placeholder; real impl uses face direction
                mesh.vertices.insert(mesh.vertices.end(), {
                    fx,   fy,   fz,
                    fx+1, fy,   fz,
                    fx+1, fy+1, fz,
                    fx,   fy+1, fz
                });
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
