#include "simulation/pipes.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <SDL3/SDL.h>
#include <algorithm>

PipeNetwork::PipeNetwork(World& world)
    : m_world(world)
{}

void PipeNetwork::rebuild()
{
    // Re-compute neighbour lists for all registered nodes based on world adjacency.
    // Nodes are registered via add_node() when pipe voxels are placed in the world.
    constexpr glm::ivec3 k_dirs[6] = {
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };
    for (auto& [id, nd] : m_nodes) {
        nd.neighbours.clear();
        for (const auto& d : k_dirs) {
            PipeNodeID nb = node_at(nd.world_pos + d);
            if (nb != PIPE_NODE_NULL && nb != id)
                nd.neighbours.push_back(nb);
        }
    }
    SDL_Log("PipeNetwork: rebuild() — %u nodes, connections updated",
            (unsigned)m_nodes.size());
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

void PipeNetwork::react(FluidPacket& packet)
{
    // Load reaction definitions from data/reactions/chemistry.json once.
    struct ReactionDef {
        struct Req  { std::string id; float min_vol; };
        struct Prod { std::string id; float vol_per_tick; };
        std::vector<Req>  reagents;
        std::vector<Prod> products;
        float min_temperature = 293.f;
        bool  exothermic      = false;
        float heat_per_tick   = 0.f;
    };
    static std::vector<ReactionDef> s_reactions;
    static bool s_loaded = false;
    if (!s_loaded) {
        s_loaded = true;
        try {
            std::ifstream f("data/reactions/chemistry.json");
            if (f.is_open()) {
                nlohmann::json j; f >> j;
                for (const auto& r : j) {
                    ReactionDef rd;
                    for (const auto& req : r.at("reagents"))
                        rd.reagents.push_back({req.at("id"), req.value("min_volume", 1.f)});
                    for (const auto& prod : r.at("products"))
                        rd.products.push_back({prod.at("id"), prod.value("volume_per_tick", 1.f)});
                    rd.min_temperature = r.value("min_temperature", 293.f);
                    rd.exothermic      = r.value("exothermic", false);
                    rd.heat_per_tick   = r.value("heat_per_tick", 0.f);
                    s_reactions.push_back(std::move(rd));
                }
                SDL_Log("PipeNetwork: loaded %u chemistry reactions",
                        (unsigned)s_reactions.size());
            }
        } catch (const std::exception& e) {
            SDL_Log("PipeNetwork: failed to load chemistry.json: %s", e.what());
        }
    }

    for (const auto& rxn : s_reactions) {
        if (packet.temperature < rxn.min_temperature) continue;
        // Check all required reagents are present in sufficient quantity.
        bool can_react = true;
        for (const auto& req : rxn.reagents) {
            bool found = false;
            for (const auto& r : packet.reagents)
                if (r.id == req.id && r.volume >= req.min_vol) { found = true; break; }
            if (!found) { can_react = false; break; }
        }
        if (!can_react) continue;
        // Produce reagents.
        for (const auto& prod : rxn.products) {
            bool found = false;
            for (auto& r : packet.reagents)
                if (r.id == prod.id) { r.volume += prod.vol_per_tick; found = true; break; }
            if (!found) packet.reagents.push_back({prod.id, prod.vol_per_tick});
        }
        // Exothermic reactions raise the packet temperature.
        if (rxn.exothermic)
            packet.temperature += rxn.heat_per_tick;
    }
}
