#include "simulation/enclosure.h"
#include <queue>

EnclosureDetector::EnclosureDetector(const World& world)
    : m_world(world)
{}

void EnclosureDetector::invalidate()
{
    m_cache.clear();
}

bool EnclosureDetector::is_enclosed(glm::ivec3 pos)
{
    // Solid voxels are never "enclosed air"
    if (m_world.get_voxel(pos).type_id != 0)
        return false;

    // Return cached result if available
    auto it = m_cache.find(pos);
    if (it != m_cache.end())
        return it->second;

    // Run BFS and populate cache for every visited cell
    bool result = run_bfs(pos);
    return result;
}

bool EnclosureDetector::run_bfs(glm::ivec3 start)
{
    // 6-connected neighbor offsets
    static constexpr glm::ivec3 k_dirs[6] = {
        { 1, 0, 0}, {-1, 0, 0},
        { 0, 1, 0}, { 0,-1, 0},
        { 0, 0, 1}, { 0, 0,-1},
    };

    std::unordered_map<glm::ivec3, bool> visited;
    std::queue<glm::ivec3> queue;

    visited[start] = true;
    queue.push(start);
    int cell_count = 0;

    while (!queue.empty()) {
        glm::ivec3 cur = queue.front();
        queue.pop();
        ++cell_count;

        if (cell_count > k_open_threshold) {
            // Region is too large — assume it's open (connected to infinite void).
            // Mark everything visited so far as "not enclosed".
            for (auto& kv : visited)
                m_cache[kv.first] = false;
            return false;
        }

        for (const glm::ivec3& d : k_dirs) {
            glm::ivec3 nb = cur + d;
            if (visited.count(nb))
                continue;
            if (m_world.get_voxel(nb).type_id == 0) {
                visited[nb] = true;
                queue.push(nb);
            }
        }
    }

    // BFS finished within budget — all visited cells are in an enclosed region.
    for (auto& kv : visited)
        m_cache[kv.first] = true;

    return true;
}
