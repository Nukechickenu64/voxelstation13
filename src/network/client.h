#pragma once
#include "core/world.h"
#include "core/entity_manager.h"
#include "network/server.h"
#include "network/input_snapshot.h"
#include <string>
#include <vector>
#include <optional>

// Client-side view of the game world.
// Applies server state deltas and performs client-side prediction for the
// local player's movement.
class Client {
public:
    Client();
    ~Client();

    // Connect to a remote server, or pass nullptr to connect to a local Server
    bool connect(const char* host, uint16_t port);
    bool connect_local(Server& server);
    void disconnect();
    bool connected() const { return m_connected; }

    // Called each logic tick
    void tick(double dt);

    // Send local player input to server
    void send_input(const struct InputSnapshot& snap);

    // Called each render frame — returns interpolated entity states
    void interpolate(double alpha);

    // Local player entity (may be NULL_ENTITY before spawn)
    EntityID local_player() const { return m_local_player; }

    // Client-side world (received from server)
    World&         world()    { return *m_world; }
    EntityManager& entities() { return *m_entities; }

    // Chat
    void      send_chat(const std::string& msg);
    const std::vector<std::string>& chat_log() const { return m_chat_log; }

private:
    void process_incoming();
    void on_chunk_data  (const void* data, size_t len);
    void on_entity_state(const void* data, size_t len);
    void on_chat_message(const void* data, size_t len);

    std::unique_ptr<World>         m_world;
    std::unique_ptr<EntityManager> m_entities;

    EntityID m_local_player = NULL_ENTITY;
    bool     m_connected    = false;

    // Rewind buffer for lag compensation (up to 150 ms of input snapshots)
    static constexpr int REWIND_BUFFER = 16;
    std::vector<std::string> m_chat_log;

    Server* m_local_server = nullptr; // set when using loopback
};
