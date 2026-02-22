#pragma once
#include "core/world.h"
#include "render/renderer.h"
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>

// Builds ChunkMesh geometry from Chunk voxel data on worker threads.
// Uses greedy meshing to merge co-planar, same-material quads.
class ChunkMesher {
public:
    ChunkMesher();
    ~ChunkMesher();

    // Start background worker pool (call once at startup)
    void start(int num_workers = 2);
    void stop();

    // Enqueue a chunk for (re)meshing.  Neighbour chunks are passed so
    // cross-boundary faces can be resolved.
    void enqueue(glm::ivec3 chunk_pos, const World& world);

    // Collect finished meshes this frame; caller uploads them to the GPU.
    std::vector<ChunkMesh> collect_finished();

private:
    void worker_loop();
    ChunkMesh build_mesh(glm::ivec3 chunk_pos, const World& world);

    struct Job {
        glm::ivec3 chunk_pos;
        // Snapshot data so the worker doesn't hold a World reference
        std::array<Voxel, CHUNK_VOL> voxels;
        // Neighbour solid flags could be added here
    };

    std::vector<std::thread>   m_workers;
    std::queue<Job>            m_job_queue;
    std::vector<ChunkMesh>     m_finished;
    std::mutex                 m_queue_mutex;
    std::mutex                 m_finished_mutex;
    std::condition_variable    m_cv;
    std::atomic<bool>          m_running{false};
};
