// ── liquids.cpp ───────────────────────────────────────────────────────────────
// Liquid / puddle physics simulation.
//
// Key design choices versus Monkestation2.0
// ─────────────────────────────────────────
// • 3-D world, Y-up.  "Down" = (0,-1,0).  Floors are solid voxels whose
//   type_id != 0.
// • Reagent pool is stored in a ReagentContainer per group (max_vol = 1e7 →
//   effectively unlimited so groups can grow without hitting a volume cap).
// • Edge cache: each group maintains, per boundary cell, the cardinal
//   horizontal directions still worth trying to spread into.  Consumed
//   directions (absorbed into group or proved permanently blocked) are pruned
//   immediately, matching Monkestation's cached_edge_turfs pruning.
// • Fall: cells without a solid floor immediately shed their proportional
//   share of the pool into the cell directly below.
// • Merge: when spread touches another group's cell, merge smaller → larger.
// • Split: after cell removal a BFS-limited connectivity check reassigns any
//   disconnected fragment to a fresh group.
// ─────────────────────────────────────────────────────────────────────────────
#include "simulation/liquids.h"
#include "simulation/physics.h"         // TransformComponent
#include "simulation/mob_system.h"      // HealthComponent, MobPlayerTag
#include <SDL3/SDL.h>
#include <queue>
#include <algorithm>
#include <cmath>
#include <cassert>

// ── Geometry helpers ──────────────────────────────────────────────────────────

bool LiquidSimulator::voxel_passable(glm::ivec3 pos) const
{
    return m_world.get_voxel(pos).type_id == 0;
}

bool LiquidSimulator::has_floor(glm::ivec3 pos) const
{
    if (pos.y == 0) return true;     // implicit bedrock at map bottom
    const Voxel v = m_world.get_voxel(pos + k_down);
    return (v.type_id != 0) && (v.flags & VFLAG_SOLID);
}

bool LiquidSimulator::cell_can_hold_liquid(glm::ivec3 pos) const
{
    return voxel_passable(pos) && has_floor(pos);
}

// ── Constructor ───────────────────────────────────────────────────────────────

LiquidSimulator::LiquidSimulator(World& world, EntityManager* entities)
    : m_world(world), m_entities(entities)
{}

// ── Group lifecycle ───────────────────────────────────────────────────────────

LiquidGroup& LiquidSimulator::create_group()
{
    LiquidGroupID id = m_next_group_id++;
    auto grp         = std::make_unique<LiquidGroup>();
    grp->id          = id;
    grp->reagents    = ReagentContainer(1e7f);   // effectively unlimited pool
    LiquidGroup& ref = *grp;
    m_groups[id]     = std::move(grp);
    return ref;
}

void LiquidSimulator::add_cell_to_group(LiquidGroup& grp, glm::ivec3 pos)
{
    if (m_cell_group.count(pos)) return;   // already owned
    grp.members.insert(pos);
    m_cell_group[pos] = grp.id;
    add_edges_for_cell(grp, pos);
}

void LiquidSimulator::remove_cell_from_group(glm::ivec3 pos)
{
    auto it = m_cell_group.find(pos);
    if (it == m_cell_group.end()) return;

    LiquidGroupID gid = it->second;
    m_cell_group.erase(it);

    auto git = m_groups.find(gid);
    if (git == m_groups.end()) return;
    LiquidGroup& grp = *git->second;

    grp.members.erase(pos);
    grp.edge_cache.erase(pos);

    // Neighbouring cells that had pos in their edge sets don't need updating —
    // those entries will be re-evaluated when process_spread() fires next tick.
}

// ── Edge cache helpers ────────────────────────────────────────────────────────

/// Called whenever a new cell is added to a group.
/// Populates edge_cache[cell] with horizontal directions whose neighbours
/// are NOT yet in this group, and removes any edge entry pointing at this cell
/// from those neighbours (since the gap is now interior).
void LiquidSimulator::add_edges_for_cell(LiquidGroup& grp, glm::ivec3 cell)
{
    std::vector<glm::ivec3> open_dirs;
    for (const auto& d : k_horiz) {
        glm::ivec3 nb = cell + d;
        if (!grp.members.count(nb)) {
            open_dirs.push_back(d);
        } else {
            // The neighbour was already in the group; remove the edge it had
            // pointing toward cell (that direction is now interior).
            auto eit = grp.edge_cache.find(nb);
            if (eit != grp.edge_cache.end()) {
                auto& dirs = eit->second;
                // Direction from nb toward cell = -d
                glm::ivec3 rev = -d;
                dirs.erase(std::remove(dirs.begin(), dirs.end(), rev), dirs.end());
                if (dirs.empty()) grp.edge_cache.erase(eit);
            }
        }
    }
    if (!open_dirs.empty())
        grp.edge_cache[cell] = std::move(open_dirs);
}

// ── Merge ─────────────────────────────────────────────────────────────────────

/// Merge src into dst. src is deleted. Returns a reference to the surviving
/// (dst) group.
void LiquidSimulator::merge_groups(LiquidGroupID dst_id, LiquidGroupID src_id)
{
    if (dst_id == src_id) return;

    auto dit = m_groups.find(dst_id);
    auto sit = m_groups.find(src_id);
    if (dit == m_groups.end() || sit == m_groups.end()) return;

    LiquidGroup& dst = *dit->second;
    LiquidGroup& src = *sit->second;

    // Transfer reagents src → dst (proportional, preserving ratios).
    float vol = src.reagents.total_volume();
    if (vol > 0.f)
        src.reagents.splash_to(vol, dst.reagents);

    // Re-home all cells from src to dst.
    for (const glm::ivec3& cell : src.members) {
        m_cell_group[cell] = dst_id;
        dst.members.insert(cell);
        add_edges_for_cell(dst, cell);
    }

    // Done with src.
    sit->second->marked_for_delete = true;
}

// ── Split (connectivity repair after cell removal) ───────────────────────────

void LiquidSimulator::try_split(LiquidGroup& grp,
                                const std::vector<glm::ivec3>& /*removed*/)
{
    if (grp.members.empty()) {
        grp.marked_for_delete = true;
        return;
    }
    if (grp.members.size() == 1) return;  // trivially connected

    // BFS from an arbitrary seed; anything NOT reachable within BFS_LIMIT
    // steps is a disconnected fragment that gets a new group.
    glm::ivec3             seed      = *grp.members.begin();
    std::unordered_set<glm::ivec3> visited;
    std::queue<glm::ivec3>         q;
    visited.insert(seed);
    q.push(seed);
    int steps = 0;

    while (!q.empty() && steps < LIQUID_SPLIT_BFS_LIMIT) {
        glm::ivec3 cur = q.front(); q.pop();
        ++steps;
        for (const auto& d : k_horiz) {
            glm::ivec3 nb = cur + d;
            if (!visited.count(nb) && grp.members.count(nb)) {
                visited.insert(nb);
                q.push(nb);
            }
        }
    }

    if (visited.size() == grp.members.size()) return;  // all still connected

    // Identify disconnected cells (not reached by the BFS).
    std::vector<glm::ivec3> stranded;
    for (const glm::ivec3& c : grp.members) {
        if (!visited.count(c)) stranded.push_back(c);
    }

    if (stranded.empty()) return;

    // How much volume each original cell "owns" (equal share).
    float vol_per_cell = grp.reagents.total_volume() /
                         static_cast<float>(grp.members.size());

    float split_volume = vol_per_cell * static_cast<float>(stranded.size());

    // Build new group for the disconnected fragment.
    LiquidGroup& ng = create_group();
    for (const glm::ivec3& c : stranded) {
        grp.members.erase(c);
        grp.edge_cache.erase(c);
        m_cell_group[c] = ng.id;
        ng.members.insert(c);
        add_edges_for_cell(ng, c);
    }

    // Transfer proportional volume to the new group.
    if (split_volume > 0.f)
        grp.reagents.splash_to(split_volume, ng.reagents);
}

// ── Fall ──────────────────────────────────────────────────────────────────────

void LiquidSimulator::process_fall(LiquidGroup& grp)
{
    if (grp.members.empty()) return;

    // Collect cells that no longer have a solid floor.
    std::vector<glm::ivec3> falling;
    for (const glm::ivec3& cell : grp.members) {
        if (!has_floor(cell)) falling.push_back(cell);
    }
    if (falling.empty()) return;

    float vol_per_cell = grp.reagents.total_volume() /
                         static_cast<float>(grp.members.size());

    for (const glm::ivec3& cell : falling) {
        glm::ivec3 below = cell + k_down;

        // Remove cell from this group.
        float share = std::min(vol_per_cell, grp.reagents.total_volume());
        remove_cell_from_group(cell);

        if (share <= 0.f) continue;

        // Where does the liquid land?
        // Fall until we find a cell that can hold liquid or we hit a solid.
        glm::ivec3 target = below;
        int safety = 64;
        while (safety-- > 0 && voxel_passable(target) && !has_floor(target))
            target += k_down;

        if (!cell_can_hold_liquid(target)) continue;  // fell into a wall

        // Add the reagent volume to the target cell's group (or new group).
        auto cit = m_cell_group.find(target);
        if (cit != m_cell_group.end()) {
            // Target already has a group — add volume there.
            auto tgit = m_groups.find(cit->second);
            if (tgit != m_groups.end())
                grp.reagents.splash_to(share, tgit->second->reagents);
        } else {
            // Create a new group at target.
            LiquidGroup& ng = create_group();
            grp.reagents.splash_to(share, ng.reagents);
            add_cell_to_group(ng, target);
        }
    }

    if (grp.members.empty()) grp.marked_for_delete = true;
}

// ── Spread ────────────────────────────────────────────────────────────────────

bool LiquidSimulator::try_spread_cell(LiquidGroup& grp,
                                       glm::ivec3   from,
                                       glm::ivec3   direction)
{
    glm::ivec3 target = from + direction;

    // Already in this group → direction is now interior, prune it.
    if (grp.members.count(target)) return false;

    // Owned by another group → merge that group into this one.
    auto cit = m_cell_group.find(target);
    if (cit != m_cell_group.end()) {
        LiquidGroupID other_id = cit->second;
        auto other_it = m_groups.find(other_id);
        if (other_it != m_groups.end()) {
            LiquidGroup& other = *other_it->second;
            if (grp.members.size() >= other.members.size())
                merge_groups(grp.id, other_id);
            else
                merge_groups(other_id, grp.id);
        }
        return true;  // consumed
    }

    // Check that the target cell can hold liquid.
    if (!cell_can_hold_liquid(target)) return false;  // wall or no floor

    // Expand group into target.
    add_cell_to_group(grp, target);
    return true;
}

void LiquidSimulator::process_spread(LiquidGroup& grp)
{
    if (grp.volume_per_cell() < LIQUID_SPREAD_THRESHOLD) return;

    // Iterate over the edge cache; collect cells/directions to prune after.
    std::vector<std::pair<glm::ivec3, glm::ivec3>> prune;

    // Copy the edge cache keys so we can mutate during iteration.
    std::vector<glm::ivec3> edge_cells;
    edge_cells.reserve(grp.edge_cache.size());
    for (auto& [c, _] : grp.edge_cache) edge_cells.push_back(c);

    for (const glm::ivec3& cell : edge_cells) {
        auto eit = grp.edge_cache.find(cell);
        if (eit == grp.edge_cache.end()) continue;

        // Copy direction list so we can prune mid-loop.
        auto dirs = eit->second;
        for (const glm::ivec3& dir : dirs) {
            bool consumed = try_spread_cell(grp, cell, dir);
            // Whether spread succeeded or was definitively blocked (solid wall),
            // remove the direction from the cache.
            prune.emplace_back(cell, dir);
            (void)consumed;
        }
    }

    // Prune consumed/blocked directions.
    for (auto& [cell, dir] : prune) {
        auto peit = grp.edge_cache.find(cell);
        if (peit == grp.edge_cache.end()) continue;
        auto& dvec = peit->second;
        dvec.erase(std::remove(dvec.begin(), dvec.end(), dir), dvec.end());
        if (dvec.empty()) grp.edge_cache.erase(peit);
    }
}

// ── Evaporation ───────────────────────────────────────────────────────────────

void LiquidSimulator::process_evaporation(LiquidGroup& grp, double dt)
{
    const float fddt   = static_cast<float>(dt);
    const int   n      = static_cast<int>(grp.members.size());
    if (n == 0) return;

    // Drain = rate * cells * dt (larger pools lose more per second absolute,
    // but same rate per cell — consistent with Monkestation evap_multiplier).
    float total_drain = LIQUID_EVAP_RATE_BASE * static_cast<float>(n) * fddt;

    // Drain proportionally across all reagents.
    float vol = grp.reagents.total_volume();
    if (vol <= 0.f) { grp.marked_for_delete = true; return; }

    {
        ReagentContainer sink(total_drain + 1.f);  // temporary sink — discards the drained volume
        grp.reagents.splash_to(
            std::min(total_drain, vol),
            sink
        );
    }

    // If puddle is too small to survive, mark for removal.
    if (grp.reagents.total_volume() < LIQUID_EVAP_THRESHOLD * static_cast<float>(n)) {
        // Clear all member cells then mark delete.
        for (const glm::ivec3& c : grp.members)
            m_cell_group.erase(c);
        grp.members.clear();
        grp.edge_cache.clear();
        grp.marked_for_delete = true;
    }
}

// ── Entity exposure ───────────────────────────────────────────────────────────

void LiquidSimulator::expose_entities(LiquidGroup& grp, double dt)
{
    // Skip groups that are too small to interact (saves iteration cost).
    if (grp.members.empty() || grp.reagents.total_volume() < 0.1f) return;

    if (!m_entities) return;

    const float exposure_rate = 0.5f;  // units transferred per second to entity
    const float fddt          = static_cast<float>(dt);

    m_entities->each<TransformComponent>([&](EntityID eid, TransformComponent& tr) {
        // Feet cell of the entity.
        glm::ivec3 feet = {
            static_cast<int>(std::floor(tr.pos.x)),
            static_cast<int>(std::floor(tr.pos.y)),
            static_cast<int>(std::floor(tr.pos.z)),
        };
        if (!grp.members.count(feet)) return;

        // Only transfer if the entity has an open reagent container.
        auto* rcc = m_entities->get_component<ReagentContainerComponent>(eid);
        if (!rcc || !rcc->is_open) return;

        float amount = std::min(
            exposure_rate * fddt,
            grp.reagents.total_volume() * 0.01f  // cap at 1% of group per second
        );
        if (amount < 1e-4f) return;

        grp.reagents.splash_to(amount, rcc->container);
    });
}

// ── Cleanup ───────────────────────────────────────────────────────────────────

void LiquidSimulator::cleanup_empty_groups()
{
    for (auto it = m_groups.begin(); it != m_groups.end(); ) {
        LiquidGroup& grp = *it->second;
        if (grp.marked_for_delete || grp.members.empty()) {
            // Ensure cell map is clean.
            for (const glm::ivec3& c : grp.members)
                m_cell_group.erase(c);
            it = m_groups.erase(it);
        } else {
            ++it;
        }
    }
}

// ── Per-tick processing ───────────────────────────────────────────────────────

void LiquidSimulator::process_tick(double dt)
{
    // Snapshot group IDs — groups may be added/merged during the loop.
    std::vector<LiquidGroupID> ids;
    ids.reserve(m_groups.size());
    for (auto& [id, _] : m_groups) ids.push_back(id);

    for (LiquidGroupID gid : ids) {
        auto git = m_groups.find(gid);
        if (git == m_groups.end()) continue;
        LiquidGroup& grp = *git->second;
        if (grp.marked_for_delete) continue;

        process_fall       (grp);
        if (grp.marked_for_delete) continue;

        process_spread     (grp);
        if (grp.marked_for_delete) continue;

        process_evaporation(grp, dt);
        if (grp.marked_for_delete) continue;

        expose_entities    (grp, dt);
    }

    cleanup_empty_groups();
}

// ── Public tick ───────────────────────────────────────────────────────────────

void LiquidSimulator::tick(double dt)
{
    m_accumulator += dt;
    while (m_accumulator >= LIQUID_TICK_DT) {
        process_tick(LIQUID_TICK_DT);
        m_accumulator -= LIQUID_TICK_DT;
    }
}

// ── World-event hook ──────────────────────────────────────────────────────────

void LiquidSimulator::on_voxel_changed(glm::ivec3 pos)
{
    // Evict any liquid cell that is now inside a solid voxel.
    if (!voxel_passable(pos)) {
        auto it = m_cell_group.find(pos);
        if (it != m_cell_group.end()) {
            LiquidGroupID gid  = it->second;
            auto git = m_groups.find(gid);
            LiquidGroup* grp   = (git != m_groups.end()) ? git->second.get() : nullptr;
            remove_cell_from_group(pos);
            if (grp) try_split(*grp, {pos});
        }
    }

    // Neighbouring cells that previously could not hold liquid because pos
    // was their floor (and it just became solid) may now be able to do so —
    // no action needed; they'll naturally be expanded into next spread tick.

    // If pos became passable and cells above it now have no floor, the fall
    // logic handles that on the next tick.
}

// ── Liquid injection ──────────────────────────────────────────────────────────

void LiquidSimulator::add_liquid(glm::ivec3 pos,
                                  std::string_view reagent_id, float amount)
{
    if (amount <= 0.f) return;

    // If pos can't hold liquid (solid, no floor) try the cell at same XZ but
    // walk down until a valid floor cell is found.
    glm::ivec3 target = pos;
    int safety = 32;
    while (safety-- > 0 && !cell_can_hold_liquid(target)) {
        if (!voxel_passable(target)) break;      // hit a solid, can't go further
        target += k_down;
    }
    if (!cell_can_hold_liquid(target)) return;

    auto cit = m_cell_group.find(target);
    if (cit != m_cell_group.end()) {
        auto git = m_groups.find(cit->second);
        if (git != m_groups.end())
            git->second->reagents.add(reagent_id, amount);
    } else {
        LiquidGroup& ng = create_group();
        ng.reagents.add(reagent_id, amount);
        add_cell_to_group(ng, target);
    }
}

void LiquidSimulator::add_liquid_from_container(glm::ivec3 pos,
                                                  ReagentContainer& src,
                                                  float amount)
{
    if (amount <= 0.f || src.total_volume() <= 0.f) return;

    glm::ivec3 target = pos;
    int safety = 32;
    while (safety-- > 0 && !cell_can_hold_liquid(target)) {
        if (!voxel_passable(target)) break;
        target += k_down;
    }
    if (!cell_can_hold_liquid(target)) return;

    auto cit = m_cell_group.find(target);
    if (cit != m_cell_group.end()) {
        auto git = m_groups.find(cit->second);
        if (git != m_groups.end())
            src.splash_to(amount, git->second->reagents);
    } else {
        LiquidGroup& ng = create_group();
        src.splash_to(amount, ng.reagents);
        add_cell_to_group(ng, target);
    }
}

// ── Query ─────────────────────────────────────────────────────────────────────

float LiquidSimulator::volume_at(glm::ivec3 pos) const
{
    auto cit = m_cell_group.find(pos);
    if (cit == m_cell_group.end()) return 0.f;
    auto git = m_groups.find(cit->second);
    if (git == m_groups.end()) return 0.f;
    return git->second->volume_per_cell();
}

LiquidGroupID LiquidSimulator::group_id_at(glm::ivec3 pos) const
{
    auto it = m_cell_group.find(pos);
    return it != m_cell_group.end() ? it->second : LIQUID_GROUP_NULL;
}

LiquidGroup* LiquidSimulator::group_at(glm::ivec3 pos)
{
    auto cit = m_cell_group.find(pos);
    if (cit == m_cell_group.end()) return nullptr;
    auto git = m_groups.find(cit->second);
    return git != m_groups.end() ? git->second.get() : nullptr;
}

const LiquidGroup* LiquidSimulator::group_at(glm::ivec3 pos) const
{
    auto cit = m_cell_group.find(pos);
    if (cit == m_cell_group.end()) return nullptr;
    auto git = m_groups.find(cit->second);
    return git != m_groups.end() ? git->second.get() : nullptr;
}
