#pragma once
#include "core/world.h"
#include "core/entity_manager.h"
#include "simulation/atmos.h"
#include "simulation/power.h"
#include "simulation/pipes.h"
#include "simulation/physics.h"
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

// ── Packet types (shared server/client) ──────────────────────────────────────
enum class PacketType : uint16_t {
    // Server → Client
    ChunkData,
    ChunkDelta,
    EntityState,
    AtmosDelta,
    ChatMessage,
    RoundState,
    // Client → Server
    InputState,
    InteractFace,
    InventoryAction,
    ChatSend,
};

struct NetAddress {
    uint32_t ip   = 0;
    uint16_t port = 0;
};

// Per-player movement input submitted each frame.
struct PlayerInput {
    glm::vec3 wish_dir{};   // world-space wish direction (Y ignored for walking)
    bool jump   = false;
    bool crouch = false;
    bool sprint = false;
};

// Authoritative server: owns all simulation state, replicates to clients.
class Server {
public:
    Server();
    ~Server();

    bool start(uint16_t port);
    void stop();
    void tick(double dt);

    // Add a bot/local player
    EntityID spawn_player(const std::string& species = "human");
    void     remove_player(EntityID id);

    World&          world()    { return *m_world; }
    EntityManager&  entities() { return *m_entities; }
    PhysicsSystem&  physics()  { return *m_physics; }

    // Queue a movement input; applied to physics at the start of the next tick.
    void queue_player_input(EntityID id, const PlayerInput& input);

    // Convenience: apply a movement input to a player entity for one tick.
    // wish_dir is a world-space unit vector (Y component ignored for walking).
    void move_player(EntityID id, glm::vec3 wish_dir,
                     bool jump, bool crouch, bool sprint, double dt);

private:
    void process_incoming();
    void apply_pending_inputs(double dt);
    void broadcast_entity_states();
    void broadcast_dirty_chunks();
    void send_to(NetAddress addr, PacketType type,
                 const void* data, size_t len);

    std::unique_ptr<World>          m_world;
    std::unique_ptr<EntityManager>  m_entities;
    std::unique_ptr<AtmosSimulator> m_atmos;
    std::unique_ptr<PowerGrid>      m_power;
    std::unique_ptr<PipeNetwork>    m_pipes;
    std::unique_ptr<PhysicsSystem>  m_physics;

    std::unordered_map<EntityID, PlayerInput> m_pending_inputs;

    struct Peer {
        NetAddress addr;
        EntityID   player_entity = NULL_ENTITY;
        uint64_t   last_ack_tick = 0;
        // Per-player chunk subscription set
        std::vector<glm::ivec3> subscribed_chunks;
    };
    std::vector<Peer> m_peers;

    uint16_t m_port = 0;
    bool     m_running = false;
};
