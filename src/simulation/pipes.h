#pragma once
#include "core/world.h"
#include <unordered_map>
#include <vector>
#include <string>

using PipeNodeID = uint32_t;
constexpr PipeNodeID PIPE_NODE_NULL = 0;

enum class PipeNodeType : uint8_t { Pipe, Pump, Valve, Scrubber, Injector };

struct Reagent {
    std::string id;     // e.g. "water", "plasma", "toxin_xyz"
    float       volume; // units (mL)
};

struct FluidPacket {
    std::vector<Reagent> reagents;
    float temperature = 293.15f;
};

struct PipeNode {
    PipeNodeID   id        = PIPE_NODE_NULL;
    PipeNodeType type      = PipeNodeType::Pipe;
    glm::ivec3   world_pos{};
    FluidPacket  contents{};
    float        flow_rate = 1.f;  // units/tick
    bool         open      = true; // valves
    std::vector<PipeNodeID> neighbours;
};

class PipeNetwork {
public:
    explicit PipeNetwork(World& world);

    void rebuild();
    void tick(double dt);

    void on_pipe_changed(glm::ivec3 pos);

    PipeNode*  node(PipeNodeID id);
    PipeNodeID node_at(glm::ivec3 pos) const;

    PipeNodeID add_node(PipeNode n);
    void       remove_node(PipeNodeID id);

private:
    void flow_step(PipeNode& src, PipeNode& dst, double dt);
    void react(FluidPacket& packet);

    World& m_world;
    std::unordered_map<PipeNodeID, PipeNode> m_nodes;
    std::unordered_map<glm::ivec3, PipeNodeID> m_pos_map;
    PipeNodeID m_next_id = 1;
};
