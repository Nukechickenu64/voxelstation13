// ── atmos.cpp ─────────────────────────────────────────────────────────────────
// TG-station13-style per-cell atmospheric simulation.
//
// Model summary:
//   - Every enclosed passable voxel has its own GasMixture ("zone", cell_count=1).
//   - m_open_edges holds every adjacent pair of enclosed cells; gas flows between
//     them at CONDUCTANCE_CELL each tick (TG share-ratio style).
//   - m_door_links holds door-voxel boundaries and space-drain edges.
//   - on_voxel_changed/on_door_changed do an O(1) local update; no full BFS.
//   - rebuild_zones() is only needed at startup or after a bulk map edit.
// ─────────────────────────────────────────────────────────────────────────────
#include "simulation/atmos.h"
#include "simulation/physics.h"
#include "simulation/mob_system.h"
#include "simulation/model_objects.h"
#include "simulation/status_effects.h"
#include "simulation/world_items.h"
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
        if (m_model_objects && m_model_objects->blocks_gas_at(pos)) return false;
        return true;
    }
    if (v.flags & VFLAG_GAS_PASSABLE) return true;
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

int AtmosSimulator::total_rooms() const { return m_num_regions; }

// ── rebuild_zones ─────────────────────────────────────────────────────────────
void AtmosSimulator::rebuild_zones()
{
    static constexpr glm::ivec3 k6[6] = {
        { 1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };

    // 0. Save per-cell gas
    std::unordered_map<glm::ivec3, GasMixture> saved;
    saved.reserve(m_cell_zone.size());
    for (auto& [pos, zid] : m_cell_zone) {
        auto it = m_zones.find(zid);
        if (it != m_zones.end()) saved.emplace(pos, it->second.gas);
    }

    // 1. Clear
    m_zones.clear();
    m_cell_zone.clear();
    m_open_edges.clear();
    m_door_links.clear();
    m_next_zone_id = 2;
    m_num_regions  = 0;

    {
        AtmosZone sp;
        sp.id = ATMOS_ZONE_SPACE;
        sp.is_space = true;
        sp.gas.temperature = 2.73f;
        m_zones[ATMOS_ZONE_SPACE] = std::move(sp);
    }

    // 2. Collect passable seeds from loaded chunks
    std::vector<glm::ivec3> seeds;
    m_world.for_each_chunk([&](const Chunk& chunk) {
        if (chunk.is_empty()) return;
        glm::ivec3 base = chunk.chunk_pos() * CHUNK_SIZE;
        for (int lx = 0; lx < CHUNK_SIZE; ++lx)
        for (int ly = 0; ly < CHUNK_SIZE; ++ly)
        for (int lz = 0; lz < CHUNK_SIZE; ++lz) {
            glm::ivec3 p = base + glm::ivec3{lx, ly, lz};
            if (voxel_is_passable(p)) seeds.push_back(p);
        }
    });

    // 3. BFS flood-fill to separate enclosed vs. open-space cells
    std::unordered_set<glm::ivec3> visited_global;
    std::unordered_set<glm::ivec3> space_cell_set;

    for (const glm::ivec3& seed : seeds) {
        if (visited_global.count(seed)) continue;

        std::unordered_set<glm::ivec3> region;
        std::queue<glm::ivec3>         q;
        region.insert(seed);
        q.push(seed);

        while (!q.empty()) {
            glm::ivec3 cur = q.front(); q.pop();
            for (const auto& d : k6) {
                glm::ivec3 nb = cur + d;
                if (!voxel_is_passable(nb)) continue;
                if (region.count(nb) || visited_global.count(nb)) continue;
                region.insert(nb);
                q.push(nb);
            }
            if (static_cast<int>(region.size()) > SPACE_THRESHOLD) {
                while (!q.empty()) { region.insert(q.front()); q.pop(); }
                break;
            }
        }

        bool is_space_region = static_cast<int>(region.size()) > SPACE_THRESHOLD;
        if (!is_space_region) ++m_num_regions;
        for (const glm::ivec3& p : region) {
            visited_global.insert(p);
            if (is_space_region) space_cell_set.insert(p);
        }
    }

    // 4. One zone per enclosed cell
    for (const glm::ivec3& pos : visited_global) {
        if (space_cell_set.count(pos)) continue;

        AtmosZoneID id = m_next_zone_id++;
        AtmosZone z;
        z.id = id;
        z.cell_count = 1;
        z.cell_pos = pos;

        auto sit = saved.find(pos);
        if (sit != saved.end()) {
            z.gas = sit->second;
        } else {
            z.gas.o2  = 21.0f;
            z.gas.n2  = 80.0f;
            z.gas.temperature = 293.15f;
        }
        m_zones[id] = std::move(z);
        m_cell_zone[pos] = id;
    }

    // 5. Build edges
    auto pair_key = [](AtmosZoneID a, AtmosZoneID b) -> uint64_t {
        if (a > b) std::swap(a, b);
        return (static_cast<uint64_t>(a) << 32) | b;
    };
    std::unordered_set<uint64_t> seen;

    for (auto& [pos, zid] : m_cell_zone) {
        AtmosZone& self_zone = m_zones[zid];
        for (const auto& d : k6) {
            glm::ivec3 nb = pos + d;
            auto nb_it = m_cell_zone.find(nb);
            if (nb_it != m_cell_zone.end()) {
                // Direct open adjacency
                AtmosZoneID nb_id = nb_it->second;
                uint64_t key = pair_key(zid, nb_id);
                if (!seen.count(key)) {
                    seen.insert(key);
                    m_open_edges.push_back({zid, nb_id});
                    self_zone.adjacent_zones.push_back(nb_id);
                    m_zones[nb_id].adjacent_zones.push_back(zid);
                }
            } else if (voxel_is_closed_door(nb)) {
                glm::ivec3 beyond = nb + d;
                auto byd_it = m_cell_zone.find(beyond);
                if (byd_it != m_cell_zone.end()) {
                    AtmosZoneID byd_id = byd_it->second;
                    uint64_t key = pair_key(zid, byd_id);
                    if (!seen.count(key)) {
                        seen.insert(key);
                        DoorLink lnk;
                        lnk.zone_a = std::min(zid, byd_id);
                        lnk.zone_b = std::max(zid, byd_id);
                        lnk.door_voxels.push_back(nb);
                        lnk.midpoint = glm::vec3(nb) + glm::vec3(0.5f);
                        m_door_links.push_back(std::move(lnk));
                        self_zone.adjacent_zones.push_back(byd_id);
                        m_zones[byd_id].adjacent_zones.push_back(zid);
                    }
                } else if (!m_cell_zone.count(beyond)) {
                    uint64_t key = pair_key(ATMOS_ZONE_SPACE, zid);
                    if (!seen.count(key)) {
                        seen.insert(key);
                        DoorLink lnk;
                        lnk.zone_a = ATMOS_ZONE_SPACE;
                        lnk.zone_b = zid;
                        lnk.door_voxels.push_back(nb);
                        m_door_links.push_back(std::move(lnk));
                    }
                }
            } else if (space_cell_set.count(nb)
                       || (voxel_is_passable(nb) && !m_cell_zone.count(nb))) {
                // Direct open-space adjacency
                uint64_t key = pair_key(ATMOS_ZONE_SPACE, zid);
                if (!seen.count(key)) {
                    seen.insert(key);
                    DoorLink lnk;
                    lnk.zone_a = ATMOS_ZONE_SPACE;
                    lnk.zone_b = zid;
                    lnk.door_voxels.push_back(pos);
                    m_door_links.push_back(std::move(lnk));
                } else {
                    for (auto& lnk : m_door_links) {
                        if ((lnk.zone_a == ATMOS_ZONE_SPACE && lnk.zone_b == zid)
                         || (lnk.zone_b == ATMOS_ZONE_SPACE && lnk.zone_a == zid)) {
                            bool dup = false;
                            for (auto& dv : lnk.door_voxels) if (dv == pos) { dup=true; break; }
                            if (!dup) lnk.door_voxels.push_back(pos);
                            break;
                        }
                    }
                }
            }
        }
    }

    // 6. Midpoints and eject_dirs
    for (DoorLink& lnk : m_door_links) {
        if (lnk.door_voxels.empty()) continue;
        glm::ivec3 s{0};
        for (auto& dv : lnk.door_voxels) s += dv;
        lnk.midpoint = glm::vec3(s) / float(lnk.door_voxels.size()) + glm::vec3(0.5f);

        if (lnk.zone_a == ATMOS_ZONE_SPACE || lnk.zone_b == ATMOS_ZONE_SPACE) {
            glm::vec3 dir_acc{};
            for (const glm::ivec3& dv : lnk.door_voxels) {
                for (const auto& d : k6) {
                    glm::ivec3 nb = dv + d;
                    if ((space_cell_set.count(nb) || !m_cell_zone.count(nb))
                            && voxel_is_passable(nb)) {
                        dir_acc += glm::vec3(d);
                        break;
                    }
                }
            }
            float len = glm::length(dir_acc);
            lnk.eject_dir = (len > 0.001f) ? (dir_acc / len) : glm::vec3{};
        }
    }
}

// ── tick ──────────────────────────────────────────────────────────────────────
void AtmosSimulator::tick(double dt)
{
    for (auto& [id, z] : m_zones) {
        if (z.is_space) continue;
        z.pressure_loss_rate = 0.f;
        z.vent_direction     = {};
    }

    // 1. Open cell-to-cell flow
    for (const CellEdge& edge : m_open_edges) {
        AtmosZone* za = zone_at_id(edge.za);
        AtmosZone* zb = zone_at_id(edge.zb);
        if (!za || !zb) continue;
        flow_cells(*za, *zb, dt, CONDUCTANCE_CELL);
    }

    // 1b. Temperature diffusion — equalises temperature between adjacent cells
    //     even when pressure is already balanced (conduction through air).
    for (const CellEdge& edge : m_open_edges) {
        AtmosZone* za = zone_at_id(edge.za);
        AtmosZone* zb = zone_at_id(edge.zb);
        if (!za || !zb) continue;
        diffuse_temperature(*za, *zb, dt);
    }

    // 2. Door/space links
    for (const DoorLink& lnk : m_door_links) {
        AtmosZone* za = zone_at_id(lnk.zone_a);
        AtmosZone* zb = zone_at_id(lnk.zone_b);
        if (!za || !zb) continue;

        int open_n = 0, closed_n = 0;
        for (const glm::ivec3& dv : lnk.door_voxels) {
            if      (voxel_is_passable   (dv)) ++open_n;
            else if (voxel_is_closed_door(dv)) ++closed_n;
        }
        int   total     = open_n + closed_n;
        float open_frac = (total > 0) ? float(open_n) / float(total) : 1.f;

        if (za->is_space || zb->is_space) {
            float cond = (open_frac > 0.f)
                         ? CONDUCTANCE_SPACE * open_frac
                         : CONDUCTANCE_SPACE_SEALED;
            AtmosZone* room = za->is_space ? zb : za;
            process_space_drain(*room, lnk, cond, dt);
        } else {
            float cond = CONDUCTANCE_CLOSED * (1.f - open_frac)
                       + CONDUCTANCE_CELL   * open_frac;
            flow_cells(*za, *zb, dt, cond);
        }
    }

    // 3. Hotspots
    for (auto& [id, zone] : m_zones)
        if (!zone.is_space && zone.has_hotspot)
            process_hotspot(zone, dt);

    // 4. Entity effects
    if (m_entities) apply_entity_effects(dt);

    // 5. Status update
    for (auto& [id, zone] : m_zones)
        if (!zone.is_space) update_status(zone);
}

// ── flow_cells ────────────────────────────────────────────────────────────────
void AtmosSimulator::flow_cells(AtmosZone& a, AtmosZone& b, double dt, float conductance)
{
    float Pa = a.gas.total_pressure();
    float Pb = b.gas.total_pressure();
    if (std::abs(Pa - Pb) < PRESSURE_THRESHOLD) return;

    float pre_Pa = Pa;
    move_gas_component(a.gas.o2,      b.gas.o2,      1.f, 1.f, conductance, dt);
    move_gas_component(a.gas.n2,      b.gas.n2,      1.f, 1.f, conductance, dt);
    move_gas_component(a.gas.co2,     b.gas.co2,     1.f, 1.f, conductance, dt);
    move_gas_component(a.gas.plasma,  b.gas.plasma,  1.f, 1.f, conductance, dt);
    move_gas_component(a.gas.n2o,     b.gas.n2o,     1.f, 1.f, conductance, dt);
    move_gas_component(a.gas.bz,      b.gas.bz,      1.f, 1.f, conductance, dt);
    move_gas_component(a.gas.tritium, b.gas.tritium, 1.f, 1.f, conductance, dt);

    float moles_change = pre_Pa - a.gas.total_pressure();
    if (moles_change > 0.f)  mix_temperature(b,  moles_change, a.gas.temperature);
    else if (moles_change < 0.f) mix_temperature(a, -moles_change, b.gas.temperature);
}

// ── process_space_drain ───────────────────────────────────────────────────────
void AtmosSimulator::process_space_drain(AtmosZone& zone, const DoorLink& lnk,
                                          float conductance, double dt)
{
    float step = std::min(1.f, conductance * float(dt));

    auto drain = [&](float& p) -> float {
        float lost = p * step;
        p -= lost;
        if (p < 0.f) { lost += p; p = 0.f; }
        return lost;
    };

    float drained = 0.f;
    drained += drain(zone.gas.o2);
    drained += drain(zone.gas.n2);
    drained += drain(zone.gas.co2);
    drained += drain(zone.gas.plasma);
    drained += drain(zone.gas.n2o);
    drained += drain(zone.gas.bz);
    drained += drain(zone.gas.tritium);

    zone.gas.temperature = std::max(
        2.73f, zone.gas.temperature - step * zone.gas.temperature * 0.25f);

    float kpa_s = (dt > 1e-9) ? drained / float(dt) : 0.f;
    zone.pressure_loss_rate += kpa_s;
    zone.vent_direction      = lnk.midpoint;
}

// ── move_gas_component ────────────────────────────────────────────────────────
void AtmosSimulator::move_gas_component(float& pa, float& pb,
                                         float Va, float Vb,
                                         float conductance, double dt)
{
    float n_total  = pa * Va + pb * Vb;
    float p_eq     = n_total / (Va + Vb);
    float excess_a = (pa - p_eq) * Va;
    if (std::abs(excess_a) < 1e-6f) return;

    float step = std::min(1.f, conductance * float(dt));
    float flow = excess_a * step;

    pa -= flow / Va;
    pb += flow / Vb;

    if (pa < 0.f) { pb += pa * (Va / Vb); pa = 0.f; }
    if (pb < 0.f) { pa += pb * (Vb / Va); pb = 0.f; }
}

// ── mix_temperature ───────────────────────────────────────────────────────────
void AtmosSimulator::mix_temperature(AtmosZone& dst, float added_moles, float src_temp)
{
    float existing = dst.gas.moles();
    float total    = existing + added_moles;
    if (total < 1e-6f) return;
    dst.gas.temperature = (dst.gas.temperature * existing + src_temp * added_moles) / total;
    dst.gas.temperature = std::max(2.73f, dst.gas.temperature);
}

// ── diffuse_temperature ───────────────────────────────────────────────────────
// Equalises temperature between adjacent cells through conduction, independent
// of pressure.  Runs after flow_cells each tick so that a pressurised but cold
// cell next to a hot cell will eventually reach thermal equilibrium.
void AtmosSimulator::diffuse_temperature(AtmosZone& a, AtmosZone& b, double dt)
{
    float diff = a.gas.temperature - b.gas.temperature;
    if (std::abs(diff) < 1.f) return;

    float step = std::min(0.5f, TEMP_DIFFUSION_RATE * float(dt));
    float flow = diff * step;

    a.gas.temperature -= flow;
    b.gas.temperature += flow;

    a.gas.temperature = std::max(2.73f, a.gas.temperature);
    b.gas.temperature = std::max(2.73f, b.gas.temperature);
}

// ── process_hotspot ───────────────────────────────────────────────────────────
void AtmosSimulator::process_hotspot(AtmosZone& zone, double dt)
{
    // Extinguish if there's not enough fuel or oxidiser
    if (zone.gas.plasma < 0.1f && zone.gas.tritium < 0.1f) {
        zone.has_hotspot  = false;
        zone.hotspot_temp = 0.f;
        return;
    }
    if (zone.gas.o2 < 0.1f) {
        zone.has_hotspot  = false;
        zone.hotspot_temp = 0.f;
        return;
    }

    float fdt     = float(dt);
    float t_ratio = std::max(1.f, zone.gas.temperature / IGNITION_TEMPERATURE);

    // Plasma fire (produces CO2 and heat)
    if (zone.gas.plasma > 0.1f) {
        float burn = std::min(zone.gas.plasma, zone.gas.o2) * fdt * 0.3f * t_ratio;
        zone.gas.plasma      -= burn;
        zone.gas.o2          -= burn;
        zone.gas.co2         += burn * 0.6f;
        zone.gas.temperature += burn * 35.f;
    }

    // Tritium fire — burns hotter and faster than plasma, no CO2 produced
    if (zone.gas.tritium > TRITIUM_FIRE_O2_MIN && zone.gas.o2 > TRITIUM_FIRE_O2_MIN) {
        float tri_burn = std::min(zone.gas.tritium, zone.gas.o2 * 0.5f)
                       * fdt * 0.5f * t_ratio;
        zone.gas.tritium     -= tri_burn;
        zone.gas.o2          -= tri_burn * 0.5f;
        zone.gas.temperature += tri_burn * TRITIUM_BURN_HEAT;
    }

    zone.hotspot_temp = zone.gas.temperature;

    // Spread fire to passable neighbours that have enough fuel+oxidiser
    if (zone.gas.temperature > FIRE_SPREAD_TEMP_THRESH) {
        static constexpr glm::ivec3 k6[6] = {
            { 1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
        };
        for (const auto& d : k6) {
            AtmosZoneID nid = zone_at(zone.cell_pos + d);
            if (nid == ATMOS_ZONE_NULL || nid == ATMOS_ZONE_SPACE) continue;
            AtmosZone* nz = zone_at_id(nid);
            if (!nz || nz->has_hotspot) continue;
            bool has_fuel = (nz->gas.plasma  >= FIRE_SPREAD_PLASMA_MIN)
                         || (nz->gas.tritium >= TRITIUM_FIRE_O2_MIN);
            if (has_fuel
             && nz->gas.o2          >= PLASMA_FIRE_O2_MIN
             && nz->gas.temperature >= IGNITION_TEMPERATURE * 0.85f)
            {
                nz->has_hotspot  = true;
                nz->hotspot_temp = nz->gas.temperature;
            }
        }
    }
}

// ── apply_entity_effects ──────────────────────────────────────────────────────
void AtmosSimulator::apply_entity_effects(double dt)
{
    float fdt = float(dt);

    // Breathing
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
        z->gas.o2  -= O2_CONSUMPTION_RATE * fdt;
        z->gas.co2 += CO2_PRODUCTION_RATE * fdt;
        if (z->gas.o2 < 0.f) z->gas.o2 = 0.f;
    });

    // ── Wind — TG-style pressure-gradient ─────────────────────────────────────
    // For each entity: sum the pressure differential across all 6 neighbours to
    // build a gradient vector.  Entities are pushed toward lower pressure (where
    // gas is actually escaping).  Status effects (Jitter/Dizzy/Knockdown) are
    // applied in the same pass based on the peak pressure difference.
    static constexpr glm::ivec3 k6w[6] = {
        { 1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };
    m_entities->each<VelocityComponent>([&](EntityID eid, VelocityComponent& vel) {
        auto* tr = m_entities->get_component<TransformComponent>(eid);
        if (!tr) return;
        glm::ivec3 feet = {
            static_cast<int>(std::floor(tr->pos.x)),
            static_cast<int>(std::floor(tr->pos.y)),
            static_cast<int>(std::floor(tr->pos.z))
        };

        // Pressure at entity's cell (0 if in space / untracked)
        AtmosZoneID zid = zone_at(feet);
        AtmosZone*  z   = zone_at_id(zid);
        float my_p = (z && !z->is_space) ? z->gas.total_pressure() : 0.f;

        // Sum directional pressure differentials to get wind gradient.
        // ATMOS_ZONE_NULL + solid voxel = wall — skip (walls don't pull gas).
        // ATMOS_ZONE_NULL + passable voxel = untracked open space outside the station — treat as vacuum (pressure 0).
        // ATMOS_ZONE_SPACE = large tracked space zone — also pressure 0.
        glm::vec3 grad{};
        float max_diff = 0.f;
        for (int i = 0; i < 6; ++i) {
            glm::ivec3 nb_pos = feet + k6w[i];
            AtmosZoneID nid = zone_at(nb_pos);
            if (nid == ATMOS_ZONE_NULL && !voxel_is_passable(nb_pos)) continue;  // solid wall — no flow
            float nbr_p;
            if (nid == ATMOS_ZONE_NULL || nid == ATMOS_ZONE_SPACE) {
                nbr_p = 0.f;   // open space / untracked void = perfect vacuum sink
            } else {
                AtmosZone* nz = zone_at_id(nid);
                nbr_p = (nz && !nz->is_space) ? nz->gas.total_pressure() : 0.f;
            }
            float diff = my_p - nbr_p;
            if (diff > 0.f) {
                grad += glm::vec3(k6w[i]) * diff;
                if (diff > max_diff) max_diff = diff;
            }
        }

        // Status effects — based on peak adjacent pressure differential
        if (auto* se = m_entities->get_component<StatusEffectsComponent>(eid)) {
            if (max_diff >= WIND_JITTER_THRESHOLD)
                se->apply(StatusEffectType::Jitter, 0.5f,
                          std::min(max_diff / WIND_KNOCKDOWN_THRESHOLD, 1.f));
            if (max_diff >= WIND_DIZZY_THRESHOLD)
                se->apply(StatusEffectType::Dizzy, 0.5f,
                          std::min(max_diff / WIND_KNOCKDOWN_THRESHOLD, 1.f));
            if (max_diff >= WIND_KNOCKDOWN_THRESHOLD) {
                float kd = std::clamp(
                    WIND_KNOCKDOWN_DUR_BASE * (max_diff / WIND_KNOCKDOWN_THRESHOLD),
                    WIND_KNOCKDOWN_DUR_BASE, WIND_KNOCKDOWN_DUR_MAX);
                se->apply(StatusEffectType::Knockdown, kd, 1.f);
            }
        }

        // Zone-level suction: if this entity's room is actively venting to space,
        // pull them directly toward the breach even before neighbour diffs build up.
        if (z && !z->is_space && z->pressure_loss_rate > 0.5f) {
            glm::vec3 to_vent = z->vent_direction - tr->pos;
            float     vdist   = glm::length(to_vent);
            if (vdist > 0.05f) {
                glm::vec3 vdir        = to_vent / vdist;
                float     suck_spd    = std::min(WIND_SPEED_FACTOR * std::pow(z->pressure_loss_rate, WIND_POWER_EXP * 0.8f), WIND_VEL_CAP);
                float     vel_along_v = glm::dot(vel.linear, vdir);
                float     zone_add    = std::min(suck_spd * fdt,
                                                 std::max(0.f, WIND_VEL_CAP - vel_along_v));
                if (zone_add > 0.f) {
                    vel.linear += vdir * zone_add;
                    // Wake any resting floor item so it slides toward the breach
                    if (auto* wic = m_entities->get_component<WorldItemComponent>(eid))
                        wic->is_resting = false;
                }
            }
        }

        // Physical wind push — only above effect threshold
        if (max_diff < WIND_EFFECT_THRESHOLD) return;
        float grad_len = glm::length(grad);
        if (grad_len < 0.001f) return;
        glm::vec3 wind_dir = grad / grad_len;   // unit vector toward low pressure

        // Items (no CharacterControllerComponent) are lighter — pushed more aggressively
        bool  is_item    = (m_entities->get_component<CharacterControllerComponent>(eid) == nullptr)
                        && (m_entities->get_component<WorldItemComponent>(eid) != nullptr);
        float item_scale = is_item ? 3.0f : 1.0f;

        // Scale wind speed with pressure difference (exponential — large diffs are disproportionately stronger)
        float wind_speed = WIND_SPEED_FACTOR * std::pow(max_diff, WIND_POWER_EXP) * item_scale;
        float vel_cap    = std::min(WIND_SPEED_FACTOR * std::pow(max_diff, WIND_POWER_EXP * 0.7f) + 1.f, WIND_VEL_CAP);
        float vel_along  = glm::dot(vel.linear, wind_dir);
        float add        = std::min(wind_speed * fdt, std::max(0.f, vel_cap - vel_along));

        auto* cc = m_entities->get_component<CharacterControllerComponent>(eid);
        if (cc && cc->on_ground) {
            if (max_diff >= WIND_KNOCKDOWN_THRESHOLD) {
                // Strong enough to launch off the floor (like TG throw)
                if (vel.linear.y < 0.f) vel.linear.y = 0.f;
                vel.linear.y += 0.8f;
            } else {
                add *= WIND_GROUND_RESIST;
            }
        }

        if (add <= 0.f) return;
        vel.linear += wind_dir * add;
        // Wake any resting floor item so physics can sweep it toward the breach
        if (auto* wic = m_entities->get_component<WorldItemComponent>(eid))
            wic->is_resting = false;
    });

    // ── Wind: resting floor items (no VelocityComponent yet) ──────────────────
    // Items dropped via WorldItemSystem::spawn() are created without a
    // VelocityComponent.  The pass above misses them entirely.  Here we iterate
    // all WorldItemComponents, and for those that are resting (or floating but
    // vel-less), compute the same gradient and — if wind is above threshold —
    // add a VelocityComponent and launch them toward the breach.
    m_entities->each<WorldItemComponent>([&](EntityID eid, WorldItemComponent& wic) {
        if (m_entities->get_component<VelocityComponent>(eid)) return; // already handled above

        auto* tr = m_entities->get_component<TransformComponent>(eid);
        if (!tr) return;

        glm::ivec3 feet = {
            static_cast<int>(std::floor(tr->pos.x)),
            static_cast<int>(std::floor(tr->pos.y)),
            static_cast<int>(std::floor(tr->pos.z))
        };

        AtmosZoneID zid = zone_at(feet);
        AtmosZone*  z   = zone_at_id(zid);
        float my_p = (z && !z->is_space) ? z->gas.total_pressure() : 0.f;

        glm::vec3 grad{};
        float max_diff = 0.f;
        for (int i = 0; i < 6; ++i) {
            glm::ivec3 nb_pos = feet + k6w[i];
            AtmosZoneID nid = zone_at(nb_pos);
            if (nid == ATMOS_ZONE_NULL && !voxel_is_passable(nb_pos)) continue;
            float nbr_p;
            if (nid == ATMOS_ZONE_NULL || nid == ATMOS_ZONE_SPACE) {
                nbr_p = 0.f;
            } else {
                AtmosZone* nz = zone_at_id(nid);
                nbr_p = (nz && !nz->is_space) ? nz->gas.total_pressure() : 0.f;
            }
            float diff = my_p - nbr_p;
            if (diff > 0.f) {
                grad += glm::vec3(k6w[i]) * diff;
                if (diff > max_diff) max_diff = diff;
            }
        }

        // Zone-level suction toward breach
        if (z && !z->is_space && z->pressure_loss_rate > 0.5f) {
            glm::vec3 to_vent = z->vent_direction - tr->pos;
            float     vdist   = glm::length(to_vent);
            if (vdist > 0.05f) {
                glm::vec3 vdir     = to_vent / vdist;
                float     suck_spd = std::min(WIND_SPEED_FACTOR * std::pow(z->pressure_loss_rate, WIND_POWER_EXP * 0.8f), WIND_VEL_CAP);
                auto& new_vel = m_entities->add_component<VelocityComponent>(eid);
                new_vel.linear = vdir * std::min(suck_spd * fdt, WIND_VEL_CAP);
                wic.is_resting = false;
                return;
            }
        }

        if (max_diff < WIND_EFFECT_THRESHOLD) return;
        float grad_len = glm::length(grad);
        if (grad_len < 0.001f) return;
        glm::vec3 wind_dir = grad / grad_len;

        float wind_speed = WIND_SPEED_FACTOR * std::pow(max_diff, WIND_POWER_EXP) * 3.0f; // items = 3× mass factor
        float vel_cap    = std::min(WIND_SPEED_FACTOR * std::pow(max_diff, WIND_POWER_EXP * 0.7f) + 1.f, WIND_VEL_CAP);
        float add        = std::min(wind_speed * fdt, vel_cap);
        if (add <= 0.f) return;

        auto& new_vel = m_entities->add_component<VelocityComponent>(eid);
        new_vel.linear = wind_dir * add;
        wic.is_resting = false;
    });

    // ── Atmospheric damage & gas status effects ────────────────────────────────
    // Applied to every mob each tick: oxy deprivation, toxins, burns from heat,
    // N2O drowsiness, BZ confusion, and tritium radiation poisoning.
    m_entities->each<MobPlayerTag>([&](EntityID eid, MobPlayerTag&) {
        auto* tr = m_entities->get_component<TransformComponent>(eid);
        auto* hp = m_entities->get_component<HealthComponent>(eid);
        auto* se = m_entities->get_component<StatusEffectsComponent>(eid);
        if (!tr || !hp || hp->dead) return;

        glm::ivec3 feet = {
            static_cast<int>(std::floor(tr->pos.x)),
            static_cast<int>(std::floor(tr->pos.y)),
            static_cast<int>(std::floor(tr->pos.z))
        };
        AtmosZoneID zid = zone_at(feet);

        // Open space — rapid oxy depletion
        if (zid == ATMOS_ZONE_SPACE) {
            hp->apply("oxy", SPACE_OXY_DAMAGE_RATE * fdt);
            return;
        }
        if (zid == ATMOS_ZONE_NULL) return;  // inside a solid voxel

        AtmosZone* z = zone_at_id(zid);
        if (!z) return;
        const GasMixture& g = z->gas;

        float total_p = g.total_pressure();

        // Healing — breathing fresh air recovers oxy damage only.
        // TG: good air heals oxy exclusively; brute/burn/tox require medical treatment.
        // In softcrit (health ≤ CRIT_THRESHOLD) the mob is breathing too laboriously
        // to self-recover — they instead take passive oxy damage and need CPR / meds.
        if (g.o2 >= OXY_HEAL_O2_MIN && total_p >= OXY_HEAL_PRESSURE_MIN) {
            if (!hp->crit) {
                hp->heal_type("oxy", OXY_HEAL_RATE * fdt);
            } else {
                // Softcrit: labored breathing — passive oxy loss (TG: 0.5/s)
                hp->apply("oxy", 0.5f * fdt);
            }
        }

        // Low total pressure — decompression oxy damage (scales to full rate near vacuum)
        if (total_p < LOW_PRESSURE_THRESHOLD) {
            float severity = std::clamp((LOW_PRESSURE_THRESHOLD - total_p)
                                        / LOW_PRESSURE_THRESHOLD, 0.f, 1.f);
            hp->apply("oxy", severity * DECOMP_DAMAGE_RATE * fdt);
        }

        // O2 deprivation — can occur even at normal total pressure (CO2-filled room)
        if (g.o2 < LOW_O2_THRESHOLD) {
            float severity = std::clamp((LOW_O2_THRESHOLD - g.o2)
                                        / LOW_O2_THRESHOLD, 0.f, 1.f);
            hp->apply("oxy", severity * OXY_DAMAGE_RATE * fdt);
        }

        // High-pressure barotrauma — crushing atmosphere (> 550 kPa)
        if (total_p > BARO_PRESSURE_THRESHOLD) {
            float excess = (total_p - BARO_PRESSURE_THRESHOLD) / 100.f;
            float rate   = std::min(BARO_BASE_RATE + excess * BARO_EXCESS_SCALE,
                                    BARO_MAX_RATE);
            hp->apply("brute", rate * fdt);
            hp->apply("burn",  rate * 0.5f * fdt);
        }
        // Low-pressure decompression sickness — nitrogen bubbles (1–10 kPa)
        else if (total_p > 0.5f && total_p < DECOMP_BARO_MAX_P) {
            float scale = std::clamp((DECOMP_BARO_MAX_P - total_p)
                                     / (DECOMP_BARO_MAX_P - 0.5f), 0.f, 1.f);
            hp->apply("brute", DECOMP_BARO_RATE * scale * fdt);
        }

        // CO2 toxicity — hypercapnia
        if (g.co2 > HIGH_CO2_THRESHOLD) {
            float severity = std::clamp((g.co2 - HIGH_CO2_THRESHOLD) / 20.f,
                                        0.f, 1.f);
            hp->apply("tox", severity * CO2_TOX_RATE * fdt);
        }

        // Heat burns — above ignition threshold
        if (g.temperature > HIGH_TEMP_THRESHOLD) {
            float excess = g.temperature - HIGH_TEMP_THRESHOLD;
            hp->apply("burn", std::min(excess * HEAT_BURN_RATE, 2.f) * fdt);
        }

        // Plasma inhalation toxicity
        if (g.plasma > 0.5f) {
            float severity = std::clamp(g.plasma / 20.f, 0.f, 1.f);
            hp->apply("tox", severity * PLASMA_TOX_RATE * fdt);
        }

        // N2O — laughing gas: Drowsy, then Confusion at high concentrations
        if (g.n2o > N2O_DROWSY_THRESHOLD) {
            float severity = std::clamp((g.n2o - N2O_DROWSY_THRESHOLD) / 30.f,
                                        0.f, 1.f);
            if (se) {
                se->apply(StatusEffectType::Drowsy,   2.f, severity);
                se->apply(StatusEffectType::Jitter,   1.f, severity * 0.4f);
                if (g.n2o > N2O_CONFUSION_THRESHOLD)
                    se->apply(StatusEffectType::Confusion, 1.5f, severity);
            }
            hp->apply("tox", severity * N2O_TOX_RATE * fdt);
        }

        // BZ — psychoactive: Confusion + tox
        if (g.bz > 0.5f) {
            float severity = std::clamp(g.bz / 5.f, 0.f, 1.f);
            if (se) se->apply(StatusEffectType::Confusion, 3.f, severity);
            hp->apply("tox", severity * BZ_TOX_RATE * fdt);
        }

        // Tritium — radiation poisoning
        if (g.tritium > 0.1f) {
            float severity = std::clamp(g.tritium / 10.f, 0.f, 1.f);
            hp->apply("tox", severity * TRITIUM_TOX_RATE * fdt);
        }
    });
}

// ── update_status ─────────────────────────────────────────────────────────────
void AtmosSimulator::update_status(AtmosZone& zone)
{
    uint8_t s = ATMOS_OK;
    const GasMixture& g = zone.gas;
    if (g.o2 < 16.f)                                     s |= ATMOS_LOW_O2;
    if (g.total_pressure() < 50.f)                       s |= ATMOS_LOW_PRESS;
    if (g.co2 > 5.f)                                     s |= ATMOS_HIGH_CO2;
    if (g.plasma > 0.5f || g.bz > 0.5f || g.n2o > 5.f)  s |= ATMOS_TOXIC;
    if (zone.has_hotspot)                                 s |= ATMOS_FIRE;
    if (zone.pressure_loss_rate > 1.f)                    s |= ATMOS_DECOMP;
    if (g.temperature > 360.f)                            s |= ATMOS_HIGH_TEMP;
    zone.status = s;
}

// ── Public accessors ──────────────────────────────────────────────────────────
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
        SDL_Log("Atmos: ignition in cell %u!", id);
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

// ── check_is_space ────────────────────────────────────────────────────────────
// Returns true if pos connects to open space (BFS > SPACE_THRESHOLD or
// immediately finds an untracked passable neighbour outside m_cell_zone).
bool AtmosSimulator::check_is_space(glm::ivec3 pos) const
{
    static constexpr glm::ivec3 k6[6] = {
        { 1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };

    // Fast path: any passable direct neighbour not yet tracked = potential space
    bool need_bfs = false;
    for (const auto& d : k6) {
        glm::ivec3 nb = pos + d;
        if (voxel_is_passable(nb) && !voxel_is_closed_door(nb)
                && !m_cell_zone.count(nb)) {
            need_bfs = true;
            break;
        }
    }
    if (!need_bfs) return false;

    std::unordered_set<glm::ivec3> visited;
    std::queue<glm::ivec3>         q;
    visited.insert(pos);
    q.push(pos);

    while (!q.empty()) {
        if (static_cast<int>(visited.size()) > SPACE_THRESHOLD) return true;
        glm::ivec3 cur = q.front(); q.pop();
        for (const auto& d : k6) {
            glm::ivec3 nb = cur + d;
            if (!voxel_is_passable(nb)) continue;
            if (visited.count(nb)) continue;
            if (!m_cell_zone.count(nb)) return true;  // untracked = open void
            visited.insert(nb);
            q.push(nb);
        }
    }
    return false;
}

// ── remove_cell ───────────────────────────────────────────────────────────────
void AtmosSimulator::remove_cell(glm::ivec3 pos)
{
    static constexpr glm::ivec3 k6[6] = {
        { 1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };

    auto cit = m_cell_zone.find(pos);
    if (cit == m_cell_zone.end()) return;
    AtmosZoneID id = cit->second;

    m_open_edges.erase(
        std::remove_if(m_open_edges.begin(), m_open_edges.end(),
            [&](const CellEdge& e){ return e.za == id || e.zb == id; }),
        m_open_edges.end());

    m_door_links.erase(
        std::remove_if(m_door_links.begin(), m_door_links.end(),
            [&](const DoorLink& lnk){
                if (lnk.zone_a == id || lnk.zone_b == id) return true;
                for (auto& dv : lnk.door_voxels) if (dv == pos) return true;
                return false;
            }),
        m_door_links.end());

    for (const auto& d : k6) {
        auto nit = m_cell_zone.find(pos + d);
        if (nit == m_cell_zone.end()) continue;
        auto& nbz = m_zones[nit->second];
        nbz.adjacent_zones.erase(
            std::remove(nbz.adjacent_zones.begin(), nbz.adjacent_zones.end(), id),
            nbz.adjacent_zones.end());
    }

    m_zones.erase(id);
    m_cell_zone.erase(cit);
}

// ── rebuild_cell_edges ────────────────────────────────────────────────────────
void AtmosSimulator::rebuild_cell_edges(glm::ivec3 pos)
{
    static constexpr glm::ivec3 k6[6] = {
        { 1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };

    auto cit = m_cell_zone.find(pos);
    if (cit == m_cell_zone.end()) return;
    AtmosZoneID id   = cit->second;
    AtmosZone&  self = m_zones[id];

    auto has_open_edge = [&](AtmosZoneID other) {
        for (auto& e : m_open_edges)
            if ((e.za == id && e.zb == other) || (e.za == other && e.zb == id))
                return true;
        return false;
    };
    auto has_door_link_to = [&](AtmosZoneID other) {
        for (auto& lnk : m_door_links)
            if ((lnk.zone_a == id && lnk.zone_b == other)
             || (lnk.zone_a == other && lnk.zone_b == id))
                return true;
        return false;
    };
    auto find_space_link = [&]() -> DoorLink* {
        for (auto& lnk : m_door_links)
            if ((lnk.zone_a == ATMOS_ZONE_SPACE && lnk.zone_b == id)
             || (lnk.zone_b == ATMOS_ZONE_SPACE && lnk.zone_a == id))
                return &lnk;
        return nullptr;
    };

    for (const auto& d : k6) {
        glm::ivec3 nb = pos + d;

        auto nb_it = m_cell_zone.find(nb);
        if (nb_it != m_cell_zone.end()) {
            AtmosZoneID nb_id = nb_it->second;
            if (!has_open_edge(nb_id)) {
                m_open_edges.push_back({id, nb_id});
                self.adjacent_zones.push_back(nb_id);
                m_zones[nb_id].adjacent_zones.push_back(id);
            }
        } else if (voxel_is_closed_door(nb)) {
            glm::ivec3 beyond = nb + d;
            auto byd_it = m_cell_zone.find(beyond);
            if (byd_it != m_cell_zone.end()) {
                AtmosZoneID byd_id = byd_it->second;
                if (!has_door_link_to(byd_id)) {
                    DoorLink lnk;
                    lnk.zone_a = std::min(id, byd_id);
                    lnk.zone_b = std::max(id, byd_id);
                    lnk.door_voxels.push_back(nb);
                    lnk.midpoint = glm::vec3(nb) + glm::vec3(0.5f);
                    m_door_links.push_back(std::move(lnk));
                    self.adjacent_zones.push_back(byd_id);
                    m_zones[byd_id].adjacent_zones.push_back(id);
                }
            }
        } else if (voxel_is_passable(nb) && !m_cell_zone.count(nb)) {
            // Breach face adjacent to space
            DoorLink* sl = find_space_link();
            if (!sl) {
                DoorLink lnk;
                lnk.zone_a    = ATMOS_ZONE_SPACE;
                lnk.zone_b    = id;
                lnk.door_voxels.push_back(pos);
                lnk.midpoint  = glm::vec3(pos) + glm::vec3(0.5f);
                lnk.eject_dir = glm::vec3(d);
                m_door_links.push_back(std::move(lnk));
            } else {
                bool dup = false;
                for (auto& dv : sl->door_voxels) if (dv == pos) { dup=true; break; }
                if (!dup) {
                    sl->door_voxels.push_back(pos);
                    glm::ivec3 s{0};
                    for (auto& dv : sl->door_voxels) s += dv;
                    sl->midpoint = glm::vec3(s) / float(sl->door_voxels.size())
                                 + glm::vec3(0.5f);
                }
            }
        }
    }
}

// ── on_voxel_changed ──────────────────────────────────────────────────────────
void AtmosSimulator::on_voxel_changed(glm::ivec3 pos)
{
    static constexpr glm::ivec3 k6[6] = {
        { 1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };

    // Save gas before removing (so it isn't lost if the cell type changed).
    GasMixture saved_gas;
    bool had_zone = false;
    {
        auto cit = m_cell_zone.find(pos);
        if (cit != m_cell_zone.end()) {
            had_zone = true;
            auto zit = m_zones.find(cit->second);
            if (zit != m_zones.end()) saved_gas = zit->second.gas;
        }
    }

    // Remove old zone + its edges
    remove_cell(pos);

    // Also remove door links where pos appears as a door voxel
    m_door_links.erase(
        std::remove_if(m_door_links.begin(), m_door_links.end(),
            [&](const DoorLink& lnk){
                for (auto& dv : lnk.door_voxels) if (dv == pos) return true;
                return false;
            }),
        m_door_links.end());

    // If pos is now passable and enclosed: create its new zone
    if (voxel_is_passable(pos) && !check_is_space(pos)) {
        AtmosZoneID new_id = m_next_zone_id++;
        AtmosZone z;
        z.id         = new_id;
        z.cell_count = 1;
        z.cell_pos   = pos;

        if (had_zone) {
            z.gas = saved_gas;
        } else {
            // New cell: average gas from enclosed neighbours
            GasMixture acc{};
            acc.temperature = 0.f;
            float w = 0.f;
            for (const auto& d : k6) {
                auto nit = m_cell_zone.find(pos + d);
                if (nit == m_cell_zone.end()) continue;
                auto zit = m_zones.find(nit->second);
                if (zit == m_zones.end()) continue;
                const GasMixture& ng = zit->second.gas;
                acc.o2 += ng.o2; acc.n2 += ng.n2; acc.co2 += ng.co2;
                acc.plasma += ng.plasma; acc.n2o += ng.n2o;
                acc.bz += ng.bz; acc.tritium += ng.tritium;
                acc.temperature += ng.temperature;
                w += 1.f;
            }
            if (w > 0.f) {
                float inv = 1.f / w;
                z.gas.o2 = acc.o2*inv; z.gas.n2 = acc.n2*inv;
                z.gas.co2 = acc.co2*inv; z.gas.plasma = acc.plasma*inv;
                z.gas.n2o = acc.n2o*inv; z.gas.bz = acc.bz*inv;
                z.gas.tritium = acc.tritium*inv;
                z.gas.temperature = std::max(2.73f, acc.temperature*inv);
            } else {
                z.gas.o2 = 21.0f; z.gas.n2 = 80.0f;
                z.gas.temperature = 293.15f;
            }
        }
        m_zones[new_id] = std::move(z);
        m_cell_zone[pos] = new_id;
        rebuild_cell_edges(pos);
    }

    // Rebuild edges for all 6 neighbours (their connection sets changed)
    for (const auto& d : k6) {
        glm::ivec3 nb = pos + d;
        auto nit = m_cell_zone.find(nb);
        if (nit == m_cell_zone.end()) continue;

        AtmosZoneID nb_id = nit->second;

        // Remove all of nb's edges/links then rebuild
        m_open_edges.erase(
            std::remove_if(m_open_edges.begin(), m_open_edges.end(),
                [&](const CellEdge& e){ return e.za == nb_id || e.zb == nb_id; }),
            m_open_edges.end());
        m_door_links.erase(
            std::remove_if(m_door_links.begin(), m_door_links.end(),
                [&](const DoorLink& lnk){
                    return lnk.zone_a == nb_id || lnk.zone_b == nb_id;
                }),
            m_door_links.end());
        m_zones[nb_id].adjacent_zones.clear();

        rebuild_cell_edges(nb);
    }
}

// ── on_door_changed ───────────────────────────────────────────────────────────
// A door voxel opened or closed.  The DoorLink conductance is recalculated
// in tick() by checking voxel_is_passable on door_voxels, so we only need
// to handle the case where the link is missing (rebuild it) or where an open
// door now bridges two cells directly (convert door link to open edge).
void AtmosSimulator::on_door_changed(glm::ivec3 pos)
{
    // Check if a DoorLink already covers this door voxel
    for (const auto& lnk : m_door_links)
        for (const auto& dv : lnk.door_voxels)
            if (dv == pos) return;  // link exists, tick() handles conductance

    // No link found — fall back to full local voxel update
    on_voxel_changed(pos);
}
