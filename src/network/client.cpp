#include "network/client.h"
#include <SDL3/SDL.h>

Client::Client()  = default;
Client::~Client() { disconnect(); }

bool Client::connect(const char* host, uint16_t port)
{
    m_world    = std::make_unique<World>();
    m_entities = std::make_unique<EntityManager>();
    SDL_Log("Client: connecting to %s:%u", host, port);
    // TODO: create UDP socket, send connect handshake, await spawn packet
    m_connected = true;
    return true;
}

bool Client::connect_local(Server& server)
{
    m_world        = std::make_unique<World>();
    m_entities     = std::make_unique<EntityManager>();
    m_local_server = &server;
    m_local_player = server.spawn_player("human");
    m_connected    = true;
    SDL_Log("Client: connected to local server, player entity %u", m_local_player);
    return true;
}

void Client::disconnect()
{
    if (!m_connected) return;
    if (m_local_server) {
        m_local_server->remove_player(m_local_player);
        m_local_server = nullptr;
    }
    m_local_player = NULL_ENTITY;
    m_connected    = false;
}

void Client::tick(double dt)
{
    if (!m_connected) return;
    process_incoming();
    (void)dt;
}

void Client::send_input(const struct InputSnapshot& /*snap*/)
{
    // TODO: serialise input snapshot and send to server (UDP)
}

void Client::interpolate(double /*alpha*/)
{
    // TODO: blend entity TransformComponents between prev_pos and pos using alpha
}

void Client::send_chat(const std::string& msg)
{
    m_chat_log.push_back("[You] " + msg);
    // TODO: send to server
}

void Client::process_incoming()
{
    if (m_local_server) {
        // Loopback: directly copy entity/chunk state from server world
        // In a real impl this would go through (de)serialisation
        return;
    }
    // TODO: receive UDP packets, dispatch handlers
}

void Client::on_chunk_data(const void* /*data*/, size_t /*len*/)
{
    // TODO: deserialise and insert chunk into m_world
}

void Client::on_entity_state(const void* /*data*/, size_t /*len*/)
{
    // TODO: update entity transform from server packet
}

void Client::on_chat_message(const void* /*data*/, size_t /*len*/)
{
    // TODO: append decoded string to m_chat_log
}
