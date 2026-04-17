#pragma once
#include "core/world.h"
#include "core/entity_manager.h"
#include "simulation/atmos.h"
#include "simulation/liquids.h"
#include "simulation/power.h"
#include "simulation/pipes.h"
#include "simulation/physics.h"
#include "simulation/model_objects.h"
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

class MobSpeciesRegistry;  // forward declaration

// ── Packet types (shared server/client) ──────────────────────────────────────
enum class PacketType : uint16_t {
    // Server → Client
    ChunkData,
    ChunkDelta,
    EntityState,
    EntitySpawn,   // new entity spawned in world (type info + position)
    AtmosDelta,
    ChatMessage,
    RoundState,
    SpawnInfo,     // tell the new client its entity ID + spawn pos
    // Server → Client (cont.)
    AppearanceState,  // relayed appearance + equipment for a remote player
    // Client → Server
    InputState,
    InteractFace,
    InventoryAction,
    ChatSend,
    Connect,          // initial handshake from client
    AppearanceUpdate, // client sends appearance/equipment whenever it changes
};

struct NetAddress {
    uint32_t ip   = 0;
    uint16_t port = 0;
};

// Per-player movement input submitted each frame.
// !! DO NOT ADD a jump field here. There is no jumping in this game unless it
// is explicitly requested in a task description.
struct PlayerInput {
    glm::vec3 wish_dir{};   // world-space wish direction (Y component used in zero-G)
    float yaw = 0.f;        // player body yaw angle (degrees)
    bool sprint    = false;
    bool grab_wall = false; // Ctrl held: anchor to nearby solid surface in zero-G
};

// Authoritative server: owns all simulation state, replicates to clients.
class Server {
public:
    Server();
    ~Server();

    bool start(uint16_t port);
    void stop();
    void tick(double dt);

    // Provide species stat data (call before spawn_player if available).
    void set_species_registry(MobSpeciesRegistry* reg) { m_species_reg = reg; }

    // Add a bot/local player
    EntityID spawn_player(const std::string& species = "human");
    void     remove_player(EntityID id);

    // Spawn an autonomous NPC mob at the given world position.
    // Adds NpcAiComponent so the entity wanders automatically.
    // species must exist in the MobSpeciesRegistry (set_species_registry).
    EntityID spawn_npc(const std::string& species, glm::vec3 pos, float yaw = 0.f,
                       const std::string& name = "");

    World&           world()   { return *m_world; }
    EntityManager&   entities(){ return *m_entities; }
    PhysicsSystem&   physics() { return *m_physics; }
    AtmosSimulator&  atmos()   { return *m_atmos; }
    LiquidSimulator& liquids() { return *m_liquids; }

    // Pass a ModelObjectManager to physics (mob collision) and atmos (gas flow).
    // Must be called before the first tick.  Pointer must outlive this Server.
    void set_model_objects(ModelObjectManager* mgr) {
        m_physics->set_model_objects(mgr);
        m_atmos->set_model_objects(mgr);
    }

    // Queue a movement input; applied to physics at the start of the next tick.
    void queue_player_input(EntityID id, const PlayerInput& input);

    // Convenience: apply a movement input to a player entity for one tick.
    // wish_dir is a world-space unit vector (Y component ignored for walking).
    void move_player(EntityID id, glm::vec3 wish_dir,
                     bool sprint, bool grab_wall, double dt);

private:
    void process_incoming();
    void apply_pending_inputs(double dt);
    void broadcast_entity_spawns();
    void broadcast_entity_states();
    void broadcast_dirty_chunks();
    void send_to(NetAddress addr, PacketType type,
                 const void* data, size_t len);
    void send_entity_spawn_to(const NetAddress& addr, EntityID eid);

    // Track which entities have been broadcast to clients (for delta spawns)
    std::vector<EntityID> m_spawned_since_last_tick;

    std::unique_ptr<World>           m_world;
    std::unique_ptr<EntityManager>   m_entities;
    std::unique_ptr<AtmosSimulator>  m_atmos;
    std::unique_ptr<LiquidSimulator> m_liquids;
    std::unique_ptr<PowerGrid>       m_power;
    std::unique_ptr<PipeNetwork>     m_pipes;
    std::unique_ptr<PhysicsSystem>   m_physics;

    MobSpeciesRegistry* m_species_reg = nullptr;

    std::unordered_map<EntityID, PlayerInput> m_pending_inputs;

    double m_atmos_acc = 0.0;  // accumulated dt for fixed-rate atmos ticks
    static constexpr double ATMOS_TICK_DT = 1.0 / 20.0;  // 20 Hz
    // Liquid accumulator is internal to LiquidSimulator (LIQUID_TICK_DT = 0.5 s)

    struct Peer {
        NetAddress addr;
        EntityID   player_entity = NULL_ENTITY;
        uint64_t   last_ack_tick = 0;
        // Per-player chunk subscription set
        std::vector<glm::ivec3> subscribed_chunks;
        // Visual appearance received in the Connect / AppearanceUpdate packets
        uint8_t     eye_r = 30, eye_g = 100, eye_b = 190;
        std::string hair_file = "hair_messy";
        uint8_t     hair_r = 89, hair_g = 60, hair_b = 30;
        bool        is_male = true;
        // Equipped slots: (slot_id, item_def_id) pairs (empty item_id = nothing)
        std::vector<std::pair<std::string,std::string>> equipped_slots;
    };
    std::vector<Peer> m_peers;

    uint16_t  m_port   = 0;
    bool      m_running = false;
    uintptr_t m_socket  = uintptr_t(-1); // UDP socket (-1 = not bound)

    // Send the full voxel data for one chunk to a specific peer.
    void send_chunk_to(const NetAddress& addr, glm::ivec3 cp, const Chunk& chunk);
};
