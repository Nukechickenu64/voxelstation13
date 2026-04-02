// ─────────────────────────────────────────────────────────────────────────────
//  VehicleGrid — implementation
// ─────────────────────────────────────────────────────────────────────────────
#include "simulation/vehicle.h"
#include <cmath>

// ── Construction / destruction ────────────────────────────────────────────────

VehicleGrid::VehicleGrid(VehicleID id, std::string name)
    : m_id(id)
    , m_name(std::move(name))
{
    m_world       = std::make_unique<World>();
    m_entities    = std::make_unique<EntityManager>();
    m_atmos       = std::make_unique<AtmosSimulator>(*m_world, m_entities.get());
    m_power       = std::make_unique<PowerGrid>(*m_world);
    m_pipes       = std::make_unique<PipeNetwork>(*m_world);
    m_physics     = std::make_unique<PhysicsSystem>(*m_world, *m_entities);
    m_lighting    = std::make_unique<LightingSystem>(*m_world);
    m_world_items = std::make_unique<WorldItemSystem>(*m_world, *m_entities);
    m_liquids     = std::make_unique<LiquidSimulator>(*m_world, m_entities.get());
}

VehicleGrid::~VehicleGrid() = default;

// ── Voxel change notifications ────────────────────────────────────────────────

void VehicleGrid::on_voxel_changed(glm::ivec3 local_pos)
{
    m_atmos->on_voxel_changed(local_pos);
    m_power->on_wire_changed(local_pos);
    m_pipes->on_pipe_changed(local_pos);
    m_lighting->update({local_pos});
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

    // World items: settle floating items
    m_world_items->tick(dt);

    // Liquid simulation
    m_liquids->tick(dt);

    // Atmos at fixed 20 Hz (same cadence as Server::tick)
    m_atmos_acc += dt;
    while (m_atmos_acc >= ATMOS_TICK_DT) {
        m_atmos->tick(ATMOS_TICK_DT);
        m_atmos_acc -= ATMOS_TICK_DT;
    }
}

// ── World-position physics (physics-gun rigid body) ───────────────────────────

void VehicleGrid::tick_world_pos(double dt)
{
    const float fdt = static_cast<float>(dt);

    // Linear friction: exponential decay — heavy ship slows gradually
    constexpr float FRICTION_K = 1.2f;
    const float fric = std::exp(-FRICTION_K * fdt);
    m_world_vel.x *= fric;
    m_world_vel.y *= fric;
    m_world_vel.z *= fric;

    // Integrate velocity into position
    m_world_pos_f += m_world_vel * fdt;

    // Floor clamp: ship can't sink below y=0
    if (m_world_pos_f.y < 0.f) {
        m_world_pos_f.y = 0.f;
        if (m_world_vel.y < 0.f) m_world_vel.y = 0.f;
    }

    // Sync integer anchor to floor of float position
    m_anchor = glm::ivec3(
        static_cast<int>(std::floor(m_world_pos_f.x)),
        static_cast<int>(std::floor(m_world_pos_f.y)),
        static_cast<int>(std::floor(m_world_pos_f.z)));
}
