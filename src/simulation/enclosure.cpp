#include "simulation/enclosure.h"
#include "core/types.h"
#include <queue>
#include <algorithm>

// 6-connected neighbour offsets (shared by all BFS steps).
static constexpr glm::ivec3 k_dirs[6] = {
    { 1, 0, 0}, {-1, 0, 0},
    { 0, 1, 0}, { 0,-1, 0},
    { 0, 0, 1}, { 0, 0,-1},
};

EnclosureDetector::EnclosureDetector(const World& world)
    : m_world(world)
{}

void EnclosureDetector::invalidate()
{
    m_cell_region.clear();
    m_regions.clear();
    m_door_cache.clear();
}

// ── helpers ───────────────────────────────────────────────────────────────────

bool EnclosureDetector::is_traversable(glm::ivec3 pos) const
{
    const Voxel& v = m_world.get_voxel(pos);
    if (v.type_id == 0) return true;                             // air
    // Open door: VERT_PLANE_Z but not SOLID → passable
    return (v.flags & VFLAG_VERT_PLANE_Z) && !(v.flags & VFLAG_SOLID);
}

bool EnclosureDetector::is_closed_door(glm::ivec3 pos) const
{
    const Voxel& v = m_world.get_voxel(pos);
    return (v.type_id != 0)
        && (v.flags & VFLAG_VERT_PLANE_Z)
        && (v.flags & VFLAG_SOLID);
}

// ── BFS ───────────────────────────────────────────────────────────────────────

int EnclosureDetector::find_region(glm::ivec3 start)
{
    if (!is_traversable(start)) return -1;

    // Return cached result if already processed.
    {
        auto it = m_cell_region.find(start);
        if (it != m_cell_region.end()) return it->second;
    }

    // BFS — traverse through air and open-door cells; record closed-door neighbours.
    std::unordered_map<glm::ivec3, bool> visited;
    std::vector<glm::ivec3>             border_doors;
    std::queue<glm::ivec3>              bfsq;

    visited[start] = true;
    bfsq.push(start);
    int cell_count = 0;

    while (!bfsq.empty()) {
        glm::ivec3 cur = bfsq.front();
        bfsq.pop();
        ++cell_count;

        if (cell_count > k_open_threshold) {
            // Region too large → open.  Mark all visited cells.
            for (auto& kv : visited)
                m_cell_region[kv.first] = -1;
            return -1;
        }

        for (const glm::ivec3& d : k_dirs) {
            glm::ivec3 nb = cur + d;
            if (visited.count(nb)) continue;

            if (is_traversable(nb)) {
                visited[nb] = true;
                bfsq.push(nb);
            } else if (is_closed_door(nb)) {
                // Record as a border closed-door (deduplicate).
                bool dup = false;
                for (auto& dp : border_doors)
                    if (dp == nb) { dup = true; break; }
                if (!dup) border_doors.push_back(nb);
            }
        }
    }

    // BFS finished within budget → enclosed.
    int rid = static_cast<int>(m_regions.size());
    RegionInfo info;
    info.enclosed     = true;
    info.size         = static_cast<int>(visited.size());
    info.border_doors = std::move(border_doors);
    m_regions.push_back(std::move(info));

    for (auto& kv : visited)
        m_cell_region[kv.first] = rid;

    return rid;
}

// ── closed-door evaluation ────────────────────────────────────────────────────

bool EnclosureDetector::eval_door(glm::ivec3 pos)
{
    // Collect the unique region IDs of all traversable cells adjacent to this door.
    std::vector<int> rids;
    for (const glm::ivec3& d : k_dirs) {
        glm::ivec3 nb = pos + d;
        if (!is_traversable(nb)) continue;

        int rid = find_region(nb);

        bool dup = false;
        for (int r : rids) if (r == rid) { dup = true; break; }
        if (!dup) rids.push_back(rid);
    }

    if (rids.empty()) return false;

    // Collect only the enclosed adjacent regions (rid >= 0).
    std::vector<int> enc;
    for (int r : rids)
        if (r >= 0) enc.push_back(r);

    if (enc.empty()) return false;   // all neighbours open → door is open
    if (enc.size() == 1) return true; // exactly one enclosed neighbour

    // Two (or more) enclosed regions: door defaults to the smaller one.
    std::sort(enc.begin(), enc.end(),
              [this](int a, int b){ return m_regions[a].size < m_regions[b].size; });

    int smaller_rid = enc.front();
    int larger_rid  = enc.back();

    // Exception: if the smaller region itself has another closed door that
    // connects into an unenclosed (open) region, it is "compromised" and
    // the door instead belongs to the larger region.
    bool compromised = false;
    for (const glm::ivec3& bd : m_regions[smaller_rid].border_doors) {
        if (bd == pos) continue;  // skip self

        for (const glm::ivec3& d : k_dirs) {
            glm::ivec3 nb = bd + d;
            if (!is_traversable(nb)) continue;

            int nb_rid = find_region(nb);
            if (nb_rid == smaller_rid) continue;  // same region, ignore
            if (nb_rid == -1) {                   // open / unenclosed
                compromised = true;
                break;
            }
        }
        if (compromised) break;
    }

    // Both smaller and larger are enclosed, so the result is always true;
    // the rule only selects *which* region the door is classified under.
    (void)larger_rid;
    return true;  // m_regions[compromised ? larger_rid : smaller_rid].enclosed
}

// ── public API ────────────────────────────────────────────────────────────────

bool EnclosureDetector::is_enclosed(glm::ivec3 pos)
{
    // Air or open-door cell: standard BFS.
    if (is_traversable(pos)) {
        return find_region(pos) >= 0;
    }

    // Closed-door cell: special smaller-region rule.
    if (is_closed_door(pos)) {
        auto it = m_door_cache.find(pos);
        if (it != m_door_cache.end()) return it->second;

        bool result = eval_door(pos);
        m_door_cache[pos] = result;
        return result;
    }

    // Other solid (wall, floor, etc.): not considered enclosed.
    return false;
}
