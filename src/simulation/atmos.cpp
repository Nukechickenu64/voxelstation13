// ── atmos.cpp ─────────────────────────────────────────────────────────────────
// Room-based atmospheric simulation (ZAS-inspired).
//
// Gas flow model:
//   - Volume-conserving partial-pressure equalisation between zones.
//   - Conductance varies: open door > closed door seam > sealed wall.
//   - Space is an infinite sink; gas drains into it but is never returned.
//   - Door links rebuilt on rebuild_zones(); conductance checked each tick.
//
// Entity effects (requires EntityManager*):
//   - Mobs (MobPlayerTag) consume O2 and produce CO2 each tick.
//   - Pressure loss pushes all velocity-bearing entities toward the vent.
//     Force scales with pressure_loss_rate; grounded entities resist via
//     WIND_GROUND_RESIST; velocity is capped per-entity along wind direction.
//   - Pressure-differential status effects (requires StatusEffectsComponent):
//       >= WIND_JITTER_THRESHOLD (0.5 kPa/s)    → Jitter
//       >= WIND_DIZZY_THRESHOLD  (1.5 kPa/s)    → Dizzy
//       >= WIND_KNOCKDOWN_THRESHOLD (4.0 kPa/s) → Knockdown (duration scales)
//   - Barotrauma and decompression brute/burn damage are applied in server.cpp
//     (atmos simulator handles velocity + status; server handles health).
// ─────────────────────────────────────────────────────────────────────────────
#include "simulation/atmos.h"
#include "simulation/physics.h"
#include "simulation/mob_system.h"
#include "simulation/model_objects.h"
#include "simulation/status_effects.h"
#include <SDL3/SDL.h>
#include <queue>
#include <algorithm>
#include <cmath>
#include <unordered_set>

// ── Constructor ───────────────────────────────────────────────────────────────
AtmosSimulator::AtmosSimulator(World& world, EntityManager* entities)
    : m_world(world), m_entities(entities)
{
    AtmosZone space;
    space.id              = ATMOS_ZONE_SPACE;
    space.is_space        = true;
    space.gas.temperature = 2.73f;
    m_zones[ATMOS_ZONE_SPACE] = std::move(space);
}

// ── Geometry helpers ──────────────────────────────────────────────────────────
bool AtmosSimulator::voxel_is_passable(glm::ivec3 pos) const
{
    const Voxel v = m_world.get_voxel(pos);
    if (v.type_id == 0) {
        // Air cell — still blocked if a gas-blocking model object occupies it
        if (m_model_objects && m_model_objects->blocks_gas_at(pos)) return false;
        return true;
    }
    // Explicit gas-passable flag (e.g. open door) overrides solidity for atmos.
    if (v.flags & VFLAG_GAS_PASSABLE) return true;
    // Model objects with blocks_gas=true are opaque regardless of voxel flags
    if (m_model_objects && m_model_objects->blocks_gas_at(pos)) return false;
    return (v.flags & VFLAG_VERT_PLANE_Z) && !(v.flags & VFLAG_SOLID);
}

bool AtmosSimulator::voxel_is_closed_door(glm::ivec3 pos) const
{
    const Voxel v = m_world.get_voxel(pos);
    return (v.type_id != 0)
        && (v.flags & VFLAG_VERT_PLANE_Z)
        && (v.flags & VFLAG_SOLID);
}

int AtmosSimulator::total_rooms() const
{
    int n = static_cast<int>(m_zones.size());
    return (n > 0) ? n - 1 : 0;
}

// ── rebuild_zones ─────────────────────────────────────────────────────────────
void AtmosSimulator::rebuild_zones()
{
    // Keep existing gas mixes so an open-rebuild preserves atmosphere.
    std::unordered_map<AtmosZoneID, GasMixture> old_gas;
    for (auto& [id, z] : m_zones)
        if (!z.is_space) old_gas[id] = z.gas;

    // Save old cell→zone mapping before clearing so we can restore gas mixes.
    std::unordered_map<glm::ivec3, AtmosZoneID> old_cell_zone = std::move(m_cell_zone);

    // Reset — keep space zone.
    for (auto it = m_zones.begin(); it != m_zones.end();)
        it = (it->first != ATMOS_ZONE_SPACE) ? m_zones.erase(it) : std::next(it);
    m_cell_zone.clear();
    m_door_links.clear();
    m_next_zone_id = 2;

    static constexpr glm::ivec3 k_dirs[6] = {
        { 1, 0, 0}, {-1, 0, 0},
        { 0, 1, 0}, { 0,-1, 0},
        { 0, 0, 1}, { 0, 0,-1},
    };

    // Collect passable seeds from all loaded chunks.
    std::vector<glm::ivec3> seeds;
    m_world.for_each_chunk([&](const Chunk& chunk) {
        if (chunk.is_empty()) return;
        glm::ivec3 base = chunk.chunk_pos() * CHUNK_SIZE;
        for (int lx = 0; lx < CHUNK_SIZE; ++lx)
        for (int ly = 0; ly < CHUNK_SIZE; ++ly)
        for (int lz = 0; lz < CHUNK_SIZE; ++lz)
            if (voxel_is_passable(base + glm::ivec3{lx, ly, lz}))
                seeds.push_back(base + glm::ivec3{lx, ly, lz});
    });

    // Per zone during BFS: cells beyond a closed door + the door voxels themselves.
    std::unordered_map<AtmosZoneID, std::vector<glm::ivec3>> zone_beyond_cells;
    std::unordered_map<AtmosZoneID, std::vector<glm::ivec3>> zone_door_voxels;

    for (const glm::ivec3& seed : seeds) {
        if (m_cell_zone.count(seed)) continue;

        std::unordered_set<glm::ivec3> visited;
        // Tracks which door voxels this zone has already claimed as a boundary.
        // A door may be cardinal-adjacent to a zone from multiple directions
        // (e.g. when the zone wraps around a corner); only the FIRST valid
        // approach direction should produce a beyond-cell / door-link entry.
        // Without this guard the same door can generate beyond-cells pointing in
        // completely different directions, creating spurious diagonal zone links
        // and making gas appear to flow through solid walls at an angle.
        std::unordered_set<glm::ivec3> visited_doors;
        std::vector<glm::ivec3>        beyond_cells;
        std::vector<glm::ivec3>        door_voxels;
        std::queue<glm::ivec3>         q;

        visited.insert(seed);
        q.push(seed);

        while (!q.empty()) {
            glm::ivec3 cur = q.front(); q.pop();

            for (const glm::ivec3& d : k_dirs) {
                glm::ivec3 nb = cur + d;
                if (visited.count(nb)) continue;

                if (voxel_is_passable(nb)) {
                    visited.insert(nb);
                    q.push(nb);
                } else if (voxel_is_closed_door(nb)) {
                    // Skip if we already chose a boundary direction for this door.
                    if (visited_doors.count(nb)) continue;

                    glm::ivec3 bv = nb + d;

                    // Only register this door as a zone boundary when:
                    //   a) the cell immediately beyond it is passable (there is
                    //      actually another room on that side), AND
                    //   b) that cell is NOT already part of the current zone —
                    //      if the zone wraps around and owns 'bv', the door is
                    //      interior to the zone and must not create an inter-zone
                    //      link from this approach direction.
                    if (voxel_is_passable(bv) && !visited.count(bv)) {
                        visited_doors.insert(nb);
                        door_voxels.push_back(nb);
                        beyond_cells.push_back(bv);
                    }
                }
            }

            if (static_cast<int>(visited.size()) > SPACE_THRESHOLD) {
                while (!q.empty()) { visited.insert(q.front()); q.pop(); }

                // Mark everything as space first.
                for (const glm::ivec3& c : visited)
                    m_cell_zone[c] = ATMOS_ZONE_SPACE;

                // Cells that were previously in an enclosed (non-space) zone
                // should not lose their gas instantly.  Group them by their old
                // zone and create a transient draining zone for each group so
                // that process_space_drain() bleeds the gas out gradually.
                std::unordered_map<AtmosZoneID, std::vector<glm::ivec3>> prev_zone_cells;
                for (const glm::ivec3& c : visited) {
                    auto it = old_cell_zone.find(c);
                    if (it != old_cell_zone.end() && it->second != ATMOS_ZONE_SPACE)
                        prev_zone_cells[it->second].push_back(c);
                }

                for (auto& [old_zid, cells] : prev_zone_cells) {
                    auto gas_it = old_gas.find(old_zid);
                    if (gas_it == old_gas.end()) continue;
                    if (gas_it->second.total_pressure() < PRESSURE_THRESHOLD) continue;

                    AtmosZoneID new_zid = m_next_zone_id++;
                    AtmosZone drain_zone{};
                    drain_zone.id         = new_zid;
                    drain_zone.cell_count = static_cast<int>(cells.size());
                    drain_zone.gas        = gas_it->second;
                    m_zones[new_zid]      = drain_zone;

                    // Re-assign these cells so zone_at() works for them.
                    for (const glm::ivec3& c : cells)
                        m_cell_zone[c] = new_zid;

                    // Build a space DoorLink.
                    // door_voxels must only contain the breach-edge cells —
                    // those room cells that directly border the newly-opened
                    // space.  Two reasons:
                    //   1) All breach-edge cells are passable air, so
                    //      open_frac == 1 and full CONDUCTANCE_SPACE is applied.
                    //   2) The final midpoint recompute (at end of rebuild_zones)
                    //      averages door_voxels, so using the breach edge puts
                    //      vent_direction at the hole, not the room center.
                    // Falls back to all cells if none are detected (shouldn't
                    // normally happen — whole-map decompression edge case).
                    static constexpr glm::ivec3 k6[6] = {
                        { 1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
                    };
                    std::unordered_set<glm::ivec3> cell_set(cells.begin(), cells.end());
                    DoorLink lnk;
                    lnk.zone_a = ATMOS_ZONE_SPACE;
                    lnk.zone_b = new_zid;
                    for (const glm::ivec3& c : cells) {
                        bool breach_edge = false;
                        for (const auto& d : k6) {
                            glm::ivec3 nb = c + d;
                            // Neighbour is passable, not in this group, and is
                            // tagged as space → c is right next to the breach.
                            if (!cell_set.count(nb)
                                    && voxel_is_passable(nb)
                                    && m_cell_zone.count(nb)
                                    && m_cell_zone.at(nb) == ATMOS_ZONE_SPACE) {
                                breach_edge = true;
                                break;
                            }
                        }
                        if (breach_edge) lnk.door_voxels.push_back(c);
                    }
                    // Fallback: nothing detected → use all cells (open_frac=1 preserved).
                    if (lnk.door_voxels.empty()) {
                        for (const glm::ivec3& c : cells)
                            lnk.door_voxels.push_back(c);
                    }
                    // midpoint is computed by the final pass at the end of rebuild_zones.
                    m_door_links.push_back(std::move(lnk));
                }

                goto next_seed;
            }
        }

        {
            AtmosZoneID zid = m_next_zone_id++;
            AtmosZone   zone{};
            zone.id         = zid;
            zone.cell_count = static_cast<int>(visited.size());

            // Compute a cell-count-weighted average of every prior zone that
            // contributed cells to this new zone.  This handles:
            //   • Preservation across trivial rebuilds (zone unchanged).
            //   • Zone merges when a door opens: weighted mix of both pressures.
            //   • Zone splits when a door closes: each sub-zone inherits the
            //     same gas (was one zone, so values are identical on both sides).
            //   • Cells that were previously space (vacuum): contribute 0 kPa
            //     and 2.73 K to the average so a vestibule that was merged into
            //     space correctly starts at vacuum when re-sealed.
            {
                GasMixture acc{};
                acc.temperature = 0.f;
                float total_w = 0.f;

                for (const glm::ivec3& c : visited) {
                    auto prev_it = old_cell_zone.find(c);
                    if (prev_it == old_cell_zone.end()) continue;

                    if (prev_it->second == ATMOS_ZONE_SPACE) {
                        // Vacuum contribution: all gas = 0, T = 2.73 K.
                        acc.temperature += 2.73f;
                        total_w += 1.f;
                    } else {
                        auto gas_it = old_gas.find(prev_it->second);
                        if (gas_it == old_gas.end()) continue;
                        const GasMixture& og = gas_it->second;
                        acc.o2          += og.o2;
                        acc.n2          += og.n2;
                        acc.co2         += og.co2;
                        acc.plasma      += og.plasma;
                        acc.n2o         += og.n2o;
                        acc.bz          += og.bz;
                        acc.tritium     += og.tritium;
                        acc.temperature += og.temperature;
                        total_w += 1.f;
                    }
                }

                if (total_w > 0.f) {
                    float inv = 1.f / total_w;
                    zone.gas.o2          = acc.o2          * inv;
                    zone.gas.n2          = acc.n2          * inv;
                    zone.gas.co2         = acc.co2         * inv;
                    zone.gas.plasma      = acc.plasma      * inv;
                    zone.gas.n2o         = acc.n2o         * inv;
                    zone.gas.bz          = acc.bz          * inv;
                    zone.gas.tritium     = acc.tritium     * inv;
                    zone.gas.temperature = std::max(2.73f, acc.temperature * inv);
                } else {
                    // No prior data (first startup) — initialise to normal station air.
                    zone.gas.o2          = 21.0f;
                    zone.gas.n2          = 80.0f;
                    zone.gas.temperature = 293.15f;
                }
            }
            m_zones[zid] = zone;
            for (const glm::ivec3& c : visited) m_cell_zone[c] = zid;
            zone_beyond_cells[zid] = std::move(beyond_cells);
            zone_door_voxels[zid]  = std::move(door_voxels);
        }
        next_seed:;
    }

    // Second pass: build DoorLinks and adjacent_zones.
    // Track which zone-pairs have already been linked (avoid duplicates).
    std::unordered_map<uint64_t, size_t> link_index; // encoded pair → m_door_links idx

    for (auto& [zid, bcells] : zone_beyond_cells) {
        auto it_a = m_zones.find(zid);
        if (it_a == m_zones.end()) continue;
        AtmosZone& za = it_a->second;

        const auto& dv_list = zone_door_voxels[zid];

        // Map each beyond-cell to its zone, build/extend DoorLinks.
        for (const glm::ivec3& bc : bcells) {
            auto cell_it = m_cell_zone.find(bc);
            AtmosZoneID adj = (cell_it != m_cell_zone.end())
                              ? cell_it->second : ATMOS_ZONE_SPACE;
            if (adj == zid || adj == ATMOS_ZONE_NULL) continue;

            // Canonical ordering for the zone pair key.
            AtmosZoneID lo = std::min(zid, adj);
            AtmosZoneID hi = std::max(zid, adj);
            uint64_t key   = (static_cast<uint64_t>(lo) << 32) | hi;

            if (!link_index.count(key)) {
                DoorLink lnk;
                lnk.zone_a = lo;
                lnk.zone_b = hi;
                link_index[key] = m_door_links.size();
                m_door_links.push_back(std::move(lnk));
            }
            DoorLink& lnk = m_door_links[link_index[key]];

            // Add door voxels from this zone's side (deduplicate).
            for (const glm::ivec3& dv : dv_list) {
                bool dup = false;
                for (auto& ex : lnk.door_voxels)
                    if (ex == dv) { dup = true; break; }
                if (!dup) lnk.door_voxels.push_back(dv);
            }

            // Bidirectional adjacency for tick() closed-door slow flow.
            auto add_adj = [](std::vector<AtmosZoneID>& v, AtmosZoneID id) {
                for (auto x : v) if (x == id) return;
                v.push_back(id);
            };
            add_adj(za.adjacent_zones, adj);
            auto it_b = m_zones.find(adj);
            if (it_b != m_zones.end())
                add_adj(it_b->second.adjacent_zones, zid);
        }
    }

    // Compute midpoints for all door links.
    for (DoorLink& lnk : m_door_links) {
        if (lnk.door_voxels.empty()) continue;
        glm::ivec3 s{0};
        for (auto& dv : lnk.door_voxels) s += dv;
        lnk.midpoint = glm::vec3(s) / static_cast<float>(lnk.door_voxels.size())
                       + glm::vec3(0.5f);
    }

    SDL_Log("AtmosSimulator: rebuild done — %d room(s), %u cells, %u door link(s).",
            total_rooms(),
            static_cast<unsigned>(m_cell_zone.size()),
            static_cast<unsigned>(m_door_links.size()));
}

// ── tick ──────────────────────────────────────────────────────────────────────
void AtmosSimulator::tick(double dt)
{
    // Apply any deferred zone rebuild (coalesced from on_voxel_changed calls).
    if (m_rebuild_pending) {
        rebuild_zones();
        m_rebuild_pending = false;
    }

    // Reset per-tick diagnostics.
    for (auto& [id, z] : m_zones) {
        z.pressure_loss_rate = 0.f;
        z.vent_direction     = {};
    }

    process_door_links(dt);

    for (auto& [id, zone] : m_zones)
        if (!zone.is_space && zone.has_hotspot)
            process_hotspot(zone, dt);

    if (m_entities) apply_entity_effects(dt);

    for (auto& [id, zone] : m_zones)
        if (!zone.is_space) update_status(zone);
}

// ── process_door_links ────────────────────────────────────────────────────────
void AtmosSimulator::process_door_links(double dt)
{
    for (const DoorLink& lnk : m_door_links) {
        AtmosZone* za = zone_at_id(lnk.zone_a);
        AtmosZone* zb = zone_at_id(lnk.zone_b);
        if (!za || !zb) continue;

        int open_n = 0, closed_n = 0;
        for (const glm::ivec3& dv : lnk.door_voxels) {
            if      (voxel_is_passable   (dv)) ++open_n;
            else if (voxel_is_closed_door(dv)) ++closed_n;
        }
        int total = open_n + closed_n;
        if (total == 0) continue;

        float open_frac   = static_cast<float>(open_n) / static_cast<float>(total);
        float conductance = CONDUCTANCE_CLOSED * (1.f - open_frac)
                          + CONDUCTANCE_OPEN   * open_frac;

        bool has_space = za->is_space || zb->is_space;
        if (has_space) {
            AtmosZone* room = za->is_space ? zb : za;
            float sc = (open_frac > 0.f)
                       ? CONDUCTANCE_SPACE        * open_frac
                       : CONDUCTANCE_SPACE_SEALED * (1.f - open_frac);
            process_space_drain(*room, lnk, sc, dt);
        } else {
            equalise_zones(*za, *zb, dt, conductance, lnk.midpoint);
        }
    }
}

// ── process_space_drain ───────────────────────────────────────────────────────
void AtmosSimulator::process_space_drain(AtmosZone& zone, const DoorLink& lnk,
                                          float conductance, double dt)
{
    float Va   = std::max(1.f, static_cast<float>(zone.cell_count));
    float step = std::min(1.f, conductance * static_cast<float>(dt));

    auto drain = [&](float& p) -> float {
        float moles_lost = p * Va * step;
        p -= moles_lost / Va;
        if (p < 0.f) { moles_lost += p * Va; p = 0.f; }
        return moles_lost;
    };

    float drained = 0.f;
    drained += drain(zone.gas.o2);
    drained += drain(zone.gas.n2);
    drained += drain(zone.gas.co2);
    drained += drain(zone.gas.plasma);
    drained += drain(zone.gas.n2o);
    drained += drain(zone.gas.bz);
    drained += drain(zone.gas.tritium);

    // Adiabatic cooling proportional to gas lost.
    zone.gas.temperature = std::max(
        2.73f,
        zone.gas.temperature - step * zone.gas.temperature * 0.25f);

    float kpa_s = (dt > 1e-9) ? drained / static_cast<float>(dt) : 0.f;
    zone.pressure_loss_rate += kpa_s;
    zone.vent_direction      = lnk.midpoint;  // world pos of breach
}

// ── equalise_zones ────────────────────────────────────────────────────────────
void AtmosSimulator::equalise_zones(AtmosZone& a, AtmosZone& b, double dt, float conductance, glm::vec3 midpoint)
{
    float Pa = a.gas.total_pressure();
    float Pb = b.gas.total_pressure();
    if (std::abs(Pa - Pb) < PRESSURE_THRESHOLD) return;

    float Va = std::max(1.f, static_cast<float>(a.cell_count));
    float Vb = std::max(1.f, static_cast<float>(b.cell_count));

    float pre_Pa = Pa;

    auto do_comp = [&](float& pa, float& pb) {
        move_gas_component(pa, pb, Va, Vb, conductance, dt);
    };
    do_comp(a.gas.o2,      b.gas.o2);
    do_comp(a.gas.n2,      b.gas.n2);
    do_comp(a.gas.co2,     b.gas.co2);
    do_comp(a.gas.plasma,  b.gas.plasma);
    do_comp(a.gas.n2o,     b.gas.n2o);
    do_comp(a.gas.bz,      b.gas.bz);
    do_comp(a.gas.tritium, b.gas.tritium);

    // Temperature mixing proportional to transferred moles.
    float post_Pa   = a.gas.total_pressure();
    float moles_a_change = (pre_Pa - post_Pa) * Va;  // >0 = gas left a

    if (moles_a_change > 0.f) {
        mix_temperature(b, moles_a_change, a.gas.temperature);
    } else if (moles_a_change < 0.f) {
        mix_temperature(a, -moles_a_change, b.gas.temperature);
    }

    // Wind pull: entities in the high-pressure zone are tugged toward the door.
    // Scale by kPa/s so it blends naturally with the space-decomp threshold.
    if (dt > 1e-9) {
        if (moles_a_change > 0.f) {
            // gas left a → a was high-pressure; pull entities in a toward door
            float kpa_s = moles_a_change / (Va * static_cast<float>(dt));
            a.pressure_loss_rate += kpa_s;
            a.vent_direction      = midpoint;
        } else if (moles_a_change < 0.f) {
            // gas left b → b was high-pressure; pull entities in b toward door
            float kpa_s = -moles_a_change / (Vb * static_cast<float>(dt));
            b.pressure_loss_rate += kpa_s;
            b.vent_direction      = midpoint;
        }
    }
}

// ── move_gas_component ────────────────────────────────────────────────────────
void AtmosSimulator::move_gas_component(float& pa, float& pb, float Va, float Vb,
                                         float conductance, double dt)
{
    float n_total = pa * Va + pb * Vb;
    float Vtotal  = Va + Vb;
    float p_eq    = n_total / Vtotal;

    float excess_a = (pa - p_eq) * Va;
    if (std::abs(excess_a) < 1e-6f) return;

    float step = std::min(1.f, conductance * static_cast<float>(dt));
    float flow  = excess_a * step;

    pa -= flow / Va;
    pb += flow / Vb;

    if (pa < 0.f) { pb += pa * (Va / Vb); pa = 0.f; }
    if (pb < 0.f) { pa += pb * (Vb / Va); pb = 0.f; }
}

// ── mix_temperature ───────────────────────────────────────────────────────────
void AtmosSimulator::mix_temperature(AtmosZone& dst, float added_moles, float src_temp)
{
    float existing = dst.gas.moles() * std::max(1.f, static_cast<float>(dst.cell_count));
    float total    = existing + added_moles;
    if (total < 1e-6f) return;
    dst.gas.temperature = (dst.gas.temperature * existing + src_temp * added_moles) / total;
    dst.gas.temperature = std::max(2.73f, dst.gas.temperature);
}

// ── process_hotspot ───────────────────────────────────────────────────────────
void AtmosSimulator::process_hotspot(AtmosZone& zone, double dt)
{
    if (zone.gas.plasma < 0.1f || zone.gas.o2 < 0.1f) {
        zone.has_hotspot  = false;
        zone.hotspot_temp = 0.f;
        return;
    }
    float fdt     = static_cast<float>(dt);
    float t_ratio = std::max(1.f, zone.gas.temperature / IGNITION_TEMPERATURE);
    float burn    = std::min(zone.gas.plasma, zone.gas.o2) * fdt * 0.3f * t_ratio;

    zone.gas.plasma      -= burn;
    zone.gas.o2          -= burn;
    zone.gas.co2         += burn * 0.6f;
    zone.gas.temperature += burn * 35.f;
    zone.hotspot_temp     = zone.gas.temperature;
}

// ── apply_entity_effects ──────────────────────────────────────────────────────
void AtmosSimulator::apply_entity_effects(double dt)
{
    float fdt = static_cast<float>(dt);

    // ── Breathing: O2 consumption / CO2 production (mobs/players only) ───
    // Only entities tagged MobPlayerTag are alive and breathing; world items
    // and decorative entities are excluded.
    m_entities->each<MobPlayerTag>([&](EntityID eid, MobPlayerTag&) {
        auto* tr = m_entities->get_component<TransformComponent>(eid);
        if (!tr) return;
        glm::ivec3 feet = {
            static_cast<int>(std::floor(tr->pos.x)),
            static_cast<int>(std::floor(tr->pos.y)),
            static_cast<int>(std::floor(tr->pos.z))
        };
        AtmosZoneID zid = zone_at(feet);
        if (zid == ATMOS_ZONE_NULL || zid == ATMOS_ZONE_SPACE) return;
        AtmosZone* z = zone_at_id(zid);
        if (!z) return;

        float V      = std::max(1.f, static_cast<float>(z->cell_count));
        z->gas.o2   -= O2_CONSUMPTION_RATE * fdt / V;
        z->gas.co2  += CO2_PRODUCTION_RATE * fdt / V;
        if (z->gas.o2 < 0.f) z->gas.o2 = 0.f;
    });

    // ── Wind: push entities by pressure flow ─────────────────────────────
    // Applied to every entity with a VelocityComponent (players, mobs, and
    // loose world items) whose zone is experiencing net gas loss.
    //
    // Force model:
    //   impulse  = pressure_loss_rate * WIND_ACCEL_PER_KPA_S * dt  (m/s added)
    //   vel_cap  = clamp(pressure_loss_rate * 0.5 + 1, 0, WIND_VEL_CAP)  (m/s)
    //
    // Grounded entities receive WIND_GROUND_RESIST fraction of the impulse;
    // floor contact provides real friction that resists small differentials.
    // Velocity capping is directional: the component along wind_dir is
    // compared to vel_cap — no wind is added once the cap is reached.
    m_entities->each<VelocityComponent>([&](EntityID eid, VelocityComponent& vel) {
        auto* tr = m_entities->get_component<TransformComponent>(eid);
        if (!tr) return;

        glm::ivec3 feet = {
            static_cast<int>(std::floor(tr->pos.x)),
            static_cast<int>(std::floor(tr->pos.y)),
            static_cast<int>(std::floor(tr->pos.z))
        };
        AtmosZoneID zid = zone_at(feet);
        if (zid == ATMOS_ZONE_NULL || zid == ATMOS_ZONE_SPACE) return;
        AtmosZone* z = zone_at_id(zid);
        if (!z || z->pressure_loss_rate < WIND_THRESHOLD) return;

        // Direction from entity toward the vent/breach draining this zone.
        glm::vec3 to_vent = z->vent_direction - tr->pos;
        float dist = glm::length(to_vent);
        if (dist < 0.01f) return;
        glm::vec3 wind_dir = to_vent / dist;

        // Raw impulse this atmos tick.
        float impulse = z->pressure_loss_rate * WIND_ACCEL_PER_KPA_S * fdt;

        // Grounded players / mobs resist wind via floor contact.
        auto* cc = m_entities->get_component<CharacterControllerComponent>(eid);
        if (cc && cc->on_ground)
            impulse *= WIND_GROUND_RESIST;

        // Directional velocity cap — scales with severity so gentle breezes
        // top out around 1 m/s while explosive decomp can reach WIND_VEL_CAP.
        float vel_cap  = std::min(z->pressure_loss_rate * 0.5f + 1.f, WIND_VEL_CAP);
        float vel_along = glm::dot(vel.linear, wind_dir);
        float add = std::min(impulse, std::max(0.f, vel_cap - vel_along));
        if (add <= 0.f) return;

        vel.linear += wind_dir * add;
    });

    // ── Pressure-differential status effects ──────────────────────────────
    // Entities with a StatusEffectsComponent that are in a decompressing zone
    // receive sensory and incapacitation effects scaled to severity:
    //
    //   rate >= WIND_JITTER_THRESHOLD   → Jitter (screen micro-shake)
    //   rate >= WIND_DIZZY_THRESHOLD    → Dizzy  (orientation overlay)
    //   rate >= WIND_KNOCKDOWN_THRESHOLD→ Knockdown (prone, grounded by wind)
    //
    // Duration is proportional to rate, capped at WIND_KNOCKDOWN_DUR_MAX.
    // Jitter/Dizzy refresh every tick so they persist as long as the wind does.
    m_entities->each<StatusEffectsComponent>([&](EntityID eid, StatusEffectsComponent& se) {
        auto* tr = m_entities->get_component<TransformComponent>(eid);
        if (!tr) return;

        glm::ivec3 feet = {
            static_cast<int>(std::floor(tr->pos.x)),
            static_cast<int>(std::floor(tr->pos.y)),
            static_cast<int>(std::floor(tr->pos.z))
        };
        AtmosZoneID zid = zone_at(feet);
        if (zid == ATMOS_ZONE_NULL || zid == ATMOS_ZONE_SPACE) return;
        AtmosZone* z = zone_at_id(zid);
        if (!z) return;

        const float rate = z->pressure_loss_rate;

        if (rate >= WIND_JITTER_THRESHOLD) {
            // Jitter duration: refresh to ~0.5 s so it stays while wind blows.
            se.apply(StatusEffectType::Jitter, 0.5f, std::min(rate / WIND_KNOCKDOWN_THRESHOLD, 1.f));
        }

        if (rate >= WIND_DIZZY_THRESHOLD) {
            se.apply(StatusEffectType::Dizzy, 0.5f, std::min(rate / WIND_KNOCKDOWN_THRESHOLD, 1.f));
        }

        if (rate >= WIND_KNOCKDOWN_THRESHOLD) {
            // Duration scales with rate; more violent decomp = longer knockdown.
            float kd_dur = std::clamp(
                WIND_KNOCKDOWN_DUR_BASE * (rate / WIND_KNOCKDOWN_THRESHOLD),
                WIND_KNOCKDOWN_DUR_BASE,
                WIND_KNOCKDOWN_DUR_MAX);
            se.apply(StatusEffectType::Knockdown, kd_dur, 1.f);
        }
    });
}

// ── update_status ─────────────────────────────────────────────────────────────
void AtmosSimulator::update_status(AtmosZone& zone)
{
    uint8_t s = ATMOS_OK;
    const GasMixture& g = zone.gas;
    if (g.o2 < 16.f)                                    s |= ATMOS_LOW_O2;
    if (g.total_pressure() < 50.f)                      s |= ATMOS_LOW_PRESS;
    if (g.co2 > 5.f)                                    s |= ATMOS_HIGH_CO2;
    if (g.plasma > 0.5f || g.bz > 0.5f || g.n2o > 5.f) s |= ATMOS_TOXIC;
    if (zone.has_hotspot)                               s |= ATMOS_FIRE;
    if (zone.pressure_loss_rate > 1.f)                  s |= ATMOS_DECOMP;
    if (g.temperature > 360.f)                          s |= ATMOS_HIGH_TEMP;
    zone.status = s;
}

// ── Public API ────────────────────────────────────────────────────────────────
AtmosZone* AtmosSimulator::zone_at_id(AtmosZoneID id)
{
    auto it = m_zones.find(id);
    return it != m_zones.end() ? &it->second : nullptr;
}

AtmosZone* AtmosSimulator::zone(AtmosZoneID id) { return zone_at_id(id); }

const AtmosZone* AtmosSimulator::zone(AtmosZoneID id) const
{
    auto it = m_zones.find(id);
    return it != m_zones.end() ? &it->second : nullptr;
}

AtmosZoneID AtmosSimulator::zone_at(glm::ivec3 pos) const
{
    auto it = m_cell_zone.find(pos);
    return it != m_cell_zone.end() ? it->second : ATMOS_ZONE_NULL;
}

GasMixture AtmosSimulator::mix_at(glm::ivec3 pos) const
{
    auto it = m_zones.find(zone_at(pos));
    return it != m_zones.end() ? it->second.gas : GasMixture{};
}

void AtmosSimulator::on_voxel_changed(glm::ivec3 /*pos*/) { m_rebuild_pending = true; }

void AtmosSimulator::on_door_changed(glm::ivec3 pos)
{
    // Collect zone IDs on either side of this door instead of doing a
    // costly full-map rebuild.  We re-BFS only the affected zones.
    static constexpr glm::ivec3 k_dirs[6] = {
        { 1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };

    std::unordered_set<AtmosZoneID> affected;
    for (const glm::ivec3& d : k_dirs) {
        auto it = m_cell_zone.find(pos + d);
        if (it != m_cell_zone.end() && it->second != ATMOS_ZONE_SPACE)
            affected.insert(it->second);
    }
    // Also check the door pos itself — it may have been passable (open door
    // that just closed) and therefore in a zone.
    if (auto it = m_cell_zone.find(pos); it != m_cell_zone.end()
            && it->second != ATMOS_ZONE_SPACE)
        affected.insert(it->second);

    if (affected.empty()) {
        // No known neighbours — fall back to full rebuild.
        m_rebuild_pending = true;
        return;
    }
    partial_rebuild(std::move(affected));
}

void AtmosSimulator::try_ignite(AtmosZoneID id)
{
    auto* z = zone(id);
    if (!z) return;
    if (z->gas.plasma >= PLASMA_FIRE_O2_MIN
     && z->gas.o2     >= PLASMA_FIRE_O2_MIN
     && z->gas.temperature >= IGNITION_TEMPERATURE)
    {
        z->has_hotspot  = true;
        z->hotspot_temp = z->gas.temperature;
        SDL_Log("Atmos: ignition in zone %u!", id);
    }
}

void AtmosSimulator::inject_gas(AtmosZoneID id, GasMixture delta)
{
    auto* z = zone(id);
    if (!z || z->is_space) return;
    float src_temp = delta.temperature;
    z->gas.o2      += delta.o2;
    z->gas.n2      += delta.n2;
    z->gas.co2     += delta.co2;
    z->gas.plasma  += delta.plasma;
    z->gas.n2o     += delta.n2o;
    z->gas.bz      += delta.bz;
    z->gas.tritium += delta.tritium;
    mix_temperature(*z, delta.total_pressure(), src_temp);
}

// ── partial_rebuild ───────────────────────────────────────────────────────────
// Re-BFS only the zones given in zone_ids rather than scanning the entire map.
// Called by on_door_changed to avoid an O(all-cells) rebuild on every door toggle.
void AtmosSimulator::partial_rebuild(std::unordered_set<AtmosZoneID> zone_ids)
{
    // ── 1. Save gas mixes from affected zones ─────────────────────────────────
    std::unordered_map<AtmosZoneID, GasMixture> old_gas;
    for (AtmosZoneID id : zone_ids) {
        if (auto it = m_zones.find(id); it != m_zones.end())
            old_gas[id] = it->second.gas;
    }

    // ── 2. Collect and unregister cells belonging to affected zones ───────────
    std::vector<glm::ivec3> seeds;
    std::unordered_map<glm::ivec3, AtmosZoneID> old_cell_part;

    for (auto it = m_cell_zone.begin(); it != m_cell_zone.end(); ) {
        if (zone_ids.count(it->second)) {
            seeds.push_back(it->first);
            old_cell_part[it->first] = it->second;
            it = m_cell_zone.erase(it);
        } else {
            ++it;
        }
    }

    // ── 3. Remove affected zones from the zone table ──────────────────────────
    for (AtmosZoneID id : zone_ids) m_zones.erase(id);

    // ── 4. Remove door links that reference any affected zone ─────────────────
    m_door_links.erase(
        std::remove_if(m_door_links.begin(), m_door_links.end(),
            [&](const DoorLink& lnk) {
                return zone_ids.count(lnk.zone_a) || zone_ids.count(lnk.zone_b);
            }),
        m_door_links.end());

    // ── 5. Prune stale adjacency entries in surviving zones ───────────────────
    for (auto& [id, z] : m_zones) {
        z.adjacent_zones.erase(
            std::remove_if(z.adjacent_zones.begin(), z.adjacent_zones.end(),
                [&](AtmosZoneID adj) { return zone_ids.count(adj) > 0; }),
            z.adjacent_zones.end());
    }

    // ── 6. BFS: re-discover zones from the freed (seed) cells ─────────────────
    static constexpr glm::ivec3 k_dirs[6] = {
        { 1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };
    // Quick membership test: is this cell a freed seed?
    std::unordered_set<glm::ivec3> freed(seeds.begin(), seeds.end());

    std::unordered_map<AtmosZoneID, std::vector<glm::ivec3>> zone_beyond_cells;
    std::unordered_map<AtmosZoneID, std::vector<glm::ivec3>> zone_door_voxels;

    for (const glm::ivec3& seed : seeds) {
        if (m_cell_zone.count(seed)) continue;  // already re-assigned

        std::unordered_set<glm::ivec3> visited;   // actual zone cells
        std::unordered_set<glm::ivec3> boundary;  // prevent double-processing neighbours
        std::unordered_set<glm::ivec3> visited_doors;
        std::vector<glm::ivec3>        beyond_cells;
        std::vector<glm::ivec3>        door_voxels_found;
        std::queue<glm::ivec3>         q;

        visited.insert(seed);
        q.push(seed);

        while (!q.empty()) {
            glm::ivec3 cur = q.front(); q.pop();

            for (const glm::ivec3& d : k_dirs) {
                glm::ivec3 nb = cur + d;
                if (visited.count(nb) || boundary.count(nb)) continue;

                // Already owned by a surviving (non-affected) zone → boundary.
                if (m_cell_zone.count(nb)) {
                    beyond_cells.push_back(nb);
                    boundary.insert(nb);
                    continue;
                }

                if (voxel_is_passable(nb)) {
                    // Expand if this is a freed seed or an unassigned passable
                    // cell (e.g. the door voxel that just opened).
                    visited.insert(nb);
                    q.push(nb);
                } else if (voxel_is_closed_door(nb)) {
                    if (visited_doors.count(nb)) continue;
                    glm::ivec3 bv = nb + d;
                    if (voxel_is_passable(bv) && !visited.count(bv) && !boundary.count(bv)) {
                        visited_doors.insert(nb);
                        door_voxels_found.push_back(nb);
                        beyond_cells.push_back(bv);
                    }
                }
            }

            if (static_cast<int>(visited.size()) > SPACE_THRESHOLD) {
                while (!q.empty()) { visited.insert(q.front()); q.pop(); }
                for (const glm::ivec3& c : visited) m_cell_zone[c] = ATMOS_ZONE_SPACE;
                goto next_seed_partial;
            }
        }

        {
            AtmosZoneID zid  = m_next_zone_id++;
            AtmosZone   zone{};
            zone.id          = zid;
            zone.cell_count  = static_cast<int>(visited.size());

            // Restore gas as a cell-weighted average of the old zones.
            {
                GasMixture acc{};
                acc.temperature = 0.f;
                float total_w   = 0.f;
                for (const glm::ivec3& c : visited) {
                    auto it2 = old_cell_part.find(c);
                    if (it2 == old_cell_part.end()) continue;
                    auto git = old_gas.find(it2->second);
                    if (git == old_gas.end()) continue;
                    const GasMixture& og = git->second;
                    acc.o2          += og.o2;
                    acc.n2          += og.n2;
                    acc.co2         += og.co2;
                    acc.plasma      += og.plasma;
                    acc.n2o         += og.n2o;
                    acc.bz          += og.bz;
                    acc.tritium     += og.tritium;
                    acc.temperature += og.temperature;
                    total_w         += 1.f;
                }
                if (total_w > 0.f) {
                    float inv            = 1.f / total_w;
                    zone.gas.o2          = acc.o2          * inv;
                    zone.gas.n2          = acc.n2          * inv;
                    zone.gas.co2         = acc.co2         * inv;
                    zone.gas.plasma      = acc.plasma      * inv;
                    zone.gas.n2o         = acc.n2o         * inv;
                    zone.gas.bz          = acc.bz          * inv;
                    zone.gas.tritium     = acc.tritium     * inv;
                    zone.gas.temperature = std::max(2.73f, acc.temperature * inv);
                } else {
                    // Cells came from freed area with no prior data (shouldn't
                    // normally happen, but initialise to normal station air).
                    zone.gas.o2          = 21.0f;
                    zone.gas.n2          = 80.0f;
                    zone.gas.temperature = 293.15f;
                }
            }

            m_zones[zid] = zone;
            for (const glm::ivec3& c : visited) m_cell_zone[c] = zid;

            zone_beyond_cells[zid] = std::move(beyond_cells);
            zone_door_voxels [zid] = std::move(door_voxels_found);
        }
        next_seed_partial:;
    }

    // ── 7. Second pass: build DoorLinks for the new zones ────────────────────
    // Pre-index existing links so we can extend rather than duplicate them.
    std::unordered_map<uint64_t, size_t> link_index;
    for (size_t i = 0; i < m_door_links.size(); ++i) {
        AtmosZoneID lo = std::min(m_door_links[i].zone_a, m_door_links[i].zone_b);
        AtmosZoneID hi = std::max(m_door_links[i].zone_a, m_door_links[i].zone_b);
        link_index[(static_cast<uint64_t>(lo) << 32) | hi] = i;
    }

    for (auto& [zid, bcells] : zone_beyond_cells) {
        auto it_a = m_zones.find(zid);
        if (it_a == m_zones.end()) continue;
        AtmosZone& za = it_a->second;

        const auto& dv_list = zone_door_voxels[zid];

        for (const glm::ivec3& bc : bcells) {
            auto cell_it = m_cell_zone.find(bc);
            AtmosZoneID adj = (cell_it != m_cell_zone.end())
                              ? cell_it->second : ATMOS_ZONE_SPACE;
            if (adj == zid || adj == ATMOS_ZONE_NULL) continue;

            AtmosZoneID lo  = std::min(zid, adj);
            AtmosZoneID hi  = std::max(zid, adj);
            uint64_t    key = (static_cast<uint64_t>(lo) << 32) | hi;

            if (!link_index.count(key)) {
                DoorLink lnk;
                lnk.zone_a    = lo;
                lnk.zone_b    = hi;
                link_index[key] = m_door_links.size();
                m_door_links.push_back(std::move(lnk));
            }
            DoorLink& lnk = m_door_links[link_index[key]];

            for (const glm::ivec3& dv : dv_list) {
                bool dup = false;
                for (auto& ex : lnk.door_voxels)
                    if (ex == dv) { dup = true; break; }
                if (!dup) lnk.door_voxels.push_back(dv);
            }

            // For open-door links to space, dv_list is typically empty: the
            // door voxel that just opened is now passable air so it was
            // consumed into 'visited' rather than recorded as a door voxel.
            // Without door voxels, process_door_links sees total==0 and skips
            // the link entirely — no gas drains.  Mirror what rebuild_zones does
            // for transient space zones: use the beyond-cell itself (a passable
            // air/space cell, open_frac == 1) so full CONDUCTANCE_SPACE is used.
            if (adj == ATMOS_ZONE_SPACE && lnk.door_voxels.empty()) {
                bool dup = false;
                for (auto& ex : lnk.door_voxels) if (ex == bc) { dup = true; break; }
                if (!dup) lnk.door_voxels.push_back(bc);
            }

            auto add_adj = [](std::vector<AtmosZoneID>& v, AtmosZoneID id) {
                for (auto x : v) if (x == id) return;
                v.push_back(id);
            };
            add_adj(za.adjacent_zones, adj);
            if (auto it_b = m_zones.find(adj); it_b != m_zones.end())
                add_adj(it_b->second.adjacent_zones, zid);
        }
    }

    // Recompute midpoints for all door links (new and updated).
    for (DoorLink& lnk : m_door_links) {
        if (lnk.door_voxels.empty()) continue;
        glm::ivec3 s{0};
        for (auto& dv : lnk.door_voxels) s += dv;
        lnk.midpoint = glm::vec3(s) / static_cast<float>(lnk.door_voxels.size())
                       + glm::vec3(0.5f);
    }
}

