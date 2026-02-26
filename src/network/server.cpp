#include "network/server.h"
#include "data/mob_species_registry.h"
#include "simulation/mob_system.h"
#include <SDL3/SDL.h>
#include <cmath>

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

    // ── Atmospheric mob damage ──────────────────────────────────────────────
    // For each player entity apply SS13-style oxygen deprivation and toxin
    // damage based on the gas mixture at their current position.
    //
    // Rates (per second):
    //   oxy damage  — 4.0/s in vacuum, 1.5/s when O2 < 16 kPa
    //   oxy healing — 0.5/s when O2 >= 16 kPa and total pressure > 50 kPa
    //   tox damage  — 1.5/s per 10 kPa of combined plasma+N2O+BZ
    m_entities->each<MobPlayerTag>([&](EntityID eid, MobPlayerTag&) {
        auto* hp = m_entities->get_component<HealthComponent>(eid);
        auto* tr = m_entities->get_component<TransformComponent>(eid);
        if (!hp || !tr || hp->dead) return;

        glm::ivec3 cell = {
            static_cast<int>(std::floor(tr->pos.x)),
            static_cast<int>(std::floor(tr->pos.y)),
            static_cast<int>(std::floor(tr->pos.z))
        };
        AtmosZoneID az   = m_atmos->zone_at(cell);
        const AtmosZone* zptr = m_atmos->zone(az);

        bool   vacuum    = !zptr || zptr->is_space || zptr->gas.total_pressure() < 1.f;
        float  o2_kpa    = zptr ? zptr->gas.o2   : 0.f;
        float  tox_kpa   = zptr ? (zptr->gas.plasma + zptr->gas.n2o + zptr->gas.bz) : 0.f;

        // Oxygen damage / healing
        if (vacuum) {
            hp->apply("oxy", static_cast<float>(4.0 * dt));
        } else if (o2_kpa < 16.f) {
            hp->apply("oxy", static_cast<float>(1.5 * dt));
        } else {
            hp->heal(static_cast<float>(0.5 * dt));   // breathing fine — recover oxy
        }

        // Toxin damage (each 10 kPa of combined toxin = 1.5 dmg/s)
        if (tox_kpa > 0.5f) {
            hp->apply("tox", static_cast<float>(1.5 * (tox_kpa / 10.f) * dt));
        }
    });

    broadcast_entity_states();
    broadcast_dirty_chunks();
}

EntityID Server::spawn_player(const std::string& species)
{
    EntityID id = m_entities->create();
    m_entities->add_component<TransformComponent>(id);
    m_entities->add_component<VelocityComponent>(id);

    // Apply species stats to the character controller if registry is available.
    CharacterControllerComponent cc{};
    HealthComponent              hp{};
    if (m_species_reg) {
        const MobSpeciesDef* def = m_species_reg->get(species);
        if (def) {
            cc.move_speed  = def->move_speed;
            cc.sprint_mult = def->sprint_mult;
            cc.jump_vel    = def->jump_vel;
            cc.height      = std::min(def->height - 0.1f, 0.9f); // stay < 1.0 for 1-tile passages
            cc.radius      = def->radius * 0.75f;                 // tighten for 1-voxel gaps
            hp.health_max  = def->health_max;
            SDL_Log("Server: spawn_player — applied species '%s' stats (hp=%.0f spd=%.1f)",
                    species.c_str(), hp.health_max, cc.move_speed);
        } else {
            SDL_Log("Server: spawn_player — species '%s' not found, using defaults", species.c_str());
        }
    }
    m_entities->add_component<CharacterControllerComponent>(id, cc);
    m_entities->add_component<HealthComponent>(id, hp);

    // Tag this entity as a player mob so the damage tick can find it.
    MobPlayerTag tag;
    tag.species = species;
    m_entities->add_component<MobPlayerTag>(id, tag);

    // Assemble the player's visible sprite from bodypart overlay layers.
    // Uses bodyparts_greyscale so that skin tone tinting (tint multiply) works
    // correctly.  Prefix "human" + gender "_m"/"_f" matches the greyscale
    // filenames: e.g. human_chest_m_s.png, human_l_arm_s.png.
    HumanAppearance app{};
    HumanOverlay base_layer{};
    base_layer.sprite_dir = "bodyparts_greyscale";
    base_layer.prefix     = "human";
    base_layer.gender     = "_m";   // default to masculine; change per character
    base_layer.tint       = {255, 200, 160, 255};  // default light skin tone
    app.layers.push_back(base_layer);
    app.dirty = true;
    m_entities->add_component<HumanAppearance>(id, app);

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
        m_physics->prepare_character_movement(id, inp.wish_dir, inp.sprint, inp.grab_wall);
    }
    (void)dt;
}

void Server::move_player(EntityID id, glm::vec3 wish_dir,
                          bool sprint, bool grab_wall, double dt)
{
    if (!m_running || !m_physics) return;
    m_physics->move_character(id, wish_dir, sprint, grab_wall, dt);
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
