#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  VehicleManager — owns all VehicleGrid instances and handles:
//
//    • Vehicle lifecycle (create / destroy)
//    • Docking & undocking with the main map
//    • Cross-grid atmospheric gas exchange through an open hatch
//    • Entity transfer between a vehicle's EntityManager and the map's
//
//  Docking model
//  ─────────────
//  Both the vehicle and the map define a DockPort — a single tile position
//  plus a face direction.  When dock() is called the vehicle's anchor is set
//  so the two ports are face-to-face.  While Docked and hatch_open == true,
//  VehicleManager::tick() equalises gas between:
//
//      vehicle.atmos().zone_at( vehicle_port.local_pos )
//      map_atmos.zone_at      ( map_port_pos )
//
//  using the same conductance constant that AtmosSimulator uses for an open
//  door.  The hatch can be opened/closed independently of the dock state.
//
//  Entity crossing
//  ───────────────
//  An entity physically walking into the dock port voxel can be handed off
//  via transfer_entity_to_vehicle() / transfer_entity_to_map() which swaps
//  the entity between EntityManagers and adjusts its TransformComponent so
//  it appears at the matching position in the new coordinate space.
//
//  Coordinate spaces
//  ─────────────────
//  Vehicles have their own local coordinate space.  The anchor converts:
//      map_pos   = vehicle_local + anchor
//      local_pos = map_pos      − anchor
//
//  While undocked the anchor has no meaning for rendering; the vehicle is
//  treated as a separate scene and not overlaid onto the map.
// ─────────────────────────────────────────────────────────────────────────────

#include "simulation/vehicle.h"
#include "simulation/physics.h"
#include <vector>
#include <memory>
#include <optional>

// ── DockLink — live record of a docked connection ────────────────────────────
struct DockLink {
    VehicleID  vehicle_id;

    // Port on the vehicle side (vehicle-local coords)
    DockPort   vehicle_port;

    // Port on the map side (map-world coords)
    glm::ivec3 map_port_pos{};
    FaceDir    map_port_face = FaceDir::PosZ;

    // Whether the hatch (airlock) is currently open for gas exchange.
    // Entities can still cross while the hatch is closed (airlock walk-through).
    bool hatch_open = false;
};

// ─────────────────────────────────────────────────────────────────────────────
//  VehicleManager
// ─────────────────────────────────────────────────────────────────────────────
class VehicleManager {
public:
    // map_world and map_atmos must outlive this VehicleManager.
    VehicleManager(World& map_world, AtmosSimulator& map_atmos,
                   EntityManager& map_entities, PhysicsSystem& map_physics);

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    // Create a new empty vehicle grid.  Returns a non-owning pointer; the
    // VehicleManager retains ownership.
    VehicleGrid* create_vehicle(const std::string& name);

    // Destroy a vehicle (undocks first if necessary).
    void destroy_vehicle(VehicleID id);

    // Lookup by ID — returns nullptr if not found.
    VehicleGrid*       get_vehicle(VehicleID id);
    const VehicleGrid* get_vehicle(VehicleID id) const;

    // All vehicles (for iteration / rendering).
    const std::vector<std::unique_ptr<VehicleGrid>>& all_vehicles() const { return m_vehicles; }

    // ── Docking ───────────────────────────────────────────────────────────────
    // Dock a vehicle so that `vehicle_port` aligns with the map-side connection
    // point at `map_port_pos` / `map_port_face`.
    //
    // Sets the vehicle anchor automatically so the two ports are face-to-face:
    //   anchor = map_port_pos + face_normal(map_port_face)
    //          - vehicle_port.local_pos
    //
    // Transitions the vehicle through Docking → Docked (instantly in this
    // implementation; add a timer/animation in the Docking handler if desired).
    //
    // Returns false if:
    //   • vehicle_id is invalid
    //   • the vehicle is already docked
    //   • the vehicle has no dock port set
    bool dock(VehicleID vehicle_id,
              glm::ivec3 map_port_pos, FaceDir map_port_face);

    // Undock a vehicle.  Closes the hatch, removes the DockLink, transitions
    // the vehicle to Undocking → InSpace.
    void undock(VehicleID vehicle_id);

    // ── Hatch control ─────────────────────────────────────────────────────────
    // Open or close the atmospheric hatch for the docked vehicle.
    // No-op if the vehicle is not docked.
    void set_hatch_open(VehicleID vehicle_id, bool open);
    bool is_hatch_open(VehicleID vehicle_id) const;

    // ── Entity transfer ───────────────────────────────────────────────────────
    // Move an entity from the main map into a vehicle.
    //   • Removes the entity's components from map_entities / map_physics.
    //   • Adds matching components to the vehicle's EntityManager.
    //   • Adjusts TransformComponent: new_local = old_map_pos − vehicle.anchor()
    // The entity ID is preserved.
    void transfer_entity_to_vehicle(EntityID         id,
                                    VehicleID        vehicle_id);

    // Move an entity from a vehicle back onto the main map.
    //   • Adjusts TransformComponent: new_map_pos = old_local + vehicle.anchor()
    void transfer_entity_to_map(EntityID  id,
                                VehicleID vehicle_id);

    // ── Spatial lookup ────────────────────────────────────────────────────────
    // Returns the vehicle whose docked voxel footprint contains map_pos.
    // Returns nullptr if no docked vehicle occupies that position.
    VehicleGrid* vehicle_at_map_pos(glm::ivec3 map_pos);

    // ── Tick ──────────────────────────────────────────────────────────────────
    // Tick all vehicles and process cross-grid atmos exchange.
    void tick(double dt);

private:
    // Find the DockLink for a given vehicle (nullptr if not docked).
    DockLink*       find_dock_link(VehicleID id);
    const DockLink* find_dock_link(VehicleID id) const;

    // Cross-grid gas equalisation for one dock link.
    void exchange_atmos(DockLink& link, double dt);

    World&          m_map_world;
    AtmosSimulator& m_map_atmos;
    EntityManager&  m_map_entities;
    PhysicsSystem&  m_map_physics;

    std::vector<std::unique_ptr<VehicleGrid>> m_vehicles;
    std::vector<DockLink>                     m_dock_links;
    VehicleID                                 m_next_id = 1;

    // Conductance used when exchanging gas through an open hatch.
    // Matches AtmosSimulator::CONDUCTANCE_OPEN (0.6f).
    static constexpr float HATCH_CONDUCTANCE = 0.6f;
    // Conductance for a docked but closed hatch (nearly airtight like a closed door).
    static constexpr float HATCH_CONDUCTANCE_CLOSED = 0.003f;
};
