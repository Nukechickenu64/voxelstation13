#pragma once
#include <glm/glm.hpp>

// Shared packet/enumeration types used by both client and server.
// Kept small so headers can be included by lightweight headers like client.h.
#include <cstdint>

// Packet types (shared server/client)
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
    AdminCmd,         // client → server: dev/admin command
};

// Admin command types (payload byte 0 in AdminCmd packets)
enum class AdminCmdType : uint8_t {
    ToggleNoclip       = 0,
    ToggleGodmode      = 1,
    ActionFullHeal     = 2,
    ActionKillPlayer   = 3,
    ActionTeleportOrigin = 4,
    ActionForceAtmos   = 5,
    ActionSpawnItems   = 6,
    ToggleAutoHeal     = 7,
    ToggleZeroGOverride= 8,
    ToggleInfiniteOxy  = 9,
};

struct NetAddress {
    uint32_t ip   = 0;
    uint16_t port = 0;
};

// Snapshot of one frame's input, sent from client to server each tick.
struct InputSnapshot {
    glm::vec3 wish_dir{};   // movement direction (world-space XZ)
    float     yaw   = 0.f;
    float     pitch = 0.f;
    bool      jump  = false;
    bool      sprint= false;
    bool      primary_interact  = false;
    bool      secondary_interact= false;
    uint64_t  tick_id = 0;      // client-side tick counter for rewind
};
