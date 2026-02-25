#include "network/server.h"
#include <SDL3/SDL.h>

Server::Server()  = default;
Server::~Server() { stop(); }

bool Server::start(uint16_t port)
{
    m_world    = std::make_unique<World>();
    m_entities = std::make_unique<EntityManager>();
    m_atmos    = std::make_unique<AtmosSimulator>(*m_world, m_entities.get());
    m_power    = std::make_unique<PowerGrid>(*m_world);
    m_pipes    = std::make_unique<PipeNetwork>(*m_world);
    m_physics  = std::make_unique<PhysicsSystem>(*m_world, *m_entities);
    m_port     = port;
    m_running  = true;
    SDL_Log("Server: started on port %u", port);
    // TODO: bind UDP socket
    return true;
}

void Server::stop()
{
    m_running = false;
    m_peers.clear();
    // TODO: close socket
}

void Server::tick(double dt)
{
    if (!m_running) return;
    process_incoming();
    apply_pending_inputs(dt);
    m_physics->tick(dt);
    // Atmos runs at a fixed 20 Hz to keep simulation stable regardless of
    // the main loop frame rate.  We accumulate elapsed time and drain it in
    // whole ticks so the gas math always sees the same dt.
    m_atmos_acc += dt;
    while (m_atmos_acc >= ATMOS_TICK_DT) {
        m_atmos->tick(ATMOS_TICK_DT);
        m_atmos_acc -= ATMOS_TICK_DT;
    }
    m_power->tick(dt);
    m_pipes->tick(dt);
    broadcast_entity_states();
    broadcast_dirty_chunks();
}

EntityID Server::spawn_player(const std::string& species)
{
    EntityID id = m_entities->create();
    m_entities->add_component<TransformComponent>(id);
    m_entities->add_component<VelocityComponent>(id);
    m_entities->add_component<CharacterControllerComponent>(id);

    // TODO: load species slot definitions from data, create Inventory component
    SDL_Log("Server: spawned player entity %u (species: %s)", id, species.c_str());
    return id;
}

void Server::remove_player(EntityID id)
{
    m_entities->destroy(id);
}

void Server::process_incoming()
{
    // TODO: receive UDP packets, dispatch by PacketType
}

void Server::broadcast_entity_states()
{
    // TODO: serialise TransformComponent for all entities, send to subscribed peers
}

void Server::queue_player_input(EntityID id, const PlayerInput& input)
{
    m_pending_inputs[id] = input;
}

void Server::apply_pending_inputs(double dt)
{
    for (auto& [id, inp] : m_pending_inputs) {
        m_physics->prepare_character_movement(id, inp.wish_dir,
                                               inp.crouch, inp.sprint);
    }
    (void)dt;
}

void Server::move_player(EntityID id, glm::vec3 wish_dir,
                          bool crouch, bool sprint, double dt)
{
    if (!m_running || !m_physics) return;
    m_physics->move_character(id, wish_dir, crouch, sprint, dt);
}

void Server::broadcast_dirty_chunks()
{
    auto dirty = m_world->dirty_chunks();
    for (Chunk* chunk : dirty) {
        // TODO: serialise and send chunk data to subscribed peers
        chunk->clear_dirty();
        (void)chunk;
    }
}

void Server::send_to(NetAddress /*addr*/, PacketType /*type*/,
                     const void* /*data*/, size_t /*len*/)
{
    // TODO: send over UDP socket
}
