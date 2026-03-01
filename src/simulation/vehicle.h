#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  VehicleGrid — a self-contained simulation zone that mirrors the main map's
//  full system stack:
//    World   (voxel grid)           AtmosSimulator  (gas simulation)
//    PowerGrid                      PipeNetwork
//    PhysicsSystem                  EntityManager
//
//  A vehicle has its own local coordinate space.  When docked it holds an
//  anchor offset so that:
//
//      map_pos = vehicle_local_pos + anchor
//
//  Entities can cross between the vehicle and the main map through a DockPort.
//  Atmospheric gas is exchanged when the hatch is open using the same
//  equalise_zones pathway as normal door-links, but bridging two separate
//  AtmosSimulator instances.
// ─────────────────────────────────────────────────────────────────────────────

#include "core/world.h"
#include "core/entity_manager.h"
#include "simulation/atmos.h"
#include "simulation/power.h"
#include "simulation/pipes.h"
#include "simulation/physics.h"
#include "simulation/model_objects.h"
#include <cstdint>
#include <string>
#include <memory>

// ── Forward declarations ──────────────────────────────────────────────────────
class MobSpeciesRegistry;

// ── VehicleID ─────────────────────────────────────────────────────────────────
using VehicleID = uint32_t;
constexpr VehicleID VEHICLE_ID_NULL = 0;

// ── DockPort — one end of a docking connection ────────────────────────────────
//  Defined in the local coordinate system of the owning grid (vehicle or map).
struct DockPort {
    glm::ivec3 local_pos{};  // position of the hatch tile in local coords
    FaceDir    face = FaceDir::PosZ;  // which face the hatch opens onto
};

// ── VehicleState ──────────────────────────────────────────────────────────────
enum class VehicleState : uint8_t {
    InSpace,    // free-floating, not linked to the main map
    Docking,    // latching in progress (transition)
    Docked,     // fully docked — atmos/entity transfers allowed
    Undocking,  // separation in progress (transition)
};

// ─────────────────────────────────────────────────────────────────────────────
//  VehicleGrid
// ─────────────────────────────────────────────────────────────────────────────
class VehicleGrid {
public:
    explicit VehicleGrid(VehicleID id, std::string name);
    ~VehicleGrid();

    // ── Identity ──────────────────────────────────────────────────────────────
    VehicleID          id()   const { return m_id; }
    const std::string& name() const { return m_name; }

    // ── State ─────────────────────────────────────────────────────────────────
    VehicleState state()                 const { return m_state; }
    void         set_state(VehicleState s)     { m_state = s; }
    bool         is_docked()             const { return m_state == VehicleState::Docked; }

    // ── World anchor (vehicle[0,0,0] in map world coords) ─────────────────────
    glm::ivec3 anchor()                  const { return m_anchor; }
    void       set_anchor(glm::ivec3 a)        { m_anchor = a; }

    // Coordinate space conversion
    glm::ivec3 to_map_pos  (glm::ivec3 local_pos) const { return local_pos + m_anchor; }
    glm::ivec3 to_local_pos(glm::ivec3 map_pos)   const { return map_pos   - m_anchor; }

    // ── Dock port ─────────────────────────────────────────────────────────────
    bool            has_dock_port()               const { return m_has_dock_port; }
    const DockPort& dock_port()                   const { return m_dock_port; }
    void            set_dock_port(DockPort port)        { m_dock_port = port; m_has_dock_port = true; }
    void            clear_dock_port()                   { m_has_dock_port = false; }

    // ── Subsystem accessors (mirrors Server's public interface) ───────────────
    World&          world()    { return *m_world; }
    const World&    world()    const { return *m_world; }
    EntityManager&  entities() { return *m_entities; }
    AtmosSimulator& atmos()    { return *m_atmos; }
    PowerGrid&      power()    { return *m_power; }
    PipeNetwork&    pipes()    { return *m_pipes; }
    PhysicsSystem&  physics()  { return *m_physics; }

    // Pass a ModelObjectManager through to collision / atmos systems.
    // Pointer must outlive this VehicleGrid.
    void set_model_objects(ModelObjectManager* mgr) {
        m_physics->set_model_objects(mgr);
        m_atmos->set_model_objects(mgr);
    }

    // ── Simulation tick ───────────────────────────────────────────────────────
    //  Runs entity MC processing lists, physics, power, pipes, and atmos at
    //  their canonical rates (atmos at 20 Hz, everything else at caller's dt).
    void tick(double dt);

    // ── Voxel change notifications (forwarded from World edits) ───────────────
    void on_voxel_changed(glm::ivec3 local_pos);
    void on_door_changed (glm::ivec3 local_pos);
    void on_wire_changed (glm::ivec3 local_pos);
    void on_pipe_changed (glm::ivec3 local_pos);

private:
    VehicleID    m_id;
    std::string  m_name;
    VehicleState m_state      = VehicleState::InSpace;
    glm::ivec3   m_anchor{};
    bool         m_has_dock_port = false;
    DockPort     m_dock_port{};

    std::unique_ptr<World>          m_world;
    std::unique_ptr<EntityManager>  m_entities;
    std::unique_ptr<AtmosSimulator> m_atmos;
    std::unique_ptr<PowerGrid>      m_power;
    std::unique_ptr<PipeNetwork>    m_pipes;
    std::unique_ptr<PhysicsSystem>  m_physics;

    // Atmos runs at a fixed 20 Hz independent of the main tick rate.
    double m_atmos_acc = 0.0;
    static constexpr double ATMOS_TICK_DT = 1.0 / 20.0;
};
