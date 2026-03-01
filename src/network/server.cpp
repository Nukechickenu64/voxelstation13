#include "network/server.h"
#include "simulation/liquids.h"
#include "data/mob_species_registry.h"
#include "simulation/mob_system.h"
#include "simulation/status_effects.h"
#include "simulation/attack_chain.h"
#include "simulation/reagents.h"
#include "simulation/npc_ai.h"
#include "core/signals.h"
#include "core/master_controller.h"
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

Server::Server()  = default;
Server::~Server() { stop(); }

bool Server::start(uint16_t port)
{
    m_world    = std::make_unique<World>();
    m_entities = std::make_unique<EntityManager>();
    m_atmos    = std::make_unique<AtmosSimulator>(*m_world, m_entities.get());
    m_liquids  = std::make_unique<LiquidSimulator>(*m_world, m_entities.get());
    m_power    = std::make_unique<PowerGrid>(*m_world);
    m_pipes    = std::make_unique<PipeNetwork>(*m_world);
    m_physics  = std::make_unique<PhysicsSystem>(*m_world, *m_entities);
    m_port     = port;
    m_running  = true;

    // ── Bump callback (density / TG Bump() proc) ──────────────────────────
    // When physics detects that movement is blocked by a dense entity,
    // dispatch to bump_attack() which fires COMSIG_ATOM_BUMPED.
    m_physics->set_bump_callback([this](EntityID mover, EntityID blocker) {
        bump_attack(mover, blocker, *m_entities, signals());
    });

    SDL_Log("Server: started on port %u (loopback only; UDP socket not yet bound)", port);
    // UDP socket bind: create SOCK_DGRAM and bind to 0.0.0.0:port.
    // Deferred until a networking backend (ENet / yojimbo / raw sockets) is chosen.
    return true;
}

void Server::stop()
{
    m_running = false;
    m_peers.clear();
    // Close UDP socket when a networking backend is integrated.
    SDL_Log("Server: stopped");
}

void Server::tick(double dt)
{
    if (!m_running) return;
    process_incoming();
    apply_pending_inputs(dt);

    // ── NPC AI tick ───────────────────────────────────────────────────────────
    // Update wander/idle state for all NpcAiComponent entities and write
    // cc->wish_move before the status-effect pass constrains it.
    tick_npc_ai(*m_entities, *m_world, dt);

    // ── Pre-physics status-effect movement constraints ─────────────────────────
    // Must run AFTER apply_pending_inputs (which fills cc->wish_move) and
    // BEFORE m_physics->tick() so the physics step sees the constrained wish_move.
    //
    // Step 1: Reset mob_state to Normal at the start of every frame so that
    //         effects which expired last tick don't keep their stale state.
    m_entities->each<CharacterControllerComponent>([](EntityID, CharacterControllerComponent& cc_r) {
        cc_r.mob_state = MobState::Normal;
    });

    // Step 2: Apply status-effect constraints.
    //   Stun / Paralysis  → zero wish_move, set Hardcrit
    //   Knockdown         → scale wish_move by 0.3 (crawl), set Softcrit
    //   Slowdown          → scale wish_move by speed_multiplier()
    m_entities->each<StatusEffectsComponent>([&](EntityID eid, StatusEffectsComponent& se) {
        auto* cc = m_entities->get_component<CharacterControllerComponent>(eid);
        if (!cc) return;
        MobState s = se.current_mob_state();
        if (s == MobState::Hardcrit) {
            cc->wish_move = glm::vec3(0.f);
            cc->mob_state = MobState::Hardcrit;
        } else if (s == MobState::Softcrit) {
            cc->mob_state = MobState::Softcrit;
            cc->wish_move *= 0.3f;   // crawl speed
        } else {
            float spd = se.speed_multiplier();
            if (spd < 0.999f)
                cc->wish_move *= spd;
        }
    });

    // Step 3: Dead mobs are fully incapacitated regardless of status effects.
    //         This catches player entities killed by atmos/damage whose wish_move
    //         was just filled in by apply_pending_inputs().
    m_entities->each<HealthComponent>([&](EntityID eid, HealthComponent& hp) {
        if (!hp.dead) return;
        auto* cc = m_entities->get_component<CharacterControllerComponent>(eid);
        if (!cc) return;
        cc->mob_state = MobState::Hardcrit;
        cc->wish_move = glm::vec3(0.f);
    });

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
    m_liquids->tick(dt);

    // ── Atmospheric mob damage ──────────────────────────────────────────────
    // For each player entity apply SS13-style oxygen deprivation, toxin, and
    // barotrauma damage based on the gas mixture at their current position.
    //
    // Rates (per second):
    //   oxy damage   — 4.0/s in vacuum, 1.5/s when O2 < 16 kPa
    //   oxy healing  — 0.5/s when O2 >= 16 kPa and total pressure > 50 kPa
    //   tox damage   — 1.5/s per 10 kPa of combined plasma+N2O+BZ
    //   barotrauma   — brute 0.5–3.0/s + burn 0.25–1.5/s when > 550 kPa
    //   decompression— brute 0–0.25/s when total pressure 1–10 kPa
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
        float  total_kpa = zptr ? zptr->gas.total_pressure() : 0.f;
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

        // ── Barotrauma / decompression damage ─────────────────────────────────
        // High pressure crush: > 550 kPa squeezes flesh and ruptures vessels.
        //   burn + brute at 0.5/s (body heat and mechanical trauma).
        // Low pressure (not full vacuum): 1–10 kPa causes decompression injury.
        //   brute 0.25/s — nitrogen bubble formation, skin rupture.
        //   Also starts at 10 kPa and ramps up toward vacuum severity.
        // Full vacuum is handled by the oxy path above; no double-counting.
        if (!vacuum && total_kpa > 550.f) {
            float excess = (total_kpa - 550.f) / 100.f;  // severity 0..N
            float rate = std::min(0.5f + excess * 0.3f, 3.0f);
            hp->apply("brute", static_cast<float>(rate * dt));
            hp->apply("burn",  static_cast<float>(rate * 0.5f * dt));
        } else if (!vacuum && total_kpa < 10.f) {
            float scale = (10.f - total_kpa) / 9.f;     // 0 at 10 kPa, 1 at 1 kPa
            hp->apply("brute", static_cast<float>(0.25f * scale * dt));
        }
    });

    // ── Status effects tick ───────────────────────────────────────────────────
    // Drain status effect durations; update CharacterController mob state.
    // NOTE: speed-multiplier / wish_move scaling is done BEFORE physics above.
    m_entities->each<StatusEffectsComponent>([&](EntityID eid, StatusEffectsComponent& se) {
        MobState status_state = se.tick(dt);
        auto* cc = m_entities->get_component<CharacterControllerComponent>(eid);
        if (cc) {
            // Only override toward harsher states — health-based crit is handled
            // in the game loop and should not be silently downgraded here.
            if (static_cast<int>(status_state) > static_cast<int>(cc->mob_state))
                cc->mob_state = status_state;
        }
    });

    // ── Grab choke: Neck/Kill → oxy damage on grabbed entity ─────────────────
    // Mirrors TG's /mob/living/proc/handle_grab_strangulation.
    // Neck state: 3 oxy/s.  Kill state: 6 oxy/s.  Fires death signal on KO.
    static constexpr float NECK_OXY_RATE = 3.f;
    static constexpr float KILL_OXY_RATE = 6.f;
    m_entities->each<GrabbedComponent>([&](EntityID eid, GrabbedComponent& gc) {
        if (gc.state < GrabState::Neck) return;
        auto* hp_g = m_entities->get_component<HealthComponent>(eid);
        if (!hp_g || hp_g->dead) return;
        float rate = (gc.state == GrabState::Kill) ? KILL_OXY_RATE : NECK_OXY_RATE;
        hp_g->apply("oxy", static_cast<float>(rate * dt));
        if (hp_g->dead) {
            signals().send_signal(eid, COMSIG_MOB_LIVING_DEATH, SigDeath{ gc.grabber });
            if (!m_entities->get_component<CorpseComponent>(eid)) {
                CorpseComponent corpse{};
                corpse.cause_of_death = "strangulation";
                m_entities->add_component<CorpseComponent>(eid, corpse);
            }
        }
    });

    // ── Corpse decay tick ─────────────────────────────────────────────────────
    // Advance time_since_death; mark revival impossible after decay_time.
    m_entities->each<CorpseComponent>([&](EntityID /*eid*/, CorpseComponent& corpse) {
        corpse.time_since_death += static_cast<float>(dt);
        if (corpse.can_be_revived && corpse.time_since_death >= corpse.decay_time)
            corpse.can_be_revived = false;
    });

    // Ensure every dead mob without a CorpseComponent gets one
    // (handles deaths from atmos, debug kill, etc.).
    m_entities->each<HealthComponent>([&](EntityID eid, HealthComponent& hp_c) {
        if (hp_c.dead && !m_entities->get_component<CorpseComponent>(eid)) {
            CorpseComponent corpse{};
            corpse.cause_of_death = "environmental hazards";
            m_entities->add_component<CorpseComponent>(eid, corpse);
        }
    });

    // ── Drag (body pulling) tick ──────────────────────────────────────────────
    // Move dragged entities to follow 1.2 m directly behind the dragger.
    // Detaches if the dragger entity no longer exists.
    {
        std::vector<EntityID> detach_list;
        m_entities->each<DragComponent>([&](EntityID eid, DragComponent& drag) {
            auto* dragger_tr = m_entities->get_component<TransformComponent>(drag.dragger);
            auto* eid_tr     = m_entities->get_component<TransformComponent>(eid);
            if (!dragger_tr || !eid_tr) {
                detach_list.push_back(eid);
                return;
            }
            // Position corpse 1.2 m directly behind dragger
            float ry = glm::radians(dragger_tr->yaw);
            glm::vec3 behind = dragger_tr->pos
                + glm::vec3(-std::sin(ry), 0.f, std::cos(ry)) * 1.2f;
            eid_tr->pos.x = behind.x;
            eid_tr->pos.z = behind.z;
            eid_tr->pos.y = dragger_tr->pos.y;  // same floor level
        });
        for (EntityID id : detach_list)
            m_entities->remove_component<DragComponent>(id);
    }

    // ── Reagent metabolisation ────────────────────────────────────────────────
    // TG runs this at SSreagents rate (1/5 s); we run each server tick.
    m_entities->each<ReagentContainerComponent>([&](EntityID eid, ReagentContainerComponent&) {
        auto* hp = m_entities->get_component<HealthComponent>(eid);
        if (hp && !hp->dead)
            metabolise_reagents(eid, *m_entities, signals(), dt);
    });

    // ── Master Controller tick ────────────────────────────────────────────────
    // Fires process() for all registered entities in SS13 subsystem order.
    master_controller().tick(dt);

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

    // ── TG SS13-style entity components ──────────────────────────────────────
    // DensityComponent — mob is dense by default (blocks movement / triggers bump)
    m_entities->add_component<DensityComponent>(id, DensityComponent{ true });

    // StatusEffectsComponent — receives stun, knockdown, etc.
    m_entities->add_component<StatusEffectsComponent>(id);

    // ReagentContainerComponent — mob's bloodstream / internal reagent container
    // Default capacity 1000u mirrors TG's /mob/living/max_blood=1000
    ReagentContainerComponent rcc;
    rcc.container = ReagentContainer(1000.f);
    m_entities->add_component<ReagentContainerComponent>(id, std::move(rcc));

    // MobTypeTag — quick istype("/mob/living/carbon/human") checks
    m_entities->add_component<MobTypeTag>(id, MobTypeTag{ "/mob/living/carbon/human" });

    // NameComponent — display name shown in examine / context menu
    m_entities->add_component<NameComponent>(id,
        NameComponent{ "Human", "A living, breathing crewmember." });

    // ── Register to Master Controller ─────────────────────────────────────────
    // "living" list: health updates, oxy damage, status effects
    master_controller().start_processing(MasterController::SS_LIVING, id,
        [this](EntityID eid, double /*dt*/) {
            // Health-based death signal (fires once on transition)
            auto* hp2 = m_entities->get_component<HealthComponent>(eid);
            if (hp2 && hp2->dead && !hp2->godmode) {
                // Only log once; the game loop handles mob_state transitions
                signals().send_signal(eid, COMSIG_MOB_LIVING_DEATH, SigDeath{});
            }
        });

    // "mobs" list: AI / input processing (player input handled by PhysicsSystem
    //  directly, but other mobs would go here)
    master_controller().start_processing(MasterController::SS_MOBS, id,
        [](EntityID /*eid*/, double /*dt*/) {
            // Stub: NPC AI would run here
        });

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
    // Deregister from Master Controller and signal bus before destroying entity
    master_controller().purge(id);
    signals().purge_entity(id);
    m_entities->destroy(id);
}

EntityID Server::spawn_npc(const std::string& species, glm::vec3 pos, float yaw,
                            const std::string& name)
{
    EntityID id = m_entities->create();

    // Transform
    TransformComponent tr{};
    tr.pos = pos;
    tr.yaw = yaw;
    m_entities->add_component<TransformComponent>(id, tr);
    m_entities->add_component<VelocityComponent>(id);

    // Species stats
    CharacterControllerComponent cc{};
    HealthComponent hp{};
    if (m_species_reg) {
        const MobSpeciesDef* def = m_species_reg->get(species);
        if (def) {
            cc.move_speed  = def->move_speed;
            cc.sprint_mult = def->sprint_mult;
            cc.jump_vel    = def->jump_vel;
            cc.height      = std::min(def->height - 0.1f, 0.9f);
            cc.radius      = def->radius * 0.75f;
            hp.health_max  = def->health_max;
        }
    }
    m_entities->add_component<CharacterControllerComponent>(id, cc);
    m_entities->add_component<HealthComponent>(id, hp);

    // Standard mob components
    m_entities->add_component<DensityComponent>(id, DensityComponent{ true });
    m_entities->add_component<StatusEffectsComponent>(id);
    m_entities->add_component<MobTypeTag>(id, MobTypeTag{ "/mob/living/carbon/" + species });

    // Display name
    std::string display = name.empty() ? species : name;
    if (!display.empty()) display[0] = static_cast<char>(::toupper(display[0]));
    m_entities->add_component<NameComponent>(id,
        NameComponent{ display, "A " + species + "." });

    // Sprite component
    MobComponent mob{};
    mob.species = species;
    mob.variant = "default";
    m_entities->add_component<MobComponent>(id, mob);

    // Human-look appearance for humanoid species
    if (species == "human") {
        HumanAppearance app{};
        HumanOverlay base{};
        base.sprite_dir = "bodyparts_greyscale";
        base.prefix     = "human";
        base.gender     = "_m";
        base.tint       = { 220, 185, 150, 255 };
        app.layers.push_back(base);
        app.dirty = true;
        m_entities->add_component<HumanAppearance>(id, app);
    }

    // NPC AI — stagger initial idle so mobs don't all move at once.
    // Tune behaviour per species:
    //   mouse  — tiny wander radius, flees from player approach (flee_dist 5 m)
    //   drone  — stationary janitor; does not wander
    //   human  — default wander, no flee/aggro
    NpcAiComponent ai{};
    ai.spawn_pos  = pos;
    ai.idle_timer = 1.f + (static_cast<float>(std::rand() % 30) / 10.f); // 1–4 s

    if (species == "mouse") {
        ai.wander_radius  = 2.5f;       // stays close to spawn
        ai.flee_dist      = 5.0f;       // flee from player within 5 m
        ai.flee_speed_mult= 2.2f;       // sprint away at 2.2× move speed
    } else if (species == "drone") {
        ai.wanders        = false;      // maintenance drones hold position
        ai.wander_radius  = 0.f;
    } else {
        ai.wander_radius  = 5.f;        // default for humanoids
    }
    m_entities->add_component<NpcAiComponent>(id, ai);

    SDL_Log("Server: spawned NPC '%s' (species: %s) at (%.1f,%.1f,%.1f)",
            display.c_str(), species.c_str(), pos.x, pos.y, pos.z);
    return id;
}

void Server::process_incoming()
{
    // When a UDP backend is available, receive packets here and dispatch:
    //   while (recv_packet(&addr, &pkt_type, data_buf, &data_len)) {
    //       switch (pkt_type) {
    //       case PacketType::InputState:   apply player input to pending_inputs;  break;
    //       case PacketType::InteractFace: call voxel on_hit handler;            break;
    //       case PacketType::ChatSend:     broadcast chat message to peers;      break;
    //       }
    //   }
}

void Server::broadcast_entity_states()
{
    if (m_peers.empty()) return;
    // Serialise each entity's transform as a compact array:
    //   [EntityID(4) x(4) y(4) z(4) yaw(4)] per entity  →  20 bytes each.
    struct EntityStateEntry {
        uint32_t id;
        float    x, y, z, yaw;
    };
    std::vector<EntityStateEntry> entries;
    m_entities->each<TransformComponent>([&](EntityID eid, TransformComponent& tr) {
        entries.push_back({static_cast<uint32_t>(eid),
                           tr.pos.x, tr.pos.y, tr.pos.z, tr.yaw});
    });
    if (entries.empty()) return;
    send_to({}, PacketType::EntityState,
            entries.data(), entries.size() * sizeof(EntityStateEntry));
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
        // Serialise chunk header + flat voxel type_id array.
        //   Header:  [cx(4)  cy(4)  cz(4)]              = 12 bytes
        //   Payload: [CHUNK_VOL × uint16_t type_ids]    = 8192 bytes
        struct ChunkHdr { int32_t cx, cy, cz; };
        const glm::ivec3 cp = chunk->chunk_pos();
        ChunkHdr hdr{ cp.x, cp.y, cp.z };
        std::vector<uint16_t> vox_data;
        vox_data.reserve(CHUNK_VOL);
        for (int z = 0; z < CHUNK_SIZE; ++z)
            for (int y = 0; y < CHUNK_SIZE; ++y)
                for (int x = 0; x < CHUNK_SIZE; ++x)
                    vox_data.push_back(chunk->get(x, y, z).type_id);
        // Transmit to subscribed peers (loopback peers are skipped by send_to).
        send_to({}, PacketType::ChunkData, &hdr, sizeof(hdr));
        // In a real impl: send header + vox_data in a single packet.
        (void)vox_data;
        chunk->clear_dirty();
    }
}

void Server::send_to(NetAddress /*addr*/, PacketType /*type*/,
                     const void* /*data*/, size_t /*len*/)
{
    // Transmit packet over UDP when a networking backend is integrated.
    // Packet wire format: [PacketType(2) | payload_len(4) | payload(len)]
    // For loopback sessions this path is not taken — state is shared by pointer.
}
