#pragma once
#include "core/world.h"
#include "data/voxel_registry.h"
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

    // Provide the voxel registry so the mesher can snapshot per-face
    // atlas indices at enqueue time.  Must be set before the first enqueue().
    void set_registry(const VoxelRegistry* reg) { m_registry = reg; }

    // Enqueue a chunk for (re)meshing.  Neighbour chunks are passed so
    // cross-boundary faces can be resolved.
    void enqueue(glm::ivec3 chunk_pos, const World& world);

    // Collect finished meshes this frame; caller uploads them to the GPU.
    // Only meshes from the current generation are returned — stale results
    // (enqueued before the last flush()) are silently discarded.
    std::vector<ChunkMesh> collect_finished();

    // Discard all pending and finished jobs from before a map reload.
    // Increments the internal generation counter so any in-flight worker
    // results are automatically dropped by collect_finished().
    void flush();

private:
    void worker_loop();
    ChunkMesh build_mesh(glm::ivec3 chunk_pos, const World& world);

    // One-chunk snapshot used for cross-seam neighbour lookups.
    struct NeighbourSnap {
        bool                         present = false;
        std::array<Voxel, CHUNK_VOL> voxels{};
    };

    struct Job {
        glm::ivec3 chunk_pos;
        uint64_t   generation = 0; // generation counter when this job was enqueued
        // Snapshot data so the worker doesn't hold a World reference
        std::array<Voxel, CHUNK_VOL> voxels;
        // Per-type atlas indices: type_atlas[type_id][face_dir] = texture layer.
        // Sized to (max_type_id + 1).  Empty when no registry is available.
        std::vector<std::array<uint16_t, static_cast<int>(FaceDir::COUNT)>> type_atlas;
        // Horizontal neighbour snapshots for cross-chunk face culling.
        // Index: 0=PosX  1=NegX  2=PosZ  3=NegZ
        std::array<NeighbourSnap, 4> neighbours;
    };

    std::vector<std::thread>     m_workers;
    std::queue<Job>              m_job_queue;
    std::vector<ChunkMesh>       m_finished;
    std::mutex                   m_queue_mutex;
    std::mutex                   m_finished_mutex;
    std::condition_variable      m_cv;
    std::atomic<bool>            m_running{false};
    std::atomic<uint64_t>        m_generation{0};
    const VoxelRegistry*         m_registry = nullptr;
};
