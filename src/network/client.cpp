#include "network/client.h"
#include <SDL3/SDL.h>
#include <cstdint>

Client::Client()  = default;
Client::~Client() { disconnect(); }

bool Client::connect(const char* host, uint16_t port)
{
    m_world    = std::make_unique<World>();
    m_entities = std::make_unique<EntityManager>();
    SDL_Log("Client: connecting to %s:%u", host, port);
    // Remote connection: create a UDP socket, send a connect-handshake packet,
    // and await a spawn packet from the server.
    // Deferred until a UDP networking backend is integrated; use connect_local() now.
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

void Client::send_input(const struct InputSnapshot& snap)
{
    if (m_local_server) {
        // Loopback: input is injected directly in the game loop via
        // server.queue_player_input(); no serialisation needed.
        return;
    }
    // Remote: serialise and send InputSnapshot over UDP.
    // Packet: [PacketType::InputState(2) | sizeof(snap)(4) | snap bytes]
    (void)snap; // socket send deferred until UDP backend is integrated.
}

void Client::interpolate(double alpha)
{
    if (!m_connected) return;
    // Blend each entity's position between prev_pos and pos using alpha [0,1]
    // so rendering is smooth between physics ticks.
    m_entities->each<TransformComponent>([alpha](EntityID, TransformComponent& tr) {
        tr.pos = glm::mix(tr.prev_pos, tr.pos, static_cast<float>(alpha));
    });
}

void Client::send_chat(const std::string& msg)
{
    m_chat_log.push_back("[You] " + msg);
    if (m_local_server) return; // loopback: chat broadcast handled server-side
    // Remote: serialise and send a ChatSend packet over UDP.
    // Packet: [PacketType::ChatSend(2) | msg.size()(4) | msg bytes]
    // send_packet(PacketType::ChatSend, msg.data(), msg.size());
}

void Client::process_incoming()
{
    if (m_local_server) {
        // Loopback: state is shared by pointer — no serialisation needed.
        return;
    }
    // Remote: receive and dispatch UDP packets.
    // while (recv_packet(&pkt_type, data_buf, &data_len)) {
    //     switch (pkt_type) {
    //     case PacketType::ChunkData:   on_chunk_data  (data_buf, data_len); break;
    //     case PacketType::EntityState: on_entity_state(data_buf, data_len); break;
    //     case PacketType::ChatMessage: on_chat_message(data_buf, data_len); break;
    //     }
    // }
}

void Client::on_chunk_data(const void* data, size_t len)
{
    // Deserialise server ChunkData packet and update m_world.
    // Packet layout: [ChunkHdr(cx,cy,cz)(12 bytes)] + [CHUNK_VOL × uint16_t type_ids]
    struct ChunkHdr { int32_t cx, cy, cz; };
    constexpr size_t k_hdr_size = sizeof(ChunkHdr);
    constexpr size_t k_payload  = static_cast<size_t>(CHUNK_VOL) * sizeof(uint16_t);
    if (!data || len < k_hdr_size + k_payload) return;
    const auto* hdr  = reinterpret_cast<const ChunkHdr*>(data);
    const auto* voxs = reinterpret_cast<const uint16_t*>(
                           static_cast<const uint8_t*>(data) + k_hdr_size);
    const glm::ivec3 chunk_pos{hdr->cx, hdr->cy, hdr->cz};
    Chunk* chunk = m_world->get_or_create_chunk(chunk_pos);
    for (int z = 0; z < CHUNK_SIZE; ++z)
        for (int y = 0; y < CHUNK_SIZE; ++y)
            for (int x = 0; x < CHUNK_SIZE; ++x) {
                Voxel v{};
                v.type_id = voxs[x + CHUNK_SIZE * (y + CHUNK_SIZE * z)];
                chunk->set(x, y, z, v);
            }
}

void Client::on_entity_state(const void* data, size_t len)
{
    // Deserialise server EntityState packet and update entity transforms.
    // Packet layout: array of [EntityID(4) x(4) y(4) z(4) yaw(4)] = 20 bytes each.
    struct EntityStateEntry { uint32_t id; float x, y, z, yaw; };
    constexpr size_t k_entry = sizeof(EntityStateEntry);
    if (!data || len < k_entry) return;
    const size_t count   = len / k_entry;
    const auto*  entries = reinterpret_cast<const EntityStateEntry*>(data);
    for (size_t i = 0; i < count; ++i) {
        EntityID eid = static_cast<EntityID>(entries[i].id);
        auto* tr = m_entities->get_component<TransformComponent>(eid);
        if (!tr) continue; // entity not yet registered on this client
        tr->prev_pos = tr->pos;
        tr->pos  = {entries[i].x, entries[i].y, entries[i].z};
        tr->yaw  = entries[i].yaw;
    }
}

void Client::on_chat_message(const void* data, size_t len)
{
    // Packet is a raw UTF-8 string; append to chat log.
    if (!data || len == 0) return;
    m_chat_log.emplace_back(reinterpret_cast<const char*>(data), len);
}
