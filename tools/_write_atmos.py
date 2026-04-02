"""Write the new per-cell atmos.cpp."""
import os

SRC = r'C:\Users\nukec\Documents\GitHub\voxelstation13\src\simulation'

CPP = r'''// ── atmos.cpp ─────────────────────────────────────────────────────────────────
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

// ── process_hotspot ───────────────────────────────────────────────────────────
void AtmosSimulator::process_hotspot(AtmosZone& zone, double dt)
{
    if (zone.gas.plasma < 0.1f || zone.gas.o2 < 0.1f) {
        zone.has_hotspot  = false;
        zone.hotspot_temp = 0.f;
        return;
    }
    float fdt     = float(dt);
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

    // Wind
    m_entities->each<VelocityComponent>([&](EntityID eid, VelocityComponent& vel) {
        auto* tr = m_entities->get_component<TransformComponent>(eid);
        if (!tr) return;
        glm::ivec3 feet = {
            static_cast<int>(std::floor(tr->pos.x)),
            static_cast<int>(std::floor(tr->pos.y)),
            static_cast<int>(std::floor(tr->pos.z))
        };
        AtmosZoneID zid = zone_at(feet);
        AtmosZone* z = zone_at_id(zid);

        if (!z || z->is_space || zid == ATMOS_ZONE_NULL || zid == ATMOS_ZONE_SPACE) {
            static constexpr glm::ivec3 k6[6] = {
                { 1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
            };
            AtmosZone* best = nullptr;
            for (const auto& d : k6) {
                AtmosZoneID nid = zone_at(feet + d);
                AtmosZone*  nz  = zone_at_id(nid);
                if (!nz || nz->is_space) continue;
                if (nz->pressure_loss_rate < WIND_THRESHOLD) continue;
                if (!best || nz->pressure_loss_rate > best->pressure_loss_rate)
                    best = nz;
            }
            if (!best) return;
            z = best;
        } else if (z->pressure_loss_rate < WIND_THRESHOLD) {
            return;
        }

        glm::vec3 to_vent = z->vent_direction - tr->pos;
        float dist = glm::length(to_vent);
        glm::vec3 wind_dir;
        if (dist < 0.5f) {
            float vlen = glm::length(vel.linear);
            if (vlen < 0.01f) return;
            wind_dir = vel.linear / vlen;
        } else {
            wind_dir = to_vent / dist;
        }

        float impulse = z->pressure_loss_rate * WIND_ACCEL_PER_KPA_S * fdt;
        auto* cc = m_entities->get_component<CharacterControllerComponent>(eid);
        bool explosive = (z->pressure_loss_rate >= WIND_KNOCKDOWN_THRESHOLD);
        if (cc && cc->on_ground) {
            if (explosive) {
                if (vel.linear.y < 0.f) vel.linear.y = 0.f;
                vel.linear.y += 1.0f;
            } else {
                impulse *= WIND_GROUND_RESIST;
            }
        }

        float vel_cap   = std::min(z->pressure_loss_rate * 0.5f + 1.f, WIND_VEL_CAP);
        float vel_along = glm::dot(vel.linear, wind_dir);
        float add = std::min(impulse, std::max(0.f, vel_cap - vel_along));
        if (add <= 0.f) return;
        vel.linear += wind_dir * add;
    });

    // Breach ejector
    constexpr float BREACH_EJECT_RADIUS = 2.0f;
    constexpr float BREACH_EJECT_SPEED  = 10.f;
    for (const DoorLink& lnk : m_door_links) {
        bool is_space_link = (lnk.zone_a == ATMOS_ZONE_SPACE || lnk.zone_b == ATMOS_ZONE_SPACE);
        if (!is_space_link || glm::length(lnk.eject_dir) < 0.001f) continue;
        AtmosZoneID room_id = (lnk.zone_a == ATMOS_ZONE_SPACE) ? lnk.zone_b : lnk.zone_a;
        AtmosZone* room = zone_at_id(room_id);
        if (!room || room->pressure_loss_rate < WIND_KNOCKDOWN_THRESHOLD) continue;

        m_entities->each<VelocityComponent>([&](EntityID eid, VelocityComponent& vel) {
            auto* tr = m_entities->get_component<TransformComponent>(eid);
            if (!tr) return;
            if (glm::length(tr->pos - lnk.midpoint) > BREACH_EJECT_RADIUS) return;
            if (glm::dot(vel.linear, lnk.eject_dir) < BREACH_EJECT_SPEED)
                vel.linear = lnk.eject_dir * BREACH_EJECT_SPEED;
        });
    }

    // Status effects
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
        if (rate >= WIND_JITTER_THRESHOLD)
            se.apply(StatusEffectType::Jitter, 0.5f,
                     std::min(rate / WIND_KNOCKDOWN_THRESHOLD, 1.f));
        if (rate >= WIND_DIZZY_THRESHOLD)
            se.apply(StatusEffectType::Dizzy, 0.5f,
                     std::min(rate / WIND_KNOCKDOWN_THRESHOLD, 1.f));
        if (rate >= WIND_KNOCKDOWN_THRESHOLD) {
            float kd = std::clamp(
                WIND_KNOCKDOWN_DUR_BASE * (rate / WIND_KNOCKDOWN_THRESHOLD),
                WIND_KNOCKDOWN_DUR_BASE, WIND_KNOCKDOWN_DUR_MAX);
            se.apply(StatusEffectType::Knockdown, kd, 1.f);
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
'''

dst = os.path.join(SRC, 'atmos.cpp')
with open(dst, 'w', encoding='utf-8') as f:
    f.write(CPP)
print(f"Written {len(CPP)} bytes to {dst}")
