#pragma once
#include "core/world.h"
#include "core/entity_manager.h"
#include "network/input_snapshot.h"
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

// Forward declarations — avoids pulling heavy headers into every TU that
// includes client.h.
class Server;
class AtmosSimulator;
class VoxelRegistry;
class ItemRegistry;

// Client-side view of the game world.
// Applies server state deltas and performs client-side prediction for the
// local player's movement.
class Client {
public:
    Client();
    ~Client();

    // Connect to a remote server (blocking up to ~5 s for the handshake).
    bool connect(const char* host, uint16_t port);
    void disconnect();
    bool connected() const { return m_connected; }

    // Called each logic tick
    void tick(double dt);

    // Send local player input to server (no-op in loopback mode)
    void send_input(const struct InputSnapshot& snap);

    // Called each render frame — returns interpolated entity states
    void interpolate(double alpha);

    // Local player entity (may be NULL_ENTITY before spawn)
    EntityID local_player() const { return m_local_player; }

    // World/entity access — works transparently in both loopback and remote mode.
    // Loopback: delegates to the in-process Server's authoritative state.
    // Remote:   returns the client's locally replicated state.
    World&         world();
    EntityManager& entities();

    // Atmospherics access — always returns nullptr (simulation runs server-side).
    AtmosSimulator* atmos() { return nullptr; }

    // Provide the voxel registry so received chunk data has correct flags.
    // Must be called before the first chunk packet arrives.
    void set_registry(const VoxelRegistry* reg) { m_voxel_registry = reg; }

    // Provide the item registry so spawned items can resolve their ItemDef*.
    void set_item_registry(const ItemRegistry* reg) { m_item_registry = reg; }

    // Set the player's visual appearance so it is sent to the server on connect
    // and relayed to other clients via EntitySpawn packets.
    struct AppearanceInfo {
        uint8_t     eye_r = 30, eye_g = 100, eye_b = 190;
        std::string hair_file = "hair_messy";
        uint8_t     hair_r = 89, hair_g = 60, hair_b = 30;
        bool        is_male = true;
    };
    void set_appearance(const AppearanceInfo& info) { m_appearance = info; }

    // Set which equipment slots are filled (slot_id → item_def_id).
    // Called whenever the local player's visible equipment changes.
    void set_equipped_slots(const std::vector<std::pair<std::string,std::string>>& slots) {
        m_equipped_slots = slots;
    }

    // Send the current appearance + equipped slots to the server.
    void send_appearance_update();

    // Chat
    void      send_chat(const std::string& msg);
    const std::vector<std::string>& chat_log() const { return m_chat_log; }

    // Admin commands
    void send_admin_cmd(AdminCmdType cmd);

    // Notify server that the player interacted with a voxel face (e.g. door).
    void send_interact_face(glm::ivec3 pos);

private:
    void process_incoming();
    void on_chunk_data      (const void* data, size_t len);
    void on_entity_state    (const void* data, size_t len);
    void on_entity_spawn    (const void* data, size_t len);
    void on_chat_message    (const void* data, size_t len);
    void on_appearance_state(const void* data, size_t len);

    std::unique_ptr<World>         m_world;
    std::unique_ptr<EntityManager> m_entities;

    EntityID m_local_player = NULL_ENTITY;
    bool     m_connected    = false;

    // Rewind buffer for lag compensation (up to 150 ms of input snapshots)
    std::vector<std::string> m_chat_log;

    // Remote: UDP socket + server address
    uintptr_t m_socket     = uintptr_t(-1);
    uint32_t  m_server_ip  = 0;
    uint16_t  m_server_port = 0;

    // Voxel type registry for flag reconstruction on received chunk data
    const VoxelRegistry* m_voxel_registry = nullptr;

    // Item registry for resolving item definitions on spawned items
    const ItemRegistry* m_item_registry = nullptr;

    // Visual appearance transmitted to server on connect / AppearanceUpdate
    AppearanceInfo m_appearance;
    std::vector<std::pair<std::string,std::string>> m_equipped_slots;

    // Rate-limiting for appearance updates sent from the render loop.
    // Tracks SDL_GetTicks() timestamp of the last successful send.
    uint64_t m_last_appearance_ms = 0;
};
