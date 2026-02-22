#include "simulation/power.h"
#include <SDL3/SDL.h>
#include <queue>

PowerGrid::PowerGrid(World& world)
    : m_world(world)
{}

void PowerGrid::rebuild()
{
    // TODO: scan all voxels for wire/generator/APC types from voxel registry,
    //       build m_nodes and m_pos_map, then connect neighbours.
    SDL_Log("PowerGrid: rebuild() — TODO: scan voxels for power nodes");
}

void PowerGrid::tick(double dt)
{
    (void)dt;
    propagate_power();
}

void PowerGrid::on_wire_changed(glm::ivec3 pos)
{
    // Minimal: just trigger a full rebuild for now.
    // An optimised implementation would do incremental graph repair.
    (void)pos;
    rebuild();
}

PowerNode* PowerGrid::node(PowerNodeID id)
{
    auto it = m_nodes.find(id);
    return it != m_nodes.end() ? &it->second : nullptr;
}

PowerNodeID PowerGrid::node_at(glm::ivec3 pos) const
{
    auto it = m_pos_map.find(pos);
    return it != m_pos_map.end() ? it->second : POWER_NODE_NULL;
}

bool PowerGrid::powered_at(glm::ivec3 pos) const
{
    PowerNodeID id = node_at(pos);
    if (id == POWER_NODE_NULL) return false;
    auto it = m_nodes.find(id);
    return it != m_nodes.end() && it->second.powered;
}

PowerNodeID PowerGrid::add_node(PowerNode n)
{
    PowerNodeID id = m_next_id++;
    n.id = id;
    m_pos_map[n.world_pos] = id;
    m_nodes[id] = std::move(n);
    return id;
}

void PowerGrid::remove_node(PowerNodeID id)
{
    auto it = m_nodes.find(id);
    if (it == m_nodes.end()) return;
    m_pos_map.erase(it->second.world_pos);
    m_nodes.erase(it);
}

void PowerGrid::propagate_power()
{
    // BFS from generators; mark nodes as powered if reachable
    for (auto& [id, nd] : m_nodes)
        nd.powered = false;

    std::queue<PowerNodeID> q;
    for (auto& [id, nd] : m_nodes)
        if (nd.type == PowerNodeType::Generator && nd.supply_w > 0)
            q.push(id);

    while (!q.empty()) {
        PowerNodeID cur = q.front(); q.pop();
        auto* nd = node(cur);
        if (!nd || nd->powered) continue;
        nd->powered = true;
        for (PowerNodeID nb : nd->neighbours)
            q.push(nb);
    }
}

void PowerGrid::balance_load(PowerNodeID /*apc_id*/)
{
    // TODO: calculate total demand within APC area vs supply; shed load if needed.
}
