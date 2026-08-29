#include "network/server.h"
#include "simulation/liquids.h"
#include "simulation/world_items.h"
#include "data/mob_species_registry.h"
#include "data/voxel_registry.h"
#include "simulation/mob_system.h"
#include "simulation/status_effects.h"
#include "simulation/attack_chain.h"
#include "simulation/reagents.h"
#include "simulation/npc_ai.h"
#include "core/signals.h"
#include "core/master_controller.h"
#include <SDL3/SDL.h>
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>

// ── Platform UDP socket abstraction ──────────────────────────────────────
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using sock_t = SOCKET;
   static constexpr sock_t k_bad_sock = INVALID_SOCKET;
   static void  net_close(sock_t s)  { ::closesocket(s); }
   static bool  net_would_block()    { return WSAGetLastError()==WSAEWOULDBLOCK; }
   static bool  net_init()           { WSADATA w; return WSAStartup(MAKEWORD(2,2),&w)==0; }
   static void  net_quit()           { WSACleanup(); }
   static bool  net_set_nonblock(sock_t s) { u_long v=1; return ::ioctlsocket(s,FIONBIO,&v)==0; }
   static const char* inet_str(struct in_addr a) { return ::inet_ntoa(a); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <errno.h>
   using sock_t = int;
   static constexpr sock_t k_bad_sock = -1;
   static void  net_close(sock_t s)  { ::close(s); }
   static bool  net_would_block()    { return errno==EAGAIN||errno==EWOULDBLOCK; }
   static bool  net_init()           { return true; }
   static void  net_quit()           { }
   static bool  net_set_nonblock(sock_t s) {
       int f=::fcntl(s,F_GETFL,0); return ::fcntl(s,F_SETFL,f|O_NONBLOCK)!=-1; }
   static const char* inet_str(struct in_addr a) { return ::inet_ntoa(a); }
#endif
static_assert(sizeof(sock_t) <= sizeof(uintptr_t));
static inline sock_t   to_sock(uintptr_t u) { return static_cast<sock_t>(u); }
static inline uintptr_t to_uptr(sock_t  s) { return static_cast<uintptr_t>(s); }

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
    m_physics     = std::make_unique<PhysicsSystem>(*m_world, *m_entities);
    m_world_items = std::make_unique<WorldItemSystem>(*m_world, *m_entities, m_item_registry);
    m_port     = port;
    m_running  = true;

    // ── Bump callback (density / TG Bump() proc) ──────────────────────────
    m_physics->set_bump_callback([this](EntityID mover, EntityID blocker) {
        bump_attack(mover, blocker, *m_entities, signals());
    });

    // ── Bind UDP socket if port != 0 ──────────────────────────────────────
    if (port != 0) {
        if (!net_init()) {
            SDL_Log("Server: network init failed");
            return false;
        }
        sock_t fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd == k_bad_sock) {
            SDL_Log("Server: socket() failed");
            return false;
        }
        net_set_nonblock(fd);

        // Allow address re-use so server can restart quickly
        int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            SDL_Log("Server: bind() failed on port %u", port);
            net_close(fd);
            return false;
        }
        m_socket = to_uptr(fd);
        SDL_Log("Server: UDP socket bound to 0.0.0.0:%u", port);
    } else {
        SDL_Log("Server: loopback mode (no UDP socket)");
    }
    return true;
}

void Server::stop()
{
    m_running = false;
    m_peers.clear();
    if (m_socket != uintptr_t(-1)) {
        net_close(to_sock(m_socket));
        m_socket = uintptr_t(-1);
        net_quit();
    }
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
    // Step 1: Reset mob_state to base_mob_state at the start of every frame so
    //         that effects which expired last tick don't keep their stale state.
    //         base_mob_state carries the persistent voluntary state (e.g. Resting)
    //         and is written by game logic; mob_state is the per-frame effective value.
    m_entities->each<CharacterControllerComponent>([](EntityID, CharacterControllerComponent& cc_r) {
        cc_r.mob_state = cc_r.base_mob_state;
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

    // Step 4: Confusion drift — randomly deflects wish_move for confused entities.
    // Mirrors TG's confusion mechanic: each tick the desired move direction has a
    // random perpendicular component added, scaling with effect strength (0..1).
    // strength 1.0 = full 90° random deflection; 0.5 = 45° max drift.
    m_entities->each<StatusEffectsComponent>([&](EntityID eid, StatusEffectsComponent& se) {
        if (!se.is_confused()) return;
        auto* cc = m_entities->get_component<CharacterControllerComponent>(eid);
        if (!cc || cc->mob_state == MobState::Hardcrit) return;
        const auto* eff = se.get(StatusEffectType::Confusion);
        float strength = eff ? eff->strength : 1.f;
        // Random angle in [-90°, 90°] scaled by strength
        float angle = (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX) * 2.f - 1.f)
                      * (glm::half_pi<float>() * strength);
        float c = std::cos(angle), s2 = std::sin(angle);
        glm::vec3 w = cc->wish_move;
        cc->wish_move.x = w.x * c - w.z * s2;
        cc->wish_move.z = w.x * s2 + w.z * c;
    });

    // ── Zero-G: update for ALL character-controller entities ─────────────────
    // Must run BEFORE physics so gravity/friction are correct this tick.
    // main.cpp also updates the local player each frame (and respects the
    // zerog_override dev tool); this pass handles NPCs and remote players.
    m_entities->each<CharacterControllerComponent>([&](EntityID eid, CharacterControllerComponent& cc_e) {
        auto* tr_e = m_entities->get_component<TransformComponent>(eid);
        if (!tr_e) return;
        glm::ivec3 cell = {
            static_cast<int>(std::floor(tr_e->pos.x)),
            static_cast<int>(std::floor(tr_e->pos.y)),
            static_cast<int>(std::floor(tr_e->pos.z))
        };
        AtmosZoneID az        = m_atmos->zone_at(cell);
        const AtmosZone* zptr = m_atmos->zone(az);
        cc_e.zero_g = (!zptr || zptr->is_space);
    });

    m_physics->tick(dt);
    m_world_items->tick(dt);  // settle floating items after physics

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

    // ── Stamina regeneration ──────────────────────────────────────────────────
    // TG: stamRegenRate ≈ 3/s when stamina is not fully depleted.
    // Stamina KO that clears here removes Knockdown only if no other knockdown
    // source is active (status effects track their own durations independently).
    static constexpr float STAM_REGEN_RATE = 3.f;
    m_entities->each<HealthComponent>([&](EntityID eid, HealthComponent& hp_stam) {
        if (hp_stam.dead || hp_stam.stam_damage <= 0.f) return;
        bool was_ko = hp_stam.stam_ko;
        hp_stam.regen_stam(STAM_REGEN_RATE * static_cast<float>(dt));
        // If stamina KO just cleared, try removing the stam-induced Knockdown.
        // We leave legitimately-applied status-effect durations alone.
        if (was_ko && !hp_stam.stam_ko) {
            auto* se_stam = m_entities->get_component<StatusEffectsComponent>(eid);
            if (se_stam) se_stam->remove(StatusEffectType::Knockdown);
        }
    });

    // ── Status effects tick ───────────────────────────────────────────────────
    // NOTE: atmospheric damage is now handled by AtmosSimulator::apply_entity_effects()
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

    // ── Per-peer dev-tool effects ─────────────────────────────────────────────
    for (auto& peer : m_peers) {
        EntityID eid = peer.player_entity;
        auto* hp = m_entities->get_component<HealthComponent>(eid);
        auto* cc = m_entities->get_component<CharacterControllerComponent>(eid);
        if (peer.dev_auto_heal && hp) {
            hp->brute = hp->burn = hp->tox = hp->oxy = hp->clone = 0.f;
            hp->stam_damage = 0.f;
            hp->stam_ko = false;
            hp->dead    = false;
            hp->recalculate();
        } else if (peer.dev_infinite_oxy && hp) {
            hp->oxy = 0.f;
            hp->recalculate();
        }
        if (peer.dev_zerog_override && cc) {
            cc->zero_g = true;
        }
    }

    broadcast_entity_spawns();
    broadcast_entity_states();
    broadcast_dirty_chunks();
    m_spawned_since_last_tick.clear();
}

EntityID Server::spawn_player(const std::string& species, glm::vec3 pos)
{
    EntityID id = m_entities->create();
    // Spawn above the floor (y=0 is the floor tile; y=1 is standing height).
    TransformComponent spawn_tr{};
    spawn_tr.pos = pos;
    m_entities->add_component<TransformComponent>(id, spawn_tr);
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
    m_spawned_since_last_tick.push_back(id);
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
    m_spawned_since_last_tick.push_back(id);
    return id;
}

void Server::process_incoming()
{
    sock_t fd = to_sock(m_socket);
    if (fd == k_bad_sock) return; // loopback mode

    static uint8_t buf[65536];
    for (;;) {
        sockaddr_in from{};
#ifdef _WIN32
        int from_len = sizeof(from);
#else
        socklen_t from_len = sizeof(from);
#endif
        int n = ::recvfrom(fd, reinterpret_cast<char*>(buf), (int)sizeof(buf), 0,
                           reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n < 0) {
            if (net_would_block()) break;
            break;
        }
        if (n < 6) continue; // too small for a packet header

        NetAddress sender;
        sender.ip   = from.sin_addr.s_addr;
        sender.port = ntohs(from.sin_port);

        // Wire format: [type:uint16_t LE][len:uint32_t LE][payload:len bytes]
        uint16_t type_raw;
        uint32_t payload_len;
        std::memcpy(&type_raw,    buf,     2);
        std::memcpy(&payload_len, buf + 2, 4);
        auto type = static_cast<PacketType>(type_raw);
        if (n < 6 + (int)payload_len) continue; // truncated payload
        const uint8_t* payload = buf + 6;

        switch (type) {
        // ── New client connect handshake ───────────────────────────────────
        case PacketType::Connect: {
            bool already = false;
            for (auto& p : m_peers)
                if (p.addr.ip == sender.ip && p.addr.port == sender.port)
                    { already = true; break; }
            if (!already) {
                Peer peer;
                peer.addr          = sender;
                // Spawn each new player further along +Z so they face existing
                // players with the default camera yaw (facing -Z).
                // Player 0: Z=0, Player 1: Z=3 (sees Player 0 ahead), etc.
                float spawn_z = static_cast<float>(m_peers.size()) * 3.f;
                peer.player_entity = spawn_player("human", {0.f, 1.f, spawn_z});

                // Parse optional appearance payload:
                // [eye_r:1][eye_g:1][eye_b:1][hair_len:1][hair:N][hair_r:1][hair_g:1][hair_b:1]
                if (payload_len >= 3) {
                    peer.eye_r = payload[0];
                    peer.eye_g = payload[1];
                    peer.eye_b = payload[2];
                }
                if (payload_len >= 4) {
                    uint8_t hair_len = payload[3];
                    if (payload_len >= static_cast<uint32_t>(4 + hair_len + 3)) {
                        peer.hair_file = std::string(
                            reinterpret_cast<const char*>(payload + 4), hair_len);
                        peer.hair_r = payload[4 + hair_len + 0];
                        peer.hair_g = payload[4 + hair_len + 1];
                        peer.hair_b = payload[4 + hair_len + 2];
                    }
                }

                // Mark local loopback connections as admin by default.
                // This ensures server-side authorization for dev commands.
                // sender.ip is in network byte order; compare against htonl(INADDR_LOOPBACK).
                peer.is_admin = (sender.ip == htonl(INADDR_LOOPBACK));
                m_peers.push_back(peer);
                if (peer.is_admin) SDL_Log("Server: granted admin to client %s:%u",
                                           inet_str(from.sin_addr), sender.port);
                SDL_Log("Server: client %s:%u connected → entity %u",
                        inet_str(from.sin_addr), sender.port, peer.player_entity);

                // Tell the client its player entity ID and spawn position
                auto* tr = m_entities->get_component<TransformComponent>(peer.player_entity);
                struct { uint32_t entity_id; float x, y, z; } si{};
                si.entity_id = static_cast<uint32_t>(peer.player_entity);
                if (tr) { si.x = tr->pos.x; si.y = tr->pos.y; si.z = tr->pos.z; }
                send_to(sender, PacketType::SpawnInfo, &si, sizeof(si));
                SDL_Log("Server: sent SpawnInfo to client %s:%u (entity %u)",
                        inet_str(from.sin_addr), sender.port, peer.player_entity);

                // Send all currently loaded chunk data to the new client
                int chunk_count = 0;
                m_world->for_each_chunk([&](const Chunk& chunk) {
                    send_chunk_to(sender, chunk.chunk_pos(), chunk);
                    chunk_count++;
                });
                SDL_Log("Server: sent %d chunks to new client", chunk_count);

                // Send all existing entities (except the new player's own entity)
                // Do this AFTER SpawnInfo so the client knows its own entity first.
                int entity_count = 0;
                m_entities->each<TransformComponent>([&](EntityID eid, TransformComponent&) {
                    if (eid == peer.player_entity) return;  // skip self
                    // Only send if entity has a rendering component
                    if (m_entities->has_component<HumanAppearance>(eid) ||
                        m_entities->has_component<MobComponent>(eid) ||
                        m_entities->has_component<WorldItemComponent>(eid)) {
                        send_entity_spawn_to(sender, eid);
                        entity_count++;
                    }
                });
                SDL_Log("Server: sent %d entities to new client", entity_count);

                // Notify all already-connected peers about the new player so
                // they can see it without waiting for the end-of-tick broadcast.
                for (const auto& ep : m_peers) {
                    if (ep.player_entity == peer.player_entity) continue;
                    send_entity_spawn_to(ep.addr, peer.player_entity);
                    SDL_Log("Server: notified peer (entity %u) about new player (entity %u)",
                            ep.player_entity, peer.player_entity);
                }
            }
            break;
        }
        // ── Appearance + equipment update from client ──────────────────────
        case PacketType::AppearanceUpdate: {
            // Format: [eye_r:1][eye_g:1][eye_b:1][hair_len:1][hair:N][hair_r:1][hair_g:1][hair_b:1]
            //         [slot_count:1]([slot_id_len:1][slot_id:N][item_id_len:1][item_id:N])*
            if (payload_len < 4) break;
            const uint8_t* p   = static_cast<const uint8_t*>(payload);
            const uint8_t* end = p + payload_len;
            uint8_t eye_r = p[0], eye_g = p[1], eye_b = p[2];
            uint8_t hlen  = p[3];
            p += 4;
            if (p + hlen + 3 > end) break;
            std::string hair_file(reinterpret_cast<const char*>(p), hlen);
            p += hlen;
            uint8_t hair_r = p[0], hair_g = p[1], hair_b = p[2];
            p += 3;

            // Parse gender byte (optional)
            bool is_male = true;
            if (p < end) is_male = (*p++ != 0);

            // Parse slot pairs (optional, may not be present for older sends)
            std::vector<std::pair<std::string,std::string>> slots;
            if (p < end) {
                uint8_t slot_count = *p++;
                for (int i = 0; i < slot_count && p < end; ++i) {
                    if (p >= end) break;
                    uint8_t slen = *p++;
                    if (p + slen > end) break;
                    std::string slot_id(reinterpret_cast<const char*>(p), slen);
                    p += slen;
                    if (p >= end) break;
                    uint8_t ilen = *p++;
                    if (p + ilen > end) break;
                    std::string item_id(reinterpret_cast<const char*>(p), ilen);
                    p += ilen;
                    if (!slot_id.empty())
                        slots.emplace_back(std::move(slot_id), std::move(item_id));
                }
            }

            for (auto& peer : m_peers) {
                if (peer.addr.ip != sender.ip || peer.addr.port != sender.port)
                    continue;
                peer.eye_r         = eye_r;
                peer.eye_g         = eye_g;
                peer.eye_b         = eye_b;
                peer.hair_file     = hair_file;
                peer.hair_r        = hair_r;
                peer.hair_g        = hair_g;
                peer.hair_b        = hair_b;
                peer.is_male       = is_male;
                peer.equipped_slots = slots;

                // Build AppearanceState relay packet:
                // [entity_id:4] + same payload as AppearanceUpdate
                std::vector<uint8_t> relay;
                uint32_t eid_raw = static_cast<uint32_t>(peer.player_entity);
                relay.insert(relay.end(),
                    reinterpret_cast<const uint8_t*>(&eid_raw),
                    reinterpret_cast<const uint8_t*>(&eid_raw) + 4);
                // eye + hair (re-encode from parsed values to ensure clean relay)
                relay.push_back(eye_r); relay.push_back(eye_g); relay.push_back(eye_b);
                relay.push_back(hlen);
                relay.insert(relay.end(), hair_file.begin(), hair_file.end());
                relay.push_back(hair_r); relay.push_back(hair_g); relay.push_back(hair_b);
                relay.push_back(is_male ? 1u : 0u);
                // slots
                relay.push_back((uint8_t)std::min(slots.size(), (size_t)255));
                for (auto& [sid, iid] : slots) {
                    relay.push_back((uint8_t)sid.size());
                    relay.insert(relay.end(), sid.begin(), sid.end());
                    relay.push_back((uint8_t)iid.size());
                    relay.insert(relay.end(), iid.begin(), iid.end());
                }

                for (const auto& other : m_peers) {
                    if (other.addr.ip == peer.addr.ip &&
                        other.addr.port == peer.addr.port) continue;
                    send_to(other.addr, PacketType::AppearanceState,
                            relay.data(), relay.size());
                }
                break;
            }
            break;
        }
        // ── Voxel face interaction (e.g. open/close door) ─────────────────
        case PacketType::InteractFace: {
            struct WireInteract { int32_t x, y, z; };
            if (payload_len < sizeof(WireInteract)) break;
            WireInteract wi;
            std::memcpy(&wi, payload, sizeof(wi));
            toggle_door({wi.x, wi.y, wi.z});
            break;
        }
        // ── Player movement input ──────────────────────────────────────────
        case PacketType::InputState: {
            struct WireInput { float dx, dy, dz; float yaw; uint8_t sprint, grab; };
            if (payload_len < sizeof(WireInput)) break;
            WireInput wi;
            std::memcpy(&wi, payload, sizeof(wi));
            for (auto& p : m_peers) {
                if (p.addr.ip == sender.ip && p.addr.port == sender.port) {
                    PlayerInput pi;
                    pi.wish_dir  = glm::vec3(wi.dx, wi.dy, wi.dz);
                    pi.yaw       = wi.yaw;
                    pi.sprint    = wi.sprint != 0;
                    pi.grab_wall = wi.grab   != 0;
                    queue_player_input(p.player_entity, pi);
                    break;
                }
            }
            break;
        }
        // ── Admin / dev-tool command ───────────────────────────────────────
        case PacketType::AdminCmd: {
            if (payload_len < 1) break;
            auto cmd = static_cast<AdminCmdType>(payload[0]);
            for (auto& p : m_peers) {
                if (p.addr.ip != sender.ip || p.addr.port != sender.port) continue;
                if (!p.is_admin) {
                    struct in_addr a; a.s_addr = p.addr.ip;
                    SDL_Log("Server: denied admin cmd %u from non-admin %s:%u",
                            static_cast<uint32_t>(cmd), inet_str(a), p.addr.port);
                    break;
                }
                EntityID eid = p.player_entity;
                switch (cmd) {
                case AdminCmdType::ToggleNoclip: {
                    auto* cc = m_entities->get_component<CharacterControllerComponent>(eid);
                    if (cc) {
                        cc->noclip = !cc->noclip;
                        SDL_Log("Server: noclip %s for entity %u", cc->noclip ? "ON" : "OFF", eid);
                    }
                    break;
                }
                case AdminCmdType::ToggleGodmode: {
                    auto* hp = m_entities->get_component<HealthComponent>(eid);
                    if (hp) {
                        hp->godmode = !hp->godmode;
                        SDL_Log("Server: godmode %s for entity %u", hp->godmode ? "ON" : "OFF", eid);
                    }
                    break;
                }
                case AdminCmdType::ActionFullHeal: {
                    auto* hp = m_entities->get_component<HealthComponent>(eid);
                    if (hp) {
                        hp->brute = hp->burn = hp->tox = hp->oxy = hp->clone = 0.f;
                        hp->stam_damage = 0.f;
                        hp->stam_ko = false;
                        hp->dead = false;
                        hp->recalculate();
                        SDL_Log("Server: full heal for entity %u", eid);
                    }
                    break;
                }
                case AdminCmdType::ActionKillPlayer: {
                    auto* hp = m_entities->get_component<HealthComponent>(eid);
                    if (hp) {
                        bool was_god = hp->godmode;
                        hp->godmode = false;
                        hp->apply("brute", hp->health_max * 2.f);
                        hp->godmode = was_god;
                        SDL_Log("Server: kill player for entity %u", eid);
                    }
                    break;
                }
                case AdminCmdType::ActionTeleportOrigin: {
                    auto* tr = m_entities->get_component<TransformComponent>(eid);
                    if (tr) {
                        bool found = false;
                        for (int sy = 1; sy <= 128 && !found; ++sy) {
                            Voxel b = m_world->get_voxel({0, sy,     0});
                            Voxel t = m_world->get_voxel({0, sy + 1, 0});
                            if (!(b.flags & VFLAG_SOLID) && !(t.flags & VFLAG_SOLID)) {
                                tr->pos = {0.5f, static_cast<float>(sy), 0.5f};
                                auto* vel = m_entities->get_component<VelocityComponent>(eid);
                                if (vel) vel->linear = {};
                                found = true;
                            }
                        }
                        if (!found) {
                            tr->pos = {0.5f, 5.f, 0.5f};
                            auto* vel = m_entities->get_component<VelocityComponent>(eid);
                            if (vel) vel->linear = {};
                        }
                        SDL_Log("Server: teleport origin for entity %u → (%.1f,%.1f,%.1f)",
                                eid, tr->pos.x, tr->pos.y, tr->pos.z);
                    }
                    break;
                }
                case AdminCmdType::ActionForceAtmos: {
                    m_atmos->rebuild_zones();
                    SDL_Log("Server: atmos zones rebuilt");
                    break;
                }
                case AdminCmdType::ActionSpawnItems: {
                    auto* tr = m_entities->get_component<TransformComponent>(eid);
                    if (tr && m_world_items) {
                        static const char* s_items[] = {
                            "wrench","screwdriver","crowbar","wirecutters",
                            "stun_baton","fire_extinguisher","id_card","flashlight"
                        };
                        for (int i = 0; i < 8; ++i) {
                            float ox = (static_cast<float>(std::rand() % 100) / 100.f - 0.5f) * 2.4f;
                            float oz = (static_cast<float>(std::rand() % 100) / 100.f - 0.5f) * 2.4f;
                            m_world_items->spawn_by_id(s_items[i],
                                tr->pos + glm::vec3(ox, 0.1f, oz));
                        }
                        SDL_Log("Server: spawned test items at entity %u position", eid);
                    }
                    break;
                }
                case AdminCmdType::ToggleAutoHeal: {
                    p.dev_auto_heal = !p.dev_auto_heal;
                    SDL_Log("Server: auto_heal %s for entity %u", p.dev_auto_heal ? "ON" : "OFF", eid);
                    break;
                }
                case AdminCmdType::ToggleZeroGOverride: {
                    p.dev_zerog_override = !p.dev_zerog_override;
                    SDL_Log("Server: zerog_override %s for entity %u",
                            p.dev_zerog_override ? "ON" : "OFF", eid);
                    break;
                }
                case AdminCmdType::ToggleInfiniteOxy: {
                    p.dev_infinite_oxy = !p.dev_infinite_oxy;
                    SDL_Log("Server: infinite_oxy %s for entity %u",
                            p.dev_infinite_oxy ? "ON" : "OFF", eid);
                    break;
                }
                }
                break;
            }
            break;
        }
        default: break;
        }
    }
}

// ── Entity spawn broadcast ────────────────────────────────────────────────────
// Sends entity type information (MobComponent, HumanAppearance, etc.) to clients
// so they can create the correct rendering components.

void Server::broadcast_entity_spawns()
{
    if (m_peers.empty() || m_spawned_since_last_tick.empty()) {
        if (!m_spawned_since_last_tick.empty()) {
            SDL_Log("Server: broadcast_entity_spawns - %d entities to broadcast, %d peers",
                    (int)m_spawned_since_last_tick.size(), (int)m_peers.size());
        }
        return;
    }

    SDL_Log("Server: broadcasting %d entity spawns to %d peers",
            (int)m_spawned_since_last_tick.size(), (int)m_peers.size());

    for (EntityID eid : m_spawned_since_last_tick) {
        for (const auto& peer : m_peers) {
            // Don't send a player their own entity (they already have it)
            if (peer.player_entity == eid) {
                SDL_Log("Server: skipping entity %d for peer %s:%u (own entity)",
                        eid, inet_str(*reinterpret_cast<const in_addr*>(&peer.addr.ip)), peer.addr.port);
                continue;
            }
            SDL_Log("Server: sending entity %d spawn to peer %s:%u",
                    eid, inet_str(*reinterpret_cast<const in_addr*>(&peer.addr.ip)), peer.addr.port);
            send_entity_spawn_to(peer.addr, eid);
        }
    }
}

void Server::send_entity_spawn_to(const NetAddress& addr, EntityID eid)
{
    // Packet format:
    // [entity_id:4][type:1][x:4][y:4][z:4][yaw:4]
    // type: 0=player_mob, 1=npc_mob, 2=item
    // For mobs: [species_len:1][species:N]
    // For items: [item_type_id:2]

    auto* tr = m_entities->get_component<TransformComponent>(eid);
    if (!tr) return;

    std::vector<uint8_t> payload;
    auto append = [&](const void* data, size_t len) {
        payload.insert(payload.end(), static_cast<const uint8_t*>(data),
                       static_cast<const uint8_t*>(data) + len);
    };

    uint32_t eid_raw = static_cast<uint32_t>(eid);
    append(&eid_raw, 4);

    // Determine entity type and write type-specific data
    if (m_entities->has_component<HumanAppearance>(eid)) {
        // Player mob with HumanAppearance
        uint8_t type = 0;
        append(&type, 1);

        // Write species info
        auto* tag = m_entities->get_component<MobPlayerTag>(eid);
        auto* name = m_entities->get_component<NameComponent>(eid);
        std::string species = tag ? tag->species : "human";
        std::string display_name = name ? name->name : "Player";

        uint8_t species_len = static_cast<uint8_t>(species.size());
        append(&species_len, 1);
        append(species.data(), species.size());

        uint8_t name_len = static_cast<uint8_t>(display_name.size());
        append(&name_len, 1);
        append(display_name.data(), display_name.size());

        // Write appearance tint (first layer)
        auto* app = m_entities->get_component<HumanAppearance>(eid);
        if (app && !app->layers.empty()) {
            const auto& tint = app->layers[0].tint;
            append(&tint.r, 1);
            append(&tint.g, 1);
            append(&tint.b, 1);
            append(&tint.a, 1);
        } else {
            uint8_t default_tint[4] = {255, 200, 160, 255};
            append(default_tint, 4);
        }

        // Write eye color and hair from this player's stored appearance profile
        const Peer* peer_of_eid = nullptr;
        for (const auto& p : m_peers)
            if (p.player_entity == eid) { peer_of_eid = &p; break; }
        if (peer_of_eid) {
            append(&peer_of_eid->eye_r, 1);
            append(&peer_of_eid->eye_g, 1);
            append(&peer_of_eid->eye_b, 1);
            uint8_t hair_len = static_cast<uint8_t>(peer_of_eid->hair_file.size());
            append(&hair_len, 1);
            append(peer_of_eid->hair_file.data(), peer_of_eid->hair_file.size());
            append(&peer_of_eid->hair_r, 1);
            append(&peer_of_eid->hair_g, 1);
            append(&peer_of_eid->hair_b, 1);
            uint8_t gender_byte = peer_of_eid->is_male ? 1u : 0u;
            append(&gender_byte, 1);
        } else {
            // Default appearance for server-side NPCs / loopback players
            uint8_t defaults[] = {30, 100, 190,            // eye color (blue)
                                   10,                      // hair_len
                                   'h','a','i','r','_','m','e','s','s','y',
                                   89, 60, 30,               // hair color (brown)
                                   1};                       // is_male (default true)
            append(defaults, sizeof(defaults));
        }
    } else if (m_entities->has_component<MobComponent>(eid)) {
        // NPC mob with MobComponent
        uint8_t type = 1;
        append(&type, 1);

        auto* mob = m_entities->get_component<MobComponent>(eid);
        auto* name = m_entities->get_component<NameComponent>(eid);
        std::string species = mob ? mob->species : "unknown";
        std::string display_name = name ? name->name : species;

        uint8_t species_len = static_cast<uint8_t>(species.size());
        append(&species_len, 1);
        append(species.data(), species.size());

        uint8_t name_len = static_cast<uint8_t>(display_name.size());
        append(&name_len, 1);
        append(display_name.data(), display_name.size());
    } else if (m_entities->has_component<WorldItemComponent>(eid)) {
        // World item
        uint8_t type = 2;
        append(&type, 1);

        auto* item = m_entities->get_component<WorldItemComponent>(eid);
        // Send item def id as string for client-side lookup
        std::string item_id = (item && item->item.def) ? item->item.def->id : "";
        uint8_t id_len = static_cast<uint8_t>(item_id.size());
        append(&id_len, 1);
        append(item_id.data(), item_id.size());
    } else {
        // Unknown entity type - skip
        return;
    }

    // Write position and yaw
    append(&tr->pos.x, 4);
    append(&tr->pos.y, 4);
    append(&tr->pos.z, 4);
    append(&tr->yaw, 4);

    send_to(addr, PacketType::EntitySpawn, payload.data(), payload.size());
}

void Server::broadcast_entity_states()
{
    if (m_peers.empty()) return;

    struct EntityStateEntry { uint32_t id; float x, y, z, yaw; };
    std::vector<EntityStateEntry> entries;
    m_entities->each<TransformComponent>([&](EntityID eid, TransformComponent& tr) {
        entries.push_back({static_cast<uint32_t>(eid),
                           tr.pos.x, tr.pos.y, tr.pos.z, tr.yaw});
    });
    if (entries.empty()) return;

    for (const auto& peer : m_peers)
        send_to(peer.addr, PacketType::EntityState,
                entries.data(), entries.size() * sizeof(EntityStateEntry));
}

void Server::queue_player_input(EntityID id, const PlayerInput& input)
{
    m_pending_inputs[id] = input;
}

void Server::apply_pending_inputs(double dt)
{
    for (auto& [id, inp] : m_pending_inputs) {
        // Update entity yaw from player input
        auto* tr = m_entities->get_component<TransformComponent>(id);
        if (tr) {
            tr->yaw = inp.yaw;
        }
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

void Server::toggle_door(glm::ivec3 seed)
{
    if (!m_voxel_registry) return;

    const uint16_t door_id      = m_voxel_registry->id_of("door");
    const uint16_t door_anim_id = m_voxel_registry->id_of("door_anim");
    const uint16_t door_open_id = m_voxel_registry->id_of("door_open");
    if (door_id == 0 || door_open_id == 0) return;

    const Voxel seed_v = m_world->get_voxel(seed);
    // Accept door, door_anim (mid-animation), or door_open
    const bool opening = (seed_v.type_id == door_id || seed_v.type_id == door_anim_id);
    const bool closing = (seed_v.type_id == door_open_id);
    if (!opening && !closing) return;

    const uint16_t target_type = opening ? door_id : door_open_id;
    const uint16_t result_type = opening ? door_open_id : door_id;

    const VoxelTypeDef* result_def = m_voxel_registry->get(result_type);
    const uint16_t result_flags = result_def ? result_def->default_flags
                                             : static_cast<uint16_t>(VFLAG_VERT_PLANE_Z);

    // Flood-fill the door group (same Z-plane, same type or anim variant)
    std::vector<glm::ivec3> group;
    std::unordered_set<glm::ivec3> visited;
    std::vector<glm::ivec3> queue;
    queue.push_back(seed);
    visited.insert(seed);
    while (!queue.empty()) {
        glm::ivec3 cur = queue.back(); queue.pop_back();
        group.push_back(cur);
        static constexpr glm::ivec3 k4[4] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
        for (const auto& d : k4) {
            glm::ivec3 nb = cur + d;
            if (nb.z != seed.z) continue;
            if (!visited.insert(nb).second) continue;
            const uint16_t nb_t = m_world->get_voxel(nb).type_id;
            if (nb_t == target_type || nb_t == door_anim_id)
                queue.push_back(nb);
        }
    }

    Voxel result_v{};
    result_v.type_id = result_type;
    result_v.flags   = result_flags;
    for (const auto& p : group)
        m_world->set_voxel(p, result_v);

    // Notify atmos for every panel so opened cells get proper zones (not zero-pressure voids).
    m_atmos->on_cells_changed(group);
    SDL_Log("Server: door %s at (%d,%d,%d) (%d panels)",
            opening ? "opened" : "closed", seed.x, seed.y, seed.z,
            static_cast<int>(group.size()));
}

void Server::broadcast_dirty_chunks()
{
    auto dirty = m_world->dirty_chunks();
    if (dirty.empty()) return;

    if (m_peers.empty()) {
        // Nothing to broadcast, but still clear the dirty flags so
        // consecutive loopback ticks don't accumulate stale dirty chunks.
        for (Chunk* c : dirty) c->clear_dirty();
        return;
    }

    struct ChunkHdr { int32_t cx, cy, cz; };
    constexpr size_t k_payload = sizeof(ChunkHdr) + CHUNK_VOL * sizeof(uint16_t);

    for (Chunk* chunk : dirty) {
        // Build packet payload: [header 12B][type_id array 8192B]
        uint8_t payload[k_payload];
        const glm::ivec3 cp = chunk->chunk_pos();
        ChunkHdr hdr{ cp.x, cp.y, cp.z };
        std::memcpy(payload, &hdr, sizeof(hdr));

        auto* vox_data = reinterpret_cast<uint16_t*>(payload + sizeof(hdr));
        for (int z = 0; z < CHUNK_SIZE; ++z)
            for (int y = 0; y < CHUNK_SIZE; ++y)
                for (int x = 0; x < CHUNK_SIZE; ++x)
                    vox_data[x + CHUNK_SIZE * (y + CHUNK_SIZE * z)] =
                        chunk->get(x, y, z).type_id;

        for (const auto& peer : m_peers)
            send_to(peer.addr, PacketType::ChunkData, payload, k_payload);

        chunk->clear_dirty();
    }
}

void Server::send_chunk_to(const NetAddress& addr, glm::ivec3 cp, const Chunk& chunk)
{
    struct ChunkHdr { int32_t cx, cy, cz; };
    constexpr size_t k_payload = sizeof(ChunkHdr) + CHUNK_VOL * sizeof(uint16_t);

    uint8_t payload[k_payload];
    ChunkHdr hdr{ cp.x, cp.y, cp.z };
    std::memcpy(payload, &hdr, sizeof(hdr));

    auto* vox_data = reinterpret_cast<uint16_t*>(payload + sizeof(hdr));
    for (int z = 0; z < CHUNK_SIZE; ++z)
        for (int y = 0; y < CHUNK_SIZE; ++y)
            for (int x = 0; x < CHUNK_SIZE; ++x)
                vox_data[x + CHUNK_SIZE * (y + CHUNK_SIZE * z)] =
                    chunk.get(x, y, z).type_id;

    send_to(addr, PacketType::ChunkData, payload, k_payload);
}

void Server::send_to(NetAddress addr, PacketType type,
                     const void* data, size_t len)
{
    sock_t fd = to_sock(m_socket);
    if (fd == k_bad_sock) return; // loopback mode — no UDP socket

    // Wire format: [type:uint16_t LE][len:uint32_t LE][payload]
    const size_t pkt_size = 6 + len;
    // Static send buffer — avoids a heap allocation on every broadcast packet.
    // sendto() copies data synchronously before returning, so reusing the
    // buffer across consecutive peer sends in broadcast loops is safe.
    static uint8_t s_srv_send_buf[65536 + 6];
    if (pkt_size > sizeof(s_srv_send_buf)) return; // oversized — drop
    uint16_t t = static_cast<uint16_t>(type);
    uint32_t l = static_cast<uint32_t>(len);
    std::memcpy(s_srv_send_buf,     &t, 2);
    std::memcpy(s_srv_send_buf + 2, &l, 4);
    if (data && len > 0)
        std::memcpy(s_srv_send_buf + 6, data, len);

    sockaddr_in dest{};
    dest.sin_family      = AF_INET;
    dest.sin_addr.s_addr = addr.ip;
    dest.sin_port        = htons(addr.port);
    ::sendto(fd, reinterpret_cast<const char*>(s_srv_send_buf),
             static_cast<int>(pkt_size), 0,
             reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
}
