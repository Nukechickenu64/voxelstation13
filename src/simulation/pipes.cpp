#include "simulation/pipes.h"
#include <SDL3/SDL.h>
#include <algorithm>

PipeNetwork::PipeNetwork(World& world)
    : m_world(world)
{}

void PipeNetwork::rebuild()
{
    // TODO: scan voxels for pipe-type voxels and build network graph.
    SDL_Log("PipeNetwork: rebuild() — TODO");
}

void PipeNetwork::tick(double dt)
{
    // Pumps drive flow from their input to their output neighbour
    for (auto& [id, nd] : m_nodes) {
        if (nd.type != PipeNodeType::Pump) continue;
        if (!nd.open) continue;
        if (nd.neighbours.empty()) continue;
        auto* dst = node(nd.neighbours.front());
        if (dst) flow_step(nd, *dst, dt);
    }
    // After flow, check junctions for reagent reactions
    for (auto& [id, nd] : m_nodes)
        if (!nd.contents.reagents.empty())
            react(nd.contents);
}

void PipeNetwork::on_pipe_changed(glm::ivec3 pos)
{
    (void)pos;
    rebuild();
}

PipeNode* PipeNetwork::node(PipeNodeID id)
{
    auto it = m_nodes.find(id);
    return it != m_nodes.end() ? &it->second : nullptr;
}

PipeNodeID PipeNetwork::node_at(glm::ivec3 pos) const
{
    auto it = m_pos_map.find(pos);
    return it != m_pos_map.end() ? it->second : PIPE_NODE_NULL;
}

PipeNodeID PipeNetwork::add_node(PipeNode n)
{
    PipeNodeID id = m_next_id++;
    n.id = id;
    m_pos_map[n.world_pos] = id;
    m_nodes[id] = std::move(n);
    return id;
}

void PipeNetwork::remove_node(PipeNodeID id)
{
    auto it = m_nodes.find(id);
    if (it == m_nodes.end()) return;
    m_pos_map.erase(it->second.world_pos);
    m_nodes.erase(it);
}

void PipeNetwork::flow_step(PipeNode& src, PipeNode& dst, double dt)
{
    float transfer = src.flow_rate * static_cast<float>(dt);
    for (auto& r : src.contents.reagents) {
        float moved = std::min(r.volume, transfer);
        r.volume -= moved;
        bool found = false;
        for (auto& dr : dst.contents.reagents) {
            if (dr.id == r.id) { dr.volume += moved; found = true; break; }
        }
        if (!found) dst.contents.reagents.push_back({r.id, moved});
        transfer -= moved;
        if (transfer <= 0) break;
    }
    // Remove exhausted reagents from src
    src.contents.reagents.erase(
        std::remove_if(src.contents.reagents.begin(), src.contents.reagents.end(),
                       [](const Reagent& r){ return r.volume <= 0.f; }),
        src.contents.reagents.end());
}

void PipeNetwork::react(FluidPacket& /*packet*/)
{
    // TODO: look up reagent combinations in chemistry reaction table (JSON)
    //       and apply results (heat, product reagents, effects).
}
