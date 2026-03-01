#pragma once
#include "core/world.h"
#include "data/voxel_registry.h"
#include "render/lighting.h"
#include "render/renderer.h"
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <unordered_map>
#include <unordered_set>

// Builds ChunkMesh geometry from Chunk voxel data on worker threads.
// Uses greedy meshing to merge co-planar, same-material quads.
class ChunkMesher {
public:
    // Shared-data types reused across all chunks enqueued in the same batch.
    using AtlasTable        = std::vector<std::array<uint16_t, static_cast<int>(FaceDir::COUNT)>>;
    using BitmaskAtlasTable = std::vector<uint16_t>; // [type_id] = base atlas layer (0 = not bitmask)
    using OverlayAtlasTable = std::vector<uint16_t>; // [type_id] = overlay atlas layer (0 = none)
    using LightColors       = std::unordered_map<glm::ivec3, LightColor>;
    using EmitTable         = std::vector<bool>;  // indexed by type_id; true = emissive

    ChunkMesher();
    ~ChunkMesher();

    // Start background worker pool (call once at startup)
    void start(int num_workers = 2);
    void stop();

    // Provide the voxel registry so the mesher can snapshot per-face
    // atlas indices at enqueue time.  Must be set before the first enqueue().
    // Calling this rebuilds the cached atlas immediately.
    void set_registry(const VoxelRegistry* reg);

    // Provide the lighting system so the mesher can snapshot the LightMap
    // at enqueue time for colored smooth lighting.
    void set_lighting(const LightingSystem* ls) { m_lighting = ls; }

    // Enqueue a single chunk for (re)meshing.  Uses the cached atlas and
    // takes its own LightMap snapshot (cheaper after enqueue_batch is used
    // for the hot path).
    void enqueue(glm::ivec3 chunk_pos, const World& world);

    // Efficient batch variant: takes ONE shared atlas + LightMap snapshot and
    // distributes it to all chunk jobs.  Use this whenever multiple dirty
    // chunks need meshing in the same frame (e.g. after a voxel edit + lighting
    // update) to avoid N redundant copies of the light map.
    void enqueue_batch(const std::vector<Chunk*>& chunks, const World& world);

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

    // Rebuild the cached atlas from the current registry.
    void rebuild_atlas();

    // One-chunk snapshot used for cross-seam neighbour lookups.
    struct NeighbourSnap {
        bool                         present = false;
        std::array<Voxel, CHUNK_VOL> voxels{};
    };

    struct Job {
        glm::ivec3 chunk_pos;
        uint64_t   generation = 0;
        std::array<Voxel, CHUNK_VOL> voxels;
        // Shared across all jobs in the same batch — pointer copy only.
        std::shared_ptr<const AtlasTable>        type_atlas;
        std::shared_ptr<const BitmaskAtlasTable> bitmask_atlas;
        std::shared_ptr<const OverlayAtlasTable> overlay_atlas;
        std::shared_ptr<const LightColors>       light_colors;
        std::shared_ptr<const EmitTable>         emit_table;
        std::array<NeighbourSnap, 4> neighbours;
    };

    std::vector<std::thread>       m_workers;
    std::queue<Job>                m_job_queue;
    std::unordered_set<glm::ivec3> m_pending; // dedup: chunks already in queue
    std::vector<ChunkMesh>         m_finished;
    std::mutex                     m_queue_mutex;
    std::mutex                     m_finished_mutex;
    std::condition_variable        m_cv;
    std::atomic<bool>              m_running{false};
    std::atomic<uint64_t>          m_generation{0};
    const VoxelRegistry*           m_registry = nullptr;
    const LightingSystem*          m_lighting  = nullptr;
    // Cached atlas — rebuilt once per set_registry() call, shared by all jobs.
    std::shared_ptr<const AtlasTable>        m_cached_atlas;
    std::shared_ptr<const BitmaskAtlasTable> m_cached_bitmask_atlas;
    std::shared_ptr<const OverlayAtlasTable> m_cached_overlay_atlas;
    std::shared_ptr<const EmitTable>         m_emit_table;
};
