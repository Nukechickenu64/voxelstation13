// ─────────────────────────────────────────────────────────────────────────────
//  VehicleGrid — implementation
// ─────────────────────────────────────────────────────────────────────────────
#include "simulation/vehicle.h"

// ── Construction / destruction ────────────────────────────────────────────────

VehicleGrid::VehicleGrid(VehicleID id, std::string name)
    : m_id(id)
    , m_name(std::move(name))
{
    m_world    = std::make_unique<World>();
    m_entities = std::make_unique<EntityManager>();
    m_atmos    = std::make_unique<AtmosSimulator>(*m_world, m_entities.get());
    m_power    = std::make_unique<PowerGrid>(*m_world);
    m_pipes    = std::make_unique<PipeNetwork>(*m_world);
    m_physics  = std::make_unique<PhysicsSystem>(*m_world, *m_entities);
}

VehicleGrid::~VehicleGrid() = default;

// ── Voxel change notifications ────────────────────────────────────────────────

void VehicleGrid::on_voxel_changed(glm::ivec3 local_pos)
{
    m_atmos->on_voxel_changed(local_pos);
    m_power->on_wire_changed(local_pos);
    m_pipes->on_pipe_changed(local_pos);
}

void VehicleGrid::on_door_changed(glm::ivec3 local_pos)
{
    m_atmos->on_door_changed(local_pos);
}

void VehicleGrid::on_wire_changed(glm::ivec3 local_pos)
{
    m_power->on_wire_changed(local_pos);
}

void VehicleGrid::on_pipe_changed(glm::ivec3 local_pos)
{
    m_pipes->on_pipe_changed(local_pos);
}

// ── Tick ──────────────────────────────────────────────────────────────────────

void VehicleGrid::tick(double dt)
{
    // Physics (full frame rate)
    m_physics->tick(dt);

    // Power grid
    m_power->tick(dt);

    // Pipe network
    m_pipes->tick(dt);

    // Atmos at fixed 20 Hz (same cadence as Server::tick)
    m_atmos_acc += dt;
    while (m_atmos_acc >= ATMOS_TICK_DT) {
        m_atmos->tick(ATMOS_TICK_DT);
        m_atmos_acc -= ATMOS_TICK_DT;
    }
}
