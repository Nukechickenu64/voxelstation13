// server_main.cpp — headless dedicated server entry point
// Compiles into vs13_server.exe (no renderer, no UI, no audio, no input).
// Usage: vs13_server [--port <N>]   (default port: 7778)

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "core/game_loop.h"
#include "core/world.h"
#include "core/entity_manager.h"
#include "core/signals.h"
#include "core/master_controller.h"
#include "core/object_types.h"
#include "core/verb_dispatch.h"
#include "simulation/atmos.h"
#include "simulation/physics.h"
#include "simulation/world_items.h"
#include "simulation/projectile_system.h"
#include "simulation/reagents.h"
#include "simulation/model_objects.h"
#include "data/voxel_registry.h"
#include "data/data_validator.h"
#include "data/mob_species_registry.h"
#include "inventory/inventory.h"
#include "inventory/item_registry.h"
#include "network/server.h"

#include <glm/glm.hpp>
#include <cstring>
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    // ── Parse command-line arguments ──────────────────────────────────────────
    uint16_t port = 7778;
    for (int i = 1; i < argc; ++i) {
        if (SDL_strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            long v = SDL_strtol(argv[++i], nullptr, 10);
            if (v > 0 && v <= 65535)
                port = static_cast<uint16_t>(v);
        }
    }

    // ── SDL init (events + timer only — no window, no GPU) ───────────────────
    if (!SDL_Init(SDL_INIT_EVENTS)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Log("[server] Starting on port %u", (unsigned)port);

    // ── Data validation ───────────────────────────────────────────────────────
    DataValidator validator;
    if (!validator.validate_all("data")) {
        SDL_Log("[server] Data validation failed with %u error(s).",
                static_cast<unsigned>(validator.errors().size()));
        // Non-fatal; continue anyway.
    }

    // ── Registry loading ──────────────────────────────────────────────────────
    VoxelRegistry voxel_reg;
    voxel_reg.load_directory("data/voxel_types");

    init_prototype_registry();
    init_verb_dispatch();
    init_signals();
    init_master_controller();
    init_reagents();

    ItemRegistry item_reg;
    item_reg.load_directory("data/item_types");

    MobSpeciesRegistry mob_species_reg;
    mob_species_reg.load_directory("data/mob_species");

    // ── Server setup ──────────────────────────────────────────────────────────
    Server server;
    server.set_species_registry(&mob_species_reg);
    server.set_item_registry(&item_reg);

    // ── Start network listener (also creates World/EntityManager/etc.) ────────
    if (!server.start(port)) {
        SDL_Log("[server] server.start() failed — exiting.");
        SDL_Quit();
        return 1;
    }
    SDL_Log("[server] Listening on UDP port %u — press Ctrl+C to stop", (unsigned)port);

    // ── Projectile system ─────────────────────────────────────────────────────
    ProjectileSystem projectile_sys(server.world(), server.entities());

    // ── Static model objects ──────────────────────────────────────────────────
    ModelObjectManager model_objs;
    server.set_model_objects(&model_objs);

    // ── Test world geometry ───────────────────────────────────────────────────
    {
        uint16_t floor_id = voxel_reg.id_of("floor");
        if (floor_id == 0) floor_id = 1;
        uint16_t wall_id = voxel_reg.id_of("reinforced_wall");
        if (wall_id == 0) wall_id = voxel_reg.id_of("wall");
        if (wall_id == 0) wall_id = 2;

        Voxel floor_voxel;
        floor_voxel.type_id = floor_id;
        floor_voxel.flags   = VFLAG_SOLID | VFLAG_OPAQUE;

        Voxel wall_voxel;
        wall_voxel.type_id = wall_id;
        wall_voxel.flags   = VFLAG_SOLID | VFLAG_OPAQUE;

        constexpr int ROOM_MIN  = -8;
        constexpr int ROOM_MAX  =  8;
        constexpr int WALL_TOP  =  4;
        constexpr int CEILING_Y =  5;

        constexpr int LOCK_X_LO  = -1;
        constexpr int LOCK_X_HI  =  1;
        constexpr int LOCK_DOOR_TOP = 3;
        constexpr int LOCK_Z_IN  =  ROOM_MAX;
        constexpr int LOCK_Z_OUT = ROOM_MAX + 4;
        constexpr int LOCK_WALL_LO = LOCK_X_LO - 1;
        constexpr int LOCK_WALL_HI = LOCK_X_HI + 1;
        constexpr int SPACE_FAR  = ROOM_MAX + 22;

        uint16_t plating_id = voxel_reg.id_of("floor_plating");
        if (plating_id == 0) plating_id = floor_id;
        Voxel plating_voxel;
        plating_voxel.type_id = plating_id;
        plating_voxel.flags   = VFLAG_SOLID | VFLAG_OPAQUE;

        // Floor
        for (int x = ROOM_MIN; x <= ROOM_MAX; ++x)
            for (int z = ROOM_MIN; z <= ROOM_MAX; ++z)
                server.world().set_voxel({x, 0, z}, floor_voxel);

        // Perimeter walls (with airlock gap in +Z wall)
        for (int i = ROOM_MIN; i <= ROOM_MAX; ++i) {
            for (int y = 1; y <= WALL_TOP; ++y) {
                server.world().set_voxel({ i,       y, ROOM_MIN}, wall_voxel);
                if (i < LOCK_X_LO || i > LOCK_X_HI || y > LOCK_DOOR_TOP)
                    server.world().set_voxel({ i, y, ROOM_MAX}, wall_voxel);
                server.world().set_voxel({ROOM_MIN, y,  i      }, wall_voxel);
                server.world().set_voxel({ROOM_MAX, y,  i      }, wall_voxel);
            }
        }

        // Ceiling
        for (int x = ROOM_MIN; x <= ROOM_MAX; ++x)
            for (int z = ROOM_MIN; z <= ROOM_MAX; ++z)
                server.world().set_voxel({x, CEILING_Y, z}, wall_voxel);

        // Interior pillars at (±5, *, ±5)
        constexpr int PIL = 5;
        for (int y = 1; y <= WALL_TOP; ++y) {
            server.world().set_voxel({ PIL, y,  PIL}, wall_voxel);
            server.world().set_voxel({-PIL, y,  PIL}, wall_voxel);
            server.world().set_voxel({ PIL, y, -PIL}, wall_voxel);
            server.world().set_voxel({-PIL, y, -PIL}, wall_voxel);
        }

        // Airlock vestibule floor
        for (int x = LOCK_WALL_LO; x <= LOCK_WALL_HI; ++x)
            for (int z = LOCK_Z_IN + 1; z < LOCK_Z_OUT; ++z)
                server.world().set_voxel({x, 0, z}, plating_voxel);

        // Airlock side walls
        for (int z = LOCK_Z_IN + 1; z <= LOCK_Z_OUT; ++z)
            for (int y = 1; y <= WALL_TOP; ++y) {
                server.world().set_voxel({LOCK_WALL_LO, y, z}, wall_voxel);
                server.world().set_voxel({LOCK_WALL_HI, y, z}, wall_voxel);
            }

        // Outer wall with door gap
        for (int x = LOCK_WALL_LO; x <= LOCK_WALL_HI; ++x)
            for (int y = 1; y <= WALL_TOP; ++y)
                if (x < LOCK_X_LO || x > LOCK_X_HI || y > LOCK_DOOR_TOP)
                    server.world().set_voxel({x, y, LOCK_Z_OUT}, wall_voxel);

        // Airlock doors
        uint16_t door_id = voxel_reg.id_of("door");
        if (door_id != 0) {
            Voxel door_voxel;
            door_voxel.type_id = door_id;
            door_voxel.flags   = VFLAG_SOLID | VFLAG_VERT_PLANE_Z;

            for (int x = LOCK_X_LO; x <= LOCK_X_HI; ++x)
                for (int y = 1; y <= LOCK_DOOR_TOP; ++y)
                    server.world().set_voxel({x, y, LOCK_Z_IN}, door_voxel);

            for (int x = LOCK_X_LO; x <= LOCK_X_HI; ++x)
                for (int y = 1; y <= LOCK_DOOR_TOP; ++y)
                    server.world().set_voxel({x, y, LOCK_Z_OUT}, door_voxel);
        }

        // Ceiling over airlock
        for (int x = LOCK_WALL_LO; x <= LOCK_WALL_HI; ++x)
            for (int z = LOCK_Z_IN + 1; z <= LOCK_Z_OUT; ++z)
                server.world().set_voxel({x, CEILING_Y, z}, wall_voxel);

        // Space platform floor (catwalk if available, else plating)
        uint16_t catwalk_id = voxel_reg.id_of("catwalk_floor");
        if (catwalk_id != 0) {
            Voxel catwalk_voxel;
            catwalk_voxel.type_id = catwalk_id;
            catwalk_voxel.flags   = VFLAG_SOLID | VFLAG_FLAT_TOP;
            for (int cx = LOCK_WALL_LO; cx <= LOCK_WALL_HI; ++cx)
                for (int cz = LOCK_Z_OUT + 1; cz <= SPACE_FAR; ++cz)
                    server.world().set_voxel({cx, 0, cz}, catwalk_voxel);
        } else {
            for (int x = LOCK_WALL_LO; x <= LOCK_WALL_HI; ++x)
                for (int z = LOCK_Z_OUT + 1; z <= SPACE_FAR; ++z)
                    server.world().set_voxel({x, 0, z}, plating_voxel);
        }

        // Handrail stubs at start of space platform
        for (int y = 1; y <= 2; ++y) {
            server.world().set_voxel({LOCK_WALL_LO, y, LOCK_Z_OUT + 1}, wall_voxel);
            server.world().set_voxel({LOCK_WALL_HI, y, LOCK_Z_OUT + 1}, wall_voxel);
        }

        // Light tubes (server simulates atmos which cares about emissive voxels)
        {
            uint16_t lt_id = voxel_reg.id_of("light_tube");
            if (lt_id != 0) {
                const VoxelTypeDef* lt_def = voxel_reg.get(lt_id);
                Voxel lt_voxel;
                lt_voxel.type_id = lt_id;
                lt_voxel.flags   = lt_def ? lt_def->default_flags
                                          : static_cast<uint16_t>(VFLAG_LIGHT_SRC);
                static const glm::ivec3 lt_pos[] = {
                    { 0, WALL_TOP - 1,  0},
                    {-4, WALL_TOP - 1, -4},
                    { 4, WALL_TOP - 1, -4},
                    {-4, WALL_TOP - 1,  4},
                    { 4, WALL_TOP - 1,  4},
                };
                for (const auto& lp : lt_pos)
                    server.world().set_voxel(lp, lt_voxel);
            }
        }

        // Test items on the floor
        auto spawn_test_item = [&](const char* item_id, int x, int z) {
            const ItemDef* def = item_reg.get(item_id);
            if (!def) return;
            ItemStack stack; stack.def = def; stack.count = 1;
            server.world_items().spawn_scattered({x, 0, z}, FaceDir::PosY, std::move(stack));
        };
        spawn_test_item("wrench",      2,  0);
        spawn_test_item("screwdriver", 2,  2);
        spawn_test_item("crowbar",    -2,  2);

        // Test NPC mobs
        server.spawn_npc("human",  { 3.f, 1.f,  3.f},   0.f, "Security Officer");
        server.spawn_npc("human",  {-4.f, 1.f, -2.f}, 180.f, "Scientist");
        server.spawn_npc("mouse",  { 2.f, 1.f,  5.f},  90.f, "Space Rat");
        server.spawn_npc("drone",  {-5.f, 1.f,  0.f}, 270.f, "Maintenance Drone");

        // Atmospherics bootstrap
        server.atmos().rebuild_zones();
    }

    // ── Main tick loop ────────────────────────────────────────────────────────
    constexpr double TICK_HZ   = 60.0;
    constexpr double TICK_SEC  = 1.0 / TICK_HZ;
    constexpr Uint32 TICK_MS   = static_cast<Uint32>(TICK_SEC * 1000.0 + 0.5);

    Uint64 prev_ticks = SDL_GetTicks();
    bool   running    = true;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT)
                running = false;
        }

        Uint64 now  = SDL_GetTicks();
        double dt   = static_cast<double>(now - prev_ticks) / 1000.0;
        prev_ticks  = now;

        server.tick(static_cast<float>(dt));
        projectile_sys.tick(dt, signals());

        // Sleep to target ~60 Hz without busy-spinning.
        Uint64 elapsed = SDL_GetTicks() - now;
        if (elapsed < TICK_MS)
            SDL_Delay(TICK_MS - static_cast<Uint32>(elapsed));
    }

    server.stop();
    SDL_Quit();
    return 0;
}
