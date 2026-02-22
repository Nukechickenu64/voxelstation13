#include "simulation/atmos.h"
#include <SDL3/SDL.h>
#include <queue>
#include <algorithm>
#include <cmath>

AtmosSimulator::AtmosSimulator(World& world)
    : m_world(world)
{
    // Pre-create the space/vacuum zone
    AtmosZone space;
    space.id       = ATMOS_ZONE_SPACE;
    space.is_space = true;
    // Space has zero pressure, very cold
    space.gas.temperature = 2.73f;
    m_zones[ATMOS_ZONE_SPACE] = std::move(space);
}

void AtmosSimulator::rebuild_zones()
{
    // Clear existing station zones (keep space zone)
    for (auto it = m_zones.begin(); it != m_zones.end();) {
        if (it->first != ATMOS_ZONE_SPACE) it = m_zones.erase(it);
        else ++it;
    }

    // BFS flood-fill over all non-solid exposed faces to build zones.
    // A face belongs to a zone if the voxel behind it is non-solid and
    // the voxel in front is also non-solid (or boundary to space).
    // TODO: full implementation; seed from all air voxels, group
    //       connected air regions, assign zone IDs to sim faces.
    SDL_Log("AtmosSimulator: rebuild_zones() — TODO: full BFS implementation");
}

void AtmosSimulator::tick(double dt)
{
    // Equalise pressure between adjacent zones
    for (auto& [id, zone] : m_zones) {
        if (zone.is_space) continue;
        for (AtmosZoneID adj_id : zone.adjacent_zones) {
            auto* adj = zone_at_id(adj_id);
            if (!adj) continue;
            equalise_pressure(zone, *adj, dt);
        }
        if (zone.has_hotspot)
            process_hotspot(zone, dt);
    }
}

// Helper not declared in header — used internally
AtmosZone* AtmosSimulator::zone_at_id(AtmosZoneID id)
{
    auto it = m_zones.find(id);
    return it != m_zones.end() ? &it->second : nullptr;
}

void AtmosSimulator::on_voxel_changed(glm::ivec3 /*pos*/)
{
    // TODO: detect if a wall was added/removed and split/merge zones accordingly.
}

AtmosZone* AtmosSimulator::zone(AtmosZoneID id)
{
    auto it = m_zones.find(id);
    return it != m_zones.end() ? &it->second : nullptr;
}

AtmosZoneID AtmosSimulator::zone_at(glm::ivec3 pos) const
{
    // Look up the sim face below this pos (floor face) for its zone
    VoxelFaceCoord coord{ pos, FaceDir::NegY };
    const VoxelFace* f = const_cast<World&>(m_world).get_sim_face(coord);
    return f ? f->atmos_id : ATMOS_ZONE_NULL;
}

GasMixture AtmosSimulator::mix_at(glm::ivec3 pos) const
{
    AtmosZoneID id = zone_at(pos);
    auto it = m_zones.find(id);
    if (it == m_zones.end()) return {};
    return it->second.gas;
}

void AtmosSimulator::try_ignite(AtmosZoneID id)
{
    auto* z = zone(id);
    if (!z) return;
    if (z->gas.plasma >= PLASMA_FIRE_O2_MIN &&
        z->gas.o2     >= PLASMA_FIRE_O2_MIN &&
        z->gas.temperature >= IGNITION_TEMPERATURE)
    {
        z->has_hotspot  = true;
        z->hotspot_temp = z->gas.temperature;
        SDL_Log("Atmos: ignition in zone %u!", id);
    }
}

void AtmosSimulator::equalise_pressure(AtmosZone& a, AtmosZone& b, double dt)
{
    float delta = a.gas.total_pressure() - b.gas.total_pressure();
    if (std::abs(delta) < PRESSURE_THRESHOLD) return;

    // Transfer a fraction of the delta each tick
    float transfer = delta * static_cast<float>(dt) * 0.5f;
    float ratio    = (a.gas.total_pressure() > 0.f)
                     ? transfer / a.gas.total_pressure() : 0.f;

    auto move = [&](float& src, float& dst) {
        float amount = src * ratio;
        src -= amount; dst += amount;
    };
    move(a.gas.o2,      b.gas.o2);
    move(a.gas.n2,      b.gas.n2);
    move(a.gas.co2,     b.gas.co2);
    move(a.gas.plasma,  b.gas.plasma);
    move(a.gas.n2o,     b.gas.n2o);
    move(a.gas.bz,      b.gas.bz);
    move(a.gas.tritium, b.gas.tritium);

    // Temperature averaging weighted by moles
    float total = a.gas.moles() + b.gas.moles();
    if (total > 0)
        a.gas.temperature = b.gas.temperature =
            (a.gas.temperature * a.gas.moles() +
             b.gas.temperature * b.gas.moles()) / total;
}

void AtmosSimulator::process_hotspot(AtmosZone& zone, double dt)
{
    if (zone.gas.plasma < 0.1f || zone.gas.o2 < 0.1f) {
        zone.has_hotspot = false;
        return;
    }
    float burn = std::min(zone.gas.plasma, zone.gas.o2) *
                 static_cast<float>(dt) * 0.4f;
    zone.gas.plasma      -= burn;
    zone.gas.o2          -= burn;
    zone.gas.co2         += burn * 0.5f;
    zone.gas.temperature += burn * 40.f;  // produces heat
}

void AtmosSimulator::apply_wind_force(AtmosZoneID /*id*/)
{
    // TODO: iterate mobs/items in zone, apply outward velocity impulse
    //       proportional to pressure differential.
}
