#pragma once
#include "core/world.h"
#include <unordered_map>
#include <vector>

using PowerNodeID = uint32_t;
constexpr PowerNodeID POWER_NODE_NULL = 0;

enum class PowerNodeType : uint8_t {
    Wire,
    Generator,
    APC,          // Area Power Controller
    Machine,
    Light,
    Airlock,
};

struct PowerNode {
    PowerNodeID   id        = POWER_NODE_NULL;
    PowerNodeType type      = PowerNodeType::Wire;
    glm::ivec3    world_pos{};
    float         voltage   = 0.f;  // V
    float         current   = 0.f;  // A
    float         demand_w  = 0.f;  // Watts consumed per tick
    float         supply_w  = 0.f;  // Watts supplied (generators only)
    bool          powered   = false;
    std::vector<PowerNodeID> neighbours;
};

// Simulates the wire/power graph similar to SS13's APC system.
class PowerGrid {
public:
    explicit PowerGrid(World& world);

    void rebuild();
    void tick(double dt);

    // Voxel wire changed at pos
    void on_wire_changed(glm::ivec3 pos);

    PowerNode*       node(PowerNodeID id);
    PowerNodeID      node_at(glm::ivec3 pos) const;
    bool             powered_at(glm::ivec3 pos) const;

    PowerNodeID add_node(PowerNode n);
    void        remove_node(PowerNodeID id);

private:
    void propagate_power();
    void balance_load(PowerNodeID apc_id);

    World& m_world;
    std::unordered_map<PowerNodeID, PowerNode> m_nodes;
    std::unordered_map<glm::ivec3, PowerNodeID> m_pos_map;
    PowerNodeID m_next_id = 1;
};
