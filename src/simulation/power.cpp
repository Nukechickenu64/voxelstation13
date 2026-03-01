#include "simulation/power.h"
#include <SDL3/SDL.h>
#include <queue>
#include <unordered_set>

PowerGrid::PowerGrid(World& world)
    : m_world(world)
{}

void PowerGrid::rebuild()
{
    // Re-compute neighbour lists for all registered nodes based on world adjacency.
    // Nodes are registered via add_node() when wires/generators/APCs are placed.
    constexpr glm::ivec3 k_dirs[6] = {
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };
    for (auto& [id, nd] : m_nodes) {
        nd.neighbours.clear();
        for (const auto& d : k_dirs) {
            PowerNodeID nb = node_at(nd.world_pos + d);
            if (nb != POWER_NODE_NULL && nb != id)
                nd.neighbours.push_back(nb);
        }
    }
    SDL_Log("PowerGrid: rebuild() — %u nodes, connections updated",
            (unsigned)m_nodes.size());
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

void PowerGrid::balance_load(PowerNodeID apc_id)
{
    auto* apc = node(apc_id);
    if (!apc || apc->type != PowerNodeType::APC) return;

    // BFS from the APC to collect all reachable nodes in this area.
    std::queue<PowerNodeID>           q;
    std::unordered_set<PowerNodeID>   visited;
    q.push(apc_id);
    visited.insert(apc_id);

    float total_supply = apc->supply_w;
    float total_demand = 0.f;

    while (!q.empty()) {
        PowerNodeID cur = q.front(); q.pop();
        auto* nd = node(cur);
        if (!nd) continue;
        if (nd->type == PowerNodeType::Generator)
            total_supply += nd->supply_w;
        else if (nd->type != PowerNodeType::Wire && nd->type != PowerNodeType::APC)
            total_demand += nd->demand_w;
        for (PowerNodeID nb : nd->neighbours)
            if (!visited.count(nb)) { visited.insert(nb); q.push(nb); }
    }

    bool overloaded = total_supply < total_demand;
    // On overload, shed non-essential consumer nodes (Machines, Lights, Airlocks)
    // while keeping Wire and APC nodes powered so control circuits remain active.
    for (PowerNodeID nid : visited) {
        auto* nd = node(nid);
        if (!nd) continue;
        if (overloaded) {
            nd->powered = (nd->type == PowerNodeType::Wire ||
                           nd->type == PowerNodeType::APC);
        } else {
            nd->powered = true;
        }
    }

    if (overloaded)
        SDL_Log("PowerGrid: APC %u OVERLOADED — supply=%.0fW demand=%.0fW",
                apc_id, total_supply, total_demand);
}
