// ─────────────────────────────────────────────────────────────────────────────
//  VehicleManager — implementation
// ─────────────────────────────────────────────────────────────────────────────
#include "simulation/vehicle_manager.h"
#include "simulation/physics.h"  // TransformComponent
#include <algorithm>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
//  Construction
// ─────────────────────────────────────────────────────────────────────────────

VehicleManager::VehicleManager(World& map_world, AtmosSimulator& map_atmos,
                               EntityManager& map_entities, PhysicsSystem& map_physics)
    : m_map_world(map_world)
    , m_map_atmos(map_atmos)
    , m_map_entities(map_entities)
    , m_map_physics(map_physics)
{}

// ─────────────────────────────────────────────────────────────────────────────
//  Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

VehicleGrid* VehicleManager::create_vehicle(const std::string& name)
{
    VehicleID id = m_next_id++;
    auto& v = m_vehicles.emplace_back(std::make_unique<VehicleGrid>(id, name));
    return v.get();
}

void VehicleManager::destroy_vehicle(VehicleID id)
{
    undock(id);  // safe no-op if not docked
    m_vehicles.erase(
        std::remove_if(m_vehicles.begin(), m_vehicles.end(),
            [id](const std::unique_ptr<VehicleGrid>& v){ return v->id() == id; }),
        m_vehicles.end());
}

VehicleGrid* VehicleManager::get_vehicle(VehicleID id)
{
    for (auto& v : m_vehicles)
        if (v->id() == id) return v.get();
    return nullptr;
}

const VehicleGrid* VehicleManager::get_vehicle(VehicleID id) const
{
    for (const auto& v : m_vehicles)
        if (v->id() == id) return v.get();
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Docking
// ─────────────────────────────────────────────────────────────────────────────

bool VehicleManager::dock(VehicleID vehicle_id,
                          glm::ivec3 map_port_pos, FaceDir map_port_face)
{
    VehicleGrid* v = get_vehicle(vehicle_id);
    if (!v)                                      return false;
    if (v->state() == VehicleState::Docked)      return false;
    if (!v->has_dock_port())                     return false;

    const DockPort& vport = v->dock_port();

    // Compute the anchor so the vehicle dock port aligns with the map dock port.
    //
    // The map port face normal points INTO the vehicle; the vehicle's local
    // origin [0,0,0] maps to `anchor` in world space.
    //
    //   vehicle_port_in_world = vport.local_pos + anchor
    //   map_port_in_world     = map_port_pos + face_normal(map_port_face)
    //
    // Equate them:
    //   anchor = map_port_pos + face_normal(map_port_face) - vport.local_pos
    glm::ivec3 anchor = map_port_pos + face_normal(map_port_face) - vport.local_pos;
    v->set_anchor(anchor);

    v->set_state(VehicleState::Docking);
    v->set_state(VehicleState::Docked);           // instant — add timer here for animation

    DockLink link;
    link.vehicle_id   = vehicle_id;
    link.vehicle_port = vport;
    link.map_port_pos  = map_port_pos;
    link.map_port_face = map_port_face;
    link.hatch_open    = false;
    m_dock_links.push_back(link);

    return true;
}

void VehicleManager::undock(VehicleID vehicle_id)
{
    VehicleGrid* v = get_vehicle(vehicle_id);
    if (!v) return;
    if (v->state() != VehicleState::Docked &&
        v->state() != VehicleState::Docking) return;

    // Remove dock link
    m_dock_links.erase(
        std::remove_if(m_dock_links.begin(), m_dock_links.end(),
            [vehicle_id](const DockLink& l){ return l.vehicle_id == vehicle_id; }),
        m_dock_links.end());

    v->set_state(VehicleState::Undocking);
    v->set_state(VehicleState::InSpace);           // instant — add timer here for animation
}

// ─────────────────────────────────────────────────────────────────────────────
//  Hatch control
// ─────────────────────────────────────────────────────────────────────────────

void VehicleManager::set_hatch_open(VehicleID vehicle_id, bool open)
{
    if (DockLink* lnk = find_dock_link(vehicle_id))
        lnk->hatch_open = open;
}

bool VehicleManager::is_hatch_open(VehicleID vehicle_id) const
{
    if (const DockLink* lnk = find_dock_link(vehicle_id))
        return lnk->hatch_open;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Entity transfer
// ─────────────────────────────────────────────────────────────────────────────

void VehicleManager::transfer_entity_to_vehicle(EntityID id, VehicleID vehicle_id)
{
    VehicleGrid* v = get_vehicle(vehicle_id);
    if (!v || !m_map_entities.alive(id)) return;

    EntityManager& ve = v->entities();

    // Register the entity as alive in the vehicle's manager before
    // adding components (mirrors what EntityManager::create() does).
    ve.adopt(id);

    // Adjust transform: map world pos → vehicle local pos
    if (TransformComponent* tc = m_map_entities.get_component<TransformComponent>(id)) {
        TransformComponent new_tc = *tc;
        new_tc.pos      = tc->pos      - glm::vec3(v->anchor());
        new_tc.prev_pos = tc->prev_pos - glm::vec3(v->anchor());
        ve.add_component<TransformComponent>(id, new_tc);
    }

    // Copy VelocityComponent if present
    if (VelocityComponent* vc = m_map_entities.get_component<VelocityComponent>(id)) {
        ve.add_component<VelocityComponent>(id, *vc);
    }

    // Copy CharacterControllerComponent if present
    if (CharacterControllerComponent* cc =
            m_map_entities.get_component<CharacterControllerComponent>(id)) {
        ve.add_component<CharacterControllerComponent>(id, *cc);
    }

    // Remove from map entity manager
    m_map_entities.destroy(id);
}

void VehicleManager::transfer_entity_to_map(EntityID id, VehicleID vehicle_id)
{
    VehicleGrid* v = get_vehicle(vehicle_id);
    if (!v) return;

    EntityManager& ve = v->entities();
    if (!ve.alive(id)) return;

    // Adjust transform: vehicle local pos → map world pos
    // Register the entity as alive in the map's manager.
    m_map_entities.adopt(id);

    if (TransformComponent* tc = ve.get_component<TransformComponent>(id)) {
        TransformComponent new_tc = *tc;
        new_tc.pos      = tc->pos      + glm::vec3(v->anchor());
        new_tc.prev_pos = tc->prev_pos + glm::vec3(v->anchor());
        m_map_entities.add_component<TransformComponent>(id, new_tc);
    }

    if (VelocityComponent* vc = ve.get_component<VelocityComponent>(id))
        m_map_entities.add_component<VelocityComponent>(id, *vc);

    if (CharacterControllerComponent* cc =
            ve.get_component<CharacterControllerComponent>(id))
        m_map_entities.add_component<CharacterControllerComponent>(id, *cc);

    ve.destroy(id);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Spatial lookup
// ─────────────────────────────────────────────────────────────────────────────

VehicleGrid* VehicleManager::vehicle_at_map_pos(glm::ivec3 map_pos)
{
    for (auto& v : m_vehicles) {
        if (v->state() != VehicleState::Docked) continue;
        // Check whether the map-pos falls within the vehicle's chunk extents.
        // We check if the vehicle's world has a non-air voxel at the translated
        // local position — this naturally respects the vehicle's actual shape.
        glm::ivec3 local = v->to_local_pos(map_pos);
        Voxel vx = v->world().get_voxel(local);
        if (vx.type_id != 0) return v.get();  // non-air → inside vehicle
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Atmospheric gas exchange
// ─────────────────────────────────────────────────────────────────────────────
// Replicates AtmosSimulator::equalise_zones() math across two independent
// AtmosSimulator instances (vehicle atmos ↔ map atmos).

static void exchange_gas_component(float& pa, float& pb,
                                   float Va, float Vb,
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

static void mix_temp(GasMixture& dst, float added_moles, float src_temp, float cells)
{
    float existing = dst.moles() * std::max(1.f, cells);
    float total    = existing + added_moles;
    if (total < 1e-6f) return;
    dst.temperature = (dst.temperature * existing + src_temp * added_moles) / total;
    dst.temperature = std::max(2.73f, dst.temperature);
}

void VehicleManager::exchange_atmos(DockLink& link, double dt)
{
    VehicleGrid* v = get_vehicle(link.vehicle_id);
    if (!v) return;

    // Map-side zone at the dock port
    AtmosZoneID map_zone_id = m_map_atmos.zone_at(link.map_port_pos);
    AtmosZone*  map_zone    = m_map_atmos.zone(map_zone_id);
    if (!map_zone || map_zone->is_space) return;

    // Vehicle-side zone at the dock port (vehicle-local coords)
    AtmosSimulator& vatmos     = v->atmos();
    AtmosZoneID     veh_zone_id = vatmos.zone_at(link.vehicle_port.local_pos);
    AtmosZone*      veh_zone    = vatmos.zone(veh_zone_id);
    if (!veh_zone || veh_zone->is_space) return;

    float cond = link.hatch_open ? HATCH_CONDUCTANCE : HATCH_CONDUCTANCE_CLOSED;
    float Pa   = map_zone->gas.total_pressure();
    float Pb   = veh_zone->gas.total_pressure();
    if (std::abs(Pa - Pb) < 0.25f) return;  // PRESSURE_THRESHOLD

    float Va = std::max(1.f, static_cast<float>(map_zone->cell_count));
    float Vb = std::max(1.f, static_cast<float>(veh_zone->cell_count));
    float pre_Pa = Pa;

    exchange_gas_component(map_zone->gas.o2,      veh_zone->gas.o2,      Va, Vb, cond, dt);
    exchange_gas_component(map_zone->gas.n2,      veh_zone->gas.n2,      Va, Vb, cond, dt);
    exchange_gas_component(map_zone->gas.co2,     veh_zone->gas.co2,     Va, Vb, cond, dt);
    exchange_gas_component(map_zone->gas.plasma,  veh_zone->gas.plasma,  Va, Vb, cond, dt);
    exchange_gas_component(map_zone->gas.n2o,     veh_zone->gas.n2o,     Va, Vb, cond, dt);
    exchange_gas_component(map_zone->gas.bz,      veh_zone->gas.bz,      Va, Vb, cond, dt);
    exchange_gas_component(map_zone->gas.tritium, veh_zone->gas.tritium, Va, Vb, cond, dt);

    // Temperature mixing
    float post_Pa = map_zone->gas.total_pressure();
    float moles_change = (pre_Pa - post_Pa) * Va;
    if (moles_change > 0.f)
        mix_temp(veh_zone->gas, moles_change, map_zone->gas.temperature,
                 static_cast<float>(veh_zone->cell_count));
    else if (moles_change < 0.f)
        mix_temp(map_zone->gas, -moles_change, veh_zone->gas.temperature,
                 static_cast<float>(map_zone->cell_count));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Tick
// ─────────────────────────────────────────────────────────────────────────────

void VehicleManager::tick(double dt)
{
    // Tick every vehicle's internal simulation
    for (auto& v : m_vehicles)
        v->tick(dt);

    // Process cross-grid atmos exchange for all docked vehicles
    for (auto& lnk : m_dock_links)
        exchange_atmos(lnk, dt);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Private helpers
// ─────────────────────────────────────────────────────────────────────────────

DockLink* VehicleManager::find_dock_link(VehicleID id)
{
    for (auto& lnk : m_dock_links)
        if (lnk.vehicle_id == id) return &lnk;
    return nullptr;
}

const DockLink* VehicleManager::find_dock_link(VehicleID id) const
{
    for (const auto& lnk : m_dock_links)
        if (lnk.vehicle_id == id) return &lnk;
    return nullptr;
}
