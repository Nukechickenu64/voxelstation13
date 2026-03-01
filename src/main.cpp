#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "core/game_loop.h"
#include "core/world.h"
#include "core/entity_manager.h"
#include "render/renderer.h"
#include "render/chunk_mesher.h"
#include "render/lighting.h"
#include "render/ui_renderer.h"
#include "simulation/atmos.h"
#include "simulation/power.h"
#include "simulation/pipes.h"
#include "simulation/physics.h"
#include "simulation/world_items.h"
#include "simulation/mob_system.h"
#include "simulation/enclosure.h"
#include "input/input_manager.h"
#include "input/alt_mode.h"
#include "inventory/inventory.h"
#include "inventory/item_registry.h"
#include "audio/audio_manager.h"
#include "data/voxel_registry.h"
#include "data/data_validator.h"
#include "data/mob_species_registry.h"
#include "ui/hud.h"
#include "ui/inventory_panel.h"
#include "ui/context_menu.h"
#include "ui/creative_menu.h"
#include "ui/debug_overlay.h"
#include "ui/gas_overlay.h"
#include "ui/player_stats_overlay.h"
#include "simulation/model_objects.h"
#include "ui/map_editor.h"
#include "ui/admin_menu.h"
#include "core/object_types.h"
#include "network/server.h"
#include "network/client.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <unordered_set>

// Player inventory is now built by make_player_inventory() in inventory.cpp

// ────────────────────────────────────────────────────────────────────────────
int main(int /*argc*/, char* /*argv*/[])
{
    // ── 1. Data validation ────────────────────────────────────────────────────
    DataValidator validator;
    if (!validator.validate_all("data")) {
        SDL_Log("Data validation failed with %u error(s).",
                static_cast<unsigned>(validator.errors().size()));
        // Non-fatal during early development; continue anyway.
    }

    // ── 2. Registry loading ───────────────────────────────────────────────────
    VoxelRegistry voxel_reg;
    voxel_reg.load_directory("data/voxel_types");

    // Initialise the global prototype/type-path registry before item loading.
    init_prototype_registry();

    ItemRegistry item_reg;
    item_reg.load_directory("data/item_types");

    MobSpeciesRegistry mob_species_reg;
    mob_species_reg.load_directory("data/mob_species");

    // ── 3. Renderer + window ──────────────────────────────────────────────────
    Renderer renderer;
    if (!renderer.init("VoxelStation 13", 1280, 720)) {
        SDL_Log("Renderer init failed.");
        return 1;
    }
    renderer.load_tile_textures(voxel_reg, "textures");
    renderer.load_item_textures(item_reg,  "textures");
    renderer.load_mob_textures("textures");
    renderer.load_door_anim("textures/specialtile/public/glass/opening.gif",
                             voxel_reg.id_of("door_anim"));
    renderer.load_human_bodyparts("legacysets/extracted");
    renderer.load_model("smes", "models/SMES.mesh", "textures/models/smes.png");

    UIRenderer ui_renderer(renderer.gpu());
    if (!ui_renderer.init(renderer.window(), renderer.width(), renderer.height())) {
        SDL_Log("UIRenderer init failed.");
        return 1;
    }
    ui_renderer.load_item_icons(item_reg, "textures");

    // ── 4. Chunk mesher ───────────────────────────────────────────────────────
    ChunkMesher mesher;
    mesher.start(2);
    mesher.set_registry(&voxel_reg);  // enables per-face texture selection

    // ── 5. Server (local single-player) ──────────────────────────────────────
    Server server;
    server.start(0); // port 0 = loopback / no networking
    server.set_species_registry(&mob_species_reg);

    // ── 6. Client ─────────────────────────────────────────────────────────────
    Client client;
    client.connect_local(server);

    // ── 6b. Enclosure detector ────────────────────────────────────────────────
    EnclosureDetector enclosure_detector(server.world());

    // ── 7. Lighting ───────────────────────────────────────────────────────────
    LightingSystem lighting(server.world());
    lighting.set_registry(&voxel_reg);  // must be set before rebuild()
    mesher.set_lighting(&lighting);     // enables colored smooth lighting
    renderer.set_lighting(&server.world(), &lighting.light_map());  // entity/model lighting

    // ── 8. Audio ─────────────────────────────────────────────────────────────
    AudioManager audio;
    audio.init();
    audio.load_events("data/sounds.json");

    // ── 9. Input ──────────────────────────────────────────────────────────────
    InputManager input;
    input.capture_cursor(renderer.window(), true);

    AltMode alt_mode(input, renderer.window());

    // ── 10. UI layers ─────────────────────────────────────────────────────────
    HUD            hud(ui_renderer);
    InventoryPanel inv_panel(ui_renderer);
    ContextMenu    ctx_menu(ui_renderer);
    CreativeMenu   creative_menu(ui_renderer);
    DebugOverlay   debug_overlay(ui_renderer);
    bool           debug_overlay_visible = false;
    GasOverlay     gas_overlay(ui_renderer);
    bool           gas_overlay_visible   = false;
    PlayerStatsOverlay player_stats_overlay(ui_renderer);
    bool               player_stats_visible = false;
    AdminMenu      admin_menu(ui_renderer);
    bool           fullbright_enabled   = false;   // toggled via F1 admin menu
    bool           ao_enabled           = true;    // AO on by default
    double         sim_time              = 0.0;  // monotonic seconds, for overlay animations

    // ── 11. Player inventory ──────────────────────────────────────────────────
    Inventory player_inv = make_player_inventory();

    // ── 11b. World item system ────────────────────────────────────────────────
    WorldItemSystem world_items(server.world(), server.entities());

    // ── 11c. Static model objects ─────────────────────────────────────────────
    ModelObjectManager model_objs;
    {
        glm::vec3 mn, mx;
        if (renderer.model_local_aabb("smes", mn, mx))
            model_objs.register_extents("smes", mn, mx);
    }
    server.set_model_objects(&model_objs);

    // ── 11d. Map editor ───────────────────────────────────────────────────────
    std::vector<std::string> model_names{"smes"};
    MapEditor map_editor(ui_renderer, server.world(), voxel_reg,
                         server.entities(), world_items, item_reg,
                         mob_species_reg, model_objs, model_names);

    // ── 12. Test world geometry — placeholder room ────────────────────────────
    {
        // Resolve voxel type IDs (fall back to 1/2 when data not loaded)
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

        // Room extents: x/z = [-8, 8], y = 0 (floor) .. 5 (ceiling)
        constexpr int ROOM_MIN  = -8;
        constexpr int ROOM_MAX  =  8;
        constexpr int WALL_TOP  =  4;  // walls fill y = 1 .. WALL_TOP
        constexpr int CEILING_Y =  5;

        // Airlock + space constants
        constexpr int LOCK_X_LO  = -1;  // inner door gap: x = -1..1
        constexpr int LOCK_X_HI  =  1;
        constexpr int LOCK_DOOR_TOP = 3; // door opening height y=1..3
        constexpr int LOCK_Z_IN  =  ROOM_MAX;      // z=8  – inner wall (station side)
        constexpr int LOCK_Z_OUT = ROOM_MAX + 4;   // z=12 – outer wall (space side)
        constexpr int LOCK_WALL_LO = LOCK_X_LO - 1;  // x=-2
        constexpr int LOCK_WALL_HI = LOCK_X_HI + 1;  // x=2
        constexpr int SPACE_FAR  = ROOM_MAX + 22;  // z=30 – end of space platform

        uint16_t plating_id = voxel_reg.id_of("floor_plating");
        if (plating_id == 0) plating_id = floor_id;
        Voxel plating_voxel;
        plating_voxel.type_id = plating_id;
        plating_voxel.flags   = VFLAG_SOLID | VFLAG_OPAQUE;

        // Floor (y = 0)
        for (int x = ROOM_MIN; x <= ROOM_MAX; ++x)
            for (int z = ROOM_MIN; z <= ROOM_MAX; ++z)
                server.world().set_voxel({x, 0, z}, floor_voxel);

        // Perimeter walls (y = 1 .. WALL_TOP) — with airlock gap in +Z wall
        for (int i = ROOM_MIN; i <= ROOM_MAX; ++i) {
            for (int y = 1; y <= WALL_TOP; ++y) {
                server.world().set_voxel({ i,       y, ROOM_MIN}, wall_voxel);
                // +Z wall: leave gap at x=LOCK_X_LO..LOCK_X_HI, y=1..LOCK_DOOR_TOP
                if (i < LOCK_X_LO || i > LOCK_X_HI || y > LOCK_DOOR_TOP)
                    server.world().set_voxel({ i, y, ROOM_MAX}, wall_voxel);
                server.world().set_voxel({ROOM_MIN, y,  i      }, wall_voxel);
                server.world().set_voxel({ROOM_MAX, y,  i      }, wall_voxel);
            }
        }

        // Ceiling (y = CEILING_Y)
        for (int x = ROOM_MIN; x <= ROOM_MAX; ++x)
            for (int z = ROOM_MIN; z <= ROOM_MAX; ++z)
                server.world().set_voxel({x, CEILING_Y, z}, wall_voxel);

        // Four interior pillars at (±5, 1..WALL_TOP, ±5)
        constexpr int PIL = 5;
        for (int y = 1; y <= WALL_TOP; ++y) {
            server.world().set_voxel({ PIL, y,  PIL}, wall_voxel);
            server.world().set_voxel({-PIL, y,  PIL}, wall_voxel);
            server.world().set_voxel({ PIL, y, -PIL}, wall_voxel);
            server.world().set_voxel({-PIL, y, -PIL}, wall_voxel);
        }

        // ── Airlock vestibule (z = ROOM_MAX+1 .. LOCK_Z_OUT-1) ────────────────
        // Floor
        for (int x = LOCK_WALL_LO; x <= LOCK_WALL_HI; ++x)
            for (int z = LOCK_Z_IN + 1; z < LOCK_Z_OUT; ++z)
                server.world().set_voxel({x, 0, z}, plating_voxel);

        // Side walls x=LOCK_WALL_LO and x=LOCK_WALL_HI
        for (int z = LOCK_Z_IN + 1; z <= LOCK_Z_OUT; ++z)
            for (int y = 1; y <= WALL_TOP; ++y) {
                server.world().set_voxel({LOCK_WALL_LO, y, z}, wall_voxel);
                server.world().set_voxel({LOCK_WALL_HI, y, z}, wall_voxel);
            }

        // Outer wall at LOCK_Z_OUT — with matching door gap at center
        for (int x = LOCK_WALL_LO; x <= LOCK_WALL_HI; ++x)
            for (int y = 1; y <= WALL_TOP; ++y)
                if (x < LOCK_X_LO || x > LOCK_X_HI || y > LOCK_DOOR_TOP)
                    server.world().set_voxel({x, y, LOCK_Z_OUT}, wall_voxel);

        // ── Airlock doors ──────────────────────────────────────────────────────
        // Close off both wall gaps with double-sided planar door voxels.
        uint16_t door_id = voxel_reg.id_of("door");
        if (door_id != 0) {
            Voxel door_voxel;
            door_voxel.type_id = door_id;
            door_voxel.flags   = VFLAG_SOLID | VFLAG_VERT_PLANE_Z;

            // Inner door (station side at z=LOCK_Z_IN)
            for (int x = LOCK_X_LO; x <= LOCK_X_HI; ++x)
                for (int y = 1; y <= LOCK_DOOR_TOP; ++y)
                    server.world().set_voxel({x, y, LOCK_Z_IN}, door_voxel);

            // Outer door (space side at z=LOCK_Z_OUT)
            for (int x = LOCK_X_LO; x <= LOCK_X_HI; ++x)
                for (int y = 1; y <= LOCK_DOOR_TOP; ++y)
                    server.world().set_voxel({x, y, LOCK_Z_OUT}, door_voxel);
        }

        // Ceiling over airlock
        for (int x = LOCK_WALL_LO; x <= LOCK_WALL_HI; ++x)
            for (int z = LOCK_Z_IN + 1; z <= LOCK_Z_OUT; ++z)
                server.world().set_voxel({x, CEILING_Y, z}, wall_voxel);

        // ── Space platform (z = LOCK_Z_OUT+1 .. SPACE_FAR) ───────────────────
        // Open to the void — no ceiling, no side walls
        for (int x = LOCK_WALL_LO; x <= LOCK_WALL_HI; ++x)
            for (int z = LOCK_Z_OUT + 1; z <= SPACE_FAR; ++z)
                server.world().set_voxel({x, 0, z}, plating_voxel);

        // Handrail stubs at the start of the space platform (first 2 blocks)
        for (int y = 1; y <= 2; ++y) {
            server.world().set_voxel({LOCK_WALL_LO, y, LOCK_Z_OUT + 1}, wall_voxel);
            server.world().set_voxel({LOCK_WALL_HI, y, LOCK_Z_OUT + 1}, wall_voxel);
        }

        // ── Space platform: catwalk_floor grating replaces solid plating ─────
        // catwalk_floor renders as a thin plane at the TOP of the voxel cell
        // and is solid (player walks on it) but see-through (no opaque fill).
        uint16_t catwalk_id = voxel_reg.id_of("catwalk_floor");
        if (catwalk_id != 0) {
            Voxel catwalk_voxel;
            catwalk_voxel.type_id = catwalk_id;
            catwalk_voxel.flags   = VFLAG_SOLID | VFLAG_FLAT_TOP;

            for (int cx = LOCK_WALL_LO; cx <= LOCK_WALL_HI; ++cx)
                for (int cz = LOCK_Z_OUT + 1; cz <= SPACE_FAR; ++cz)
                    server.world().set_voxel({cx, 0, cz}, catwalk_voxel);
        }

        // Place the player above the floor so they aren't embedded in it
        EntityID player_ent = client.local_player();
        if (player_ent != NULL_ENTITY) {
            auto* tr = server.entities().get_component<TransformComponent>(player_ent);
            if (tr) tr->pos = {0.f, 1.f, 0.f};
        }

        // Enqueue all dirty chunks for meshing
        for (Chunk* c : server.world().dirty_chunks())
            mesher.enqueue(c->chunk_pos(), server.world());

        // ── Lighting: place light tubes, then run full light propagation ─────
        // Light tubes sit just below the ceiling (y=WALL_TOP-1 = 3), flush with
        // the ceiling interior face, in a cross pattern inside the main room.
        {
            uint16_t lt_id = voxel_reg.id_of("light_tube");
            if (lt_id != 0) {
                const VoxelTypeDef* lt_def = voxel_reg.get(lt_id);
                Voxel lt_voxel;
                lt_voxel.type_id = lt_id;
                lt_voxel.flags   = lt_def ? lt_def->default_flags : static_cast<uint16_t>(VFLAG_LIGHT_SRC);

                // Five lights in a cross pattern on the ceiling interior face.
                static const glm::ivec3 lt_pos[] = {
                    { 0, WALL_TOP - 1,  0},   // centre
                    {-4, WALL_TOP - 1, -4},   // NW corner
                    { 4, WALL_TOP - 1, -4},   // NE corner
                    {-4, WALL_TOP - 1,  4},   // SW corner
                    { 4, WALL_TOP - 1,  4},   // SE corner
                };
                for (const auto& lp : lt_pos)
                    server.world().set_voxel(lp, lt_voxel);
            }
        }

        // Full lighting rebuild now that emissive voxels are in place.
        lighting.rebuild();

        // Re-enqueue chunks whose light_level values changed.
        for (Chunk* c : server.world().dirty_chunks())
            mesher.enqueue(c->chunk_pos(), server.world());

        // Spawn a couple of test items on the floor for M1 testing.
        // Use spawn_scattered() so items land at random offsets on the turf.
        auto spawn_test_item = [&](const char* item_id, int x, int z) {
            const ItemDef* def = item_reg.get(item_id);
            if (!def) return;
            ItemStack stack; stack.def = def; stack.count = 1;
            world_items.spawn_scattered({x, 0, z}, FaceDir::PosY, std::move(stack));
        };
        spawn_test_item("wrench",      2,  0);
        spawn_test_item("screwdriver", 2,  2);
        spawn_test_item("crowbar",    -2,  2);

        // Spawn a mob dummy for Doom-style sprite testing.
        // Stand at (4, 1, -2): visible from the player start at (0,1,0).
        // Walk around it to see all four rotation sprites change.
        {
            EntityID dummy = server.entities().create();
            TransformComponent tr{};
            tr.pos = { 4.f, 1.f, -2.f };
            tr.yaw = 0.f;   // faces -Z (same default as the camera)
            server.entities().add_component<TransformComponent>(dummy, tr);
            HumanAppearance app{};
            HumanOverlay base{};
            base.sprite_dir = "bodyparts_greyscale";
            base.prefix     = "human";
            base.gender     = "_m";   // masculine greyscale sprites
            base.tint       = {255, 200, 160, 255};  // default light skin tone
            app.layers.push_back(base);
            app.dirty = true;
            server.entities().add_component<HumanAppearance>(dummy, app);
            // Give the dummy a wrench in-hand so inhand overlays are exercised.
            {
                InventoryComponent ic{};
                const ItemDef* wd = item_reg.get("wrench");
                if (wd) {
                    ItemStack ws; ws.def = wd; ws.count = 1;
                    ic.inv.put("l_hand", std::move(ws));
                }
                server.entities().add_component<InventoryComponent>(dummy, std::move(ic));
            }
        }

        // ── Atmospherics bootstrap ──────────────────────────────────────────
        // Build the initial room graph.  Must happen AFTER all voxels are
        // placed so the BFS sees the completed geometry.
        server.atmos().rebuild_zones();
    }

    // ── 13. HUD state ─────────────────────────────────────────────────────────
    HUDState hud_state;
    // health values are synced each tick from HealthComponent — initialise
    // to sensible defaults in case the component isn't ready yet.
    hud_state.health     = 100.f;
    hud_state.health_max = 100.f;

    // ── 14. Camera state ──────────────────────────────────────────────────────
    float cam_yaw   = 0.f;
    float cam_pitch = 0.f;
    glm::vec3 cam_pos = {0.f, 1.5f, 0.f};  // player feet at y=1, eyes at +0.5

    // Currently hovered / targeted world item entity (updated each render frame)
    EntityID hovered_item_entity = NULL_ENTITY;
    std::vector<EntityID> item_candidates;   // all items in selection area, sorted nearest-first
    int scroll_item_idx = 0;                 // which candidate is selected (scroll wheel cycles)

    // Alt-mode world drag: entity whose item is being dragged into a panel slot
    EntityID alt_drag_entity = NULL_ENTITY;

    // True while a context menu was opened via FPS right-click (not alt-mode).
    bool fps_ctx_rclick      = false;
    bool fps_ctx_just_opened = false;
    EntityID fps_ctx_entity  = NULL_ENTITY; // entity the menu was opened on

    // ── 14b. Build mode state ─────────────────────────────────────────────────
    bool build_mode = false;
    Voxel build_voxel;
    {
        uint16_t bv_id = voxel_reg.id_of("reinforced_wall");
        if (bv_id == 0) bv_id = voxel_reg.id_of("wall");
        if (bv_id == 0) bv_id = 2;
        build_voxel.type_id = bv_id;
        build_voxel.flags   = VFLAG_SOLID | VFLAG_OPAQUE;
    }

    // ── 14c. Door state ───────────────────────────────────────────────────────
    // Three voxel type IDs for the door state machine:
    //   door      → closed, solid, shows door.png
    //   door_anim → animating, solid, tile layer updated frame-by-frame from GIF
    //   door_open → fully open, passable, shows door_open.png
    const uint16_t door_type_id      = voxel_reg.id_of("door");
    const uint16_t door_anim_type_id = voxel_reg.id_of("door_anim");
    const uint16_t door_open_type_id = voxel_reg.id_of("door_open");

    // Speed multiplier for door open/close animation.
    // 1.0 = original GIF speed, 2.0 = twice as fast, 0.5 = half speed.
    constexpr float DOOR_ANIM_SPEED = 6.0f;

    // Per-panel door animation state (one entry per door group currently animating).
    struct DoorGroup {
        std::vector<glm::ivec3> voxels;
        int   frame    = 0;
        float accum_ms = 0.f;
        bool  closing  = false;  // true = playing frames in reverse toward door state
    };
    std::vector<DoorGroup> animating_doors;

    // Flood-fill: find all adjacent voxels of target_type in the same Z-plane.
    auto flood_fill_door = [&](glm::ivec3 seed, uint16_t target_type) {
        std::vector<glm::ivec3>             group;
        std::unordered_set<glm::ivec3> visited;
        std::vector<glm::ivec3>             queue;
        auto try_push = [&](glm::ivec3 p) {
            if (p.z != seed.z) return;
            if (!visited.insert(p).second) return;
            if (server.world().get_voxel(p).type_id == target_type)
                queue.push_back(p);
        };
        try_push(seed);
        while (!queue.empty() && static_cast<int>(group.size()) < 64) {
            glm::ivec3 cur = queue.back(); queue.pop_back();
            group.push_back(cur);
            try_push(cur + glm::ivec3{ 1, 0, 0});
            try_push(cur + glm::ivec3{-1, 0, 0});
            try_push(cur + glm::ivec3{ 0, 1, 0});
            try_push(cur + glm::ivec3{ 0,-1, 0});
        }
        return group;
    };

    // ── 15. Game loop ─────────────────────────────────────────────────────────
    GameLoop loop(1.0 / 60.0);

    loop.run(
        // Fixed-timestep update
        [&](double dt) {
            // Poll SDL events
            SDL_Event e;
            input.begin_frame();
            // Invalidate enclosure cache each tick so world changes are reflected
            enclosure_detector.invalidate();
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) { loop.stop(); return; }
                if (e.type == SDL_EVENT_WINDOW_RESIZED) {
                    int w = e.window.data1, h = e.window.data2;
                    ui_renderer.on_resize(w, h);
                }
                input.process_event(e);
            }

            // Alt-mode
            alt_mode.set_camera_angles(cam_yaw, cam_pitch);
            bool mode_changed = alt_mode.update();
            (void)mode_changed;

            // Camera look (only when not in alt-mode)
            if (!alt_mode.active()) {
                const float SENSITIVITY = 0.15f;
                glm::vec2 mdelta = input.mouse_delta();
                cam_yaw   += mdelta.x * SENSITIVITY;
                cam_pitch  = glm::clamp(cam_pitch - mdelta.y * SENSITIVITY, -89.f, 89.f);
            }

            // F1: toggle admin menu
            // F2: creative menu  (unchanged)
            // F7: map editor     (unchanged)
            EntityID player = client.local_player();
            {
                const bool* ks = SDL_GetKeyboardState(nullptr);
                // F1 — admin menu
                {
                    static bool s_f1_prev = false;
                    bool f1_now = ks[SDL_SCANCODE_F1];
                    if (f1_now && !s_f1_prev) {
                        if (admin_menu.is_open()) {
                            admin_menu.close();
                            if (!alt_mode.active() && !creative_menu.is_open() && !map_editor.is_open())
                                input.capture_cursor(renderer.window(), true);
                        } else {
                            admin_menu.open();
                            input.capture_cursor(renderer.window(), false);
                        }
                    }
                    s_f1_prev = f1_now;
                }
                // F2 creative menu
                {
                    static bool s_f2_prev = false;
                    bool f2_now = ks[SDL_SCANCODE_F2];
                    if (f2_now && !s_f2_prev) {
                        if (creative_menu.is_open()) {
                            creative_menu.close();
                            // Re-capture cursor unless alt-mode is also active
                            if (!alt_mode.active())
                                input.capture_cursor(renderer.window(), true);
                        } else {
                            creative_menu.open(item_reg, voxel_reg);
                            // Free the cursor so the player can click items
                            input.capture_cursor(renderer.window(), false);
                        }
                    }
                    s_f2_prev = f2_now;
                }
                // F7: toggle map editor
                {
                    static bool s_f7_prev = false;
                    bool f7_now = ks[SDL_SCANCODE_F7];
                    if (f7_now && !s_f7_prev) {
                        if (map_editor.is_open()) {
                            map_editor.close();
                            if (!alt_mode.active())
                                input.capture_cursor(renderer.window(), true);
                        } else {
                            map_editor.open(cam_pos);
                            input.capture_cursor(renderer.window(), false);
                        }
                    }
                    s_f7_prev = f7_now;
                }
            }

            // Movement wish direction – full 3D when noclip or jetpack in zero-G,
            // XZ-only otherwise.  No wish is sent when zero-G without a jetpack
            // (the player is helpless; only collisions and existing momentum matter).
            glm::vec3 wish = {};
            float yaw_rad   = glm::radians(cam_yaw);
            float pitch_rad = glm::radians(cam_pitch);

            // Read and update controller state for this frame
            bool noclip_active   = false;
            bool zero_g_active   = false;
            bool jetpack_present = false;
            if (player != NULL_ENTITY) {
                auto* cc_r = server.entities().get_component<CharacterControllerComponent>(player);
                if (cc_r) {
                    noclip_active = cc_r->noclip;
                    zero_g_active = cc_r->zero_g;

                    // Detect jetpack in back slot (client inventory mirrors equipment)
                    const auto* back = player_inv.find_slot("back");
                    if (back && back->item && back->item->def) {
                        for (const auto& tag : back->item->def->tags) {
                            if (tag == "jetpack") { jetpack_present = true; break; }
                        }
                    }
                    cc_r->jetpack_equipped = jetpack_present;
                }
            }

            // Use full 3D thrust direction in noclip, zero-G+jetpack, or wall-grab
            bool grab_wall_held = input.is_held(Action::GrabWall);
            bool full_3d = noclip_active || (zero_g_active && jetpack_present) || (zero_g_active && grab_wall_held);
            glm::vec3 fwd, right;
            if (full_3d) {
                fwd = {
                    std::cos(pitch_rad) * std::sin(yaw_rad),
                    std::sin(pitch_rad),
                   -std::cos(pitch_rad) * std::cos(yaw_rad)
                };
                right = glm::normalize(glm::cross(fwd, {0.f, 1.f, 0.f}));
            } else {
                fwd   = {std::sin(yaw_rad), 0.f, -std::cos(yaw_rad)};
                right = glm::cross(fwd, {0.f, 1.f, 0.f});
            }

            // In zero-G without a jetpack the player cannot self-propel,
            // unless they are wall-grabbing (Ctrl) — then WASD crawls along surfaces
            bool can_move = !zero_g_active || jetpack_present || noclip_active || grab_wall_held;
            if (can_move) {
                if (input.is_held(Action::MoveForward)) wish += fwd;
                if (input.is_held(Action::MoveBack))    wish -= fwd;
                if (input.is_held(Action::MoveRight))   wish += right;
                if (input.is_held(Action::MoveLeft))    wish -= right;
            }

            // Submit movement input to server
            if (player != NULL_ENTITY) {
                server.queue_player_input(player, PlayerInput{
                    wish,
                    input.is_held(Action::Sprint),
                    grab_wall_held,
                });
            }

            // Switch active hand (Space) — always available
            if (input.is_pressed(Action::SwitchHand))
                player_inv.cycle_active_hand();

            // Scroll wheel cycles through overlapping items in the selection area
            // (suppress when the context menu is open — it owns the scroll there)
            float scroll = input.scroll_delta();
            if (scroll != 0.f && !item_candidates.empty() && !ctx_menu.is_open()) {
                scroll_item_idx -= (scroll > 0.f ? 1 : -1);
                int n = static_cast<int>(item_candidates.size());
                scroll_item_idx = ((scroll_item_idx % n) + n) % n;
            }

            // Advance simulation time for overlay animations
            sim_time += dt;

            // Server tick (applies pending inputs, steps physics + simulations)
            server.tick(dt);
            client.tick(dt);
            world_items.tick(dt);  // settle floating items that have come to rest

            // ── Zero-G: anywhere outside a pressurised room is zero-G ──────────
            // Untracked cells (null zone) = open space → zero-G ON.
            // Only cells inside an explicit non-space atmos zone have gravity.
            if (player != NULL_ENTITY) {
                auto* cc2 = server.entities().get_component<CharacterControllerComponent>(player);
                auto* tr2 = server.entities().get_component<TransformComponent>(player);
                if (cc2 && tr2) {
                    glm::ivec3 cell = {
                        static_cast<int>(std::floor(tr2->pos.x)),
                        static_cast<int>(std::floor(tr2->pos.y)),
                        static_cast<int>(std::floor(tr2->pos.z))
                    };
                    AtmosZoneID az        = server.atmos().zone_at(cell);
                    const AtmosZone* zptr = server.atmos().zone(az);
                    // In space if: no tracked zone (open void) OR zone is explicitly space
                    bool in_space = !zptr || zptr->is_space;
                    if (in_space != cc2->zero_g) {
                        cc2->zero_g = in_space;
                        SDL_Log("Zero-G: %s", in_space ? "entered space" : "left space");
                    }
                }
            }

            // Sync camera to physics-driven player position
            // Also sync the player body yaw to the camera horizontal direction so
            // the body always faces where the player is looking.
            if (player != NULL_ENTITY) {
                auto* tr = server.entities().get_component<TransformComponent>(player);
                if (tr) {
                    cam_pos  = tr->pos + glm::vec3(0, 0.5f, 0);
                    tr->yaw  = cam_yaw;  // body follows camera horizontal

                    // Push the camera slightly forward so the player can see
                    // their own body when looking down in first-person.
                    float yaw_r = glm::radians(cam_yaw);
                    glm::vec3 horiz_fwd = { std::sin(yaw_r), 0.f, -std::cos(yaw_r) };
                    cam_pos += horiz_fwd * 0.1f;
                }
            }

            // Upload any finished chunk meshes
            for (auto& mesh : mesher.collect_finished())
                renderer.upload_mesh(renderer.get_or_create_mesh(mesh.chunk_pos) = std::move(mesh));

            // Lighting update for dirtied chunks
            // lighting.update(…)

            // Audio listener
            glm::vec3 fwd3 = {std::sin(glm::radians(cam_yaw)),
                               std::sin(glm::radians(cam_pitch)),
                              -std::cos(glm::radians(cam_yaw))};
            audio.set_listener(cam_pos, glm::normalize(fwd3), {0,1,0});
            // Read actual gas pressure at player position for audio
            {
                glm::ivec3 atmos_cell = {
                    static_cast<int>(std::floor(cam_pos.x)),
                    static_cast<int>(std::floor(cam_pos.y)),
                    static_cast<int>(std::floor(cam_pos.z))
                };
                AtmosZoneID az = server.atmos().zone_at(atmos_cell);
                AtmosZone* az_ptr = server.atmos().zone(az);
                float local_kpa = az_ptr ? az_ptr->gas.total_pressure() : 0.f;
                audio.set_local_pressure(local_kpa > 0.f ? local_kpa : 101.325f);
            }
            audio.update(static_cast<float>(dt));

            // Update HUD state
            hud_state.clock_str          = "00:00"; // TODO: round timer
            hud_state.cam_pitch          = cam_pitch;
            hud_state.active_hand_is_left = (player_inv.active_hand_id() == "l_hand");

            // ── Player health from HealthComponent ───────────────────────────
            if (player != NULL_ENTITY) {
                auto* hp = server.entities().get_component<HealthComponent>(player);
                if (hp) {
                    hud_state.health     = hp->current();
                    hud_state.health_max = hp->health_max;
                }
            }

            // ── Suit sensors: read atmos at player feet ───────────────────────
            {
                glm::ivec3 feet_cell = {
                    static_cast<int>(std::floor(cam_pos.x)),
                    static_cast<int>(std::floor(cam_pos.y - 0.5f)),
                    static_cast<int>(std::floor(cam_pos.z))
                };
                AtmosZoneID   az      = server.atmos().zone_at(feet_cell);
                const AtmosZone* zptr = server.atmos().zone(az);
                if (zptr && !zptr->is_space && zptr->gas.total_pressure() > 0.f) {
                    const GasMixture& g       = zptr->gas;
                    hud_state.oxy_sat          = std::clamp(g.o2 / 21.f, 0.f, 1.f);
                    hud_state.suit_pressure_kpa = g.total_pressure();
                    hud_state.tox_level        = g.plasma + g.bz + g.n2o;
                    char tmp[32];
                    std::snprintf(tmp, sizeof(tmp), "%.0f K", g.temperature);
                    hud_state.suit_temp_str = tmp;
                } else {
                    // Space or untracked — vacuum conditions
                    hud_state.oxy_sat           = 0.f;
                    hud_state.suit_pressure_kpa = 0.f;
                    hud_state.tox_level         = 0.f;
                    hud_state.suit_temp_str     = "2.7 K";
                }
            }

            // ── Item drop (X) ─────────────────────────────────────────────────
            if (input.is_pressed(Action::DropItem)) {
                auto maybe_item = player_inv.take(player_inv.active_hand_id());
                if (maybe_item) {
                    // Cast downward from player feet to find a floor face
                    RayHit down = server.world().raycast(cam_pos, {0,-1,0}, 3.f);
                    if (down.valid) {
                        // Drop with scatter so items don't all pile at turf centre
                        world_items.spawn_scattered(down.voxel, down.face, std::move(*maybe_item));
                    } else {
                        // No floor nearby – spawn floating at player feet
                        world_items.spawn_floating(
                            cam_pos - glm::vec3(0, 0.5f, 0), std::move(*maybe_item));
                    }
                }
            }

            // ── Item throw (F) ────────────────────────────────────────────────
            // Tosses the active-hand item forward in the camera direction.
            // The item spawns as a floating entity ~1 m in front of the player;
            // gravity / physics will carry it from there.
            if (input.is_pressed(Action::ThrowItem)) {
                auto maybe_item = player_inv.take(player_inv.active_hand_id());
                if (maybe_item) {
                    float yr = glm::radians(cam_yaw), pr = glm::radians(cam_pitch);
                    glm::vec3 throw_dir = {
                        std::cos(pr) * std::sin(yr),
                        std::sin(pr),
                       -std::cos(pr) * std::cos(yr)
                    };
                    // Clamp downward pitch so the item doesn't launch into the floor
                    if (throw_dir.y < -0.3f) throw_dir.y = -0.3f;
                    throw_dir = glm::normalize(throw_dir);

                    // Throw speed in m/s
                    constexpr float THROW_SPEED = 8.f;

                    // Spawn 1 m in front of the player at eye height
                    glm::vec3 spawn_pos = cam_pos + throw_dir * 1.0f;
                    world_items.spawn_floating(spawn_pos, std::move(*maybe_item),
                                               throw_dir * THROW_SPEED);
                    SDL_Log("Throw: item tossed forward at %.1f m/s", THROW_SPEED);
                }
            }

            // ── Door animation tick ──────────────────────────────────────────
            // Advance each animating door panel, update door_anim tile layer,
            // and finalise to door_open when all frames have played.
            if (door_anim_type_id != 0 && renderer.door_anim_frame_count() > 0) {
                const float dt_ms = static_cast<float>(dt * 1000.0) * DOOR_ANIM_SPEED;
                int last_frame = -1;
                for (auto it = animating_doors.begin(); it != animating_doors.end(); ) {
                    DoorGroup& grp = *it;
                    grp.accum_ms += dt_ms;

                    if (!grp.closing) {
                        // Opening: advance forward
                        while (grp.frame < renderer.door_anim_frame_count()) {
                            int delay = renderer.door_anim_frame_delay_ms(grp.frame);
                            if (grp.accum_ms < static_cast<float>(delay)) break;
                            grp.accum_ms -= static_cast<float>(delay);
                            ++grp.frame;
                        }
                        if (grp.frame >= renderer.door_anim_frame_count()) {
                            // All frames played → switch voxels to door_open
                            if (door_open_type_id != 0) {
                                Voxel open_v;
                                open_v.type_id = door_open_type_id;
                                // Use registry default_flags so GAS_PASSABLE etc. are included.
                                {
                                    const VoxelTypeDef* dod = voxel_reg.get(door_open_type_id);
                                    open_v.flags = dod ? dod->default_flags
                                                       : static_cast<uint16_t>(VFLAG_VERT_PLANE_Z | VFLAG_GAS_PASSABLE);
                                }
                                for (auto& p : grp.voxels)
                                    server.world().set_voxel(p, open_v);
                                for (Chunk* c : server.world().dirty_chunks())
                                    mesher.enqueue(c->chunk_pos(), server.world());
                            }
                            // Door is now gas-passable — notify atmos so zones merge.
                            if (!grp.voxels.empty())
                                server.atmos().on_door_changed(grp.voxels.front());
                            it = animating_doors.erase(it);
                            continue;
                        }
                    } else {
                        // Closing: play frames in reverse
                        while (grp.frame >= 0) {
                            int delay = renderer.door_anim_frame_delay_ms(grp.frame);
                            if (grp.accum_ms < static_cast<float>(delay)) break;
                            grp.accum_ms -= static_cast<float>(delay);
                            --grp.frame;
                        }
                        if (grp.frame < 0) {
                            // All reversed frames played → switch voxels to door (closed)
                            if (door_type_id != 0) {
                                Voxel closed_v;
                                closed_v.type_id = door_type_id;
                                closed_v.flags   = VFLAG_SOLID | VFLAG_VERT_PLANE_Z;
                                for (auto& p : grp.voxels)
                                    server.world().set_voxel(p, closed_v);
                                for (Chunk* c : server.world().dirty_chunks())
                                    mesher.enqueue(c->chunk_pos(), server.world());
                            }
                            // Door is now fully sealed — notify atmos to finalise zone split.
                            if (!grp.voxels.empty())
                                server.atmos().on_door_changed(grp.voxels.front());
                            it = animating_doors.erase(it);
                            continue;
                        }
                    }

                    last_frame = grp.frame;
                    ++it;
                }
                if (last_frame >= 0)
                    renderer.update_tile_layer(door_anim_type_id,
                                               renderer.door_anim_frame_pixels(last_frame));
            }

            // ── LMB / E: interact with world based on active hand ─────────────
            // Empty hand  + item  → pick up
            // Empty hand  + turf  → nothing (can't pick up turf)
            // Held item   + item  → item-on-item interaction
            // Held item   + turf  → knock on the wall with held item
            {
                bool fps_lmb = !alt_mode.active() && !build_mode && !ctx_menu.is_open() && input.is_pressed(Action::PrimaryInteract);
                bool e_press = !alt_mode.active() && !ctx_menu.is_open() && input.is_pressed(Action::PickUp);
                if (fps_lmb || e_press) {
                    constexpr float ITEM_REACH = 1.5f;
                    float yr = glm::radians(cam_yaw), pr = glm::radians(cam_pitch);
                    glm::vec3 rdir = {
                        std::cos(pr) * std::sin(yr),
                        std::sin(pr),
                       -std::cos(pr) * std::cos(yr)
                    };
                    auto*  active_slot = player_inv.active_hand();
                    bool   hand_empty  = !active_slot || !active_slot->item;

                    RayHit fhit = server.world().raycast(cam_pos, rdir, ITEM_REACH);
                    float item_dist = 0.f;
                    EntityID item_ent = world_items.ray_cast_items(
                        cam_pos, rdir, ITEM_REACH,
                        fhit.valid ? fhit.distance : ITEM_REACH, item_dist);

                    if (item_ent != NULL_ENTITY) {
                        if (hand_empty || e_press) {
                            // Pick up — auto_equip tries all slots; re-spawn if no room
                            auto picked = world_items.pick_up(item_ent);
                            if (picked && picked->def) {
                                auto leftover = player_inv.auto_equip(std::move(*picked));
                                if (leftover)
                                    world_items.spawn_floating(cam_pos, std::move(*leftover));
                            }
                        } else {
                            // Use held item on world item
                            auto* wic = server.entities().get_component<WorldItemComponent>(item_ent);
                            if (wic && wic->item.def) {
                                SDL_Log("Interact: %s on %s",
                                        active_slot->item->def->name.c_str(),
                                        wic->item.def->name.c_str());
                                audio.play("click", cam_pos);
                            }
                        }
                    } else if (fhit.valid) {
                        Voxel hit_v = server.world().get_voxel(fhit.voxel);
                        bool has_id = active_slot && active_slot->item && active_slot->item->def
                            && [&]{
                                for (auto& t : active_slot->item->def->tags)
                                    if (t == "id_card") return true;
                                return false;
                            }();

                        // ── Open door ───────────────────────────────────
                        if (door_type_id != 0 && hit_v.type_id == door_type_id
                            && (hand_empty || has_id)) {
                            auto grp_voxels = flood_fill_door(fhit.voxel, door_type_id);
                            if (!grp_voxels.empty() && door_anim_type_id != 0) {
                                Voxel anim_v;
                                anim_v.type_id = door_anim_type_id;
                                anim_v.flags   = VFLAG_SOLID | VFLAG_VERT_PLANE_Z;
                                for (auto& p : grp_voxels)
                                    server.world().set_voxel(p, anim_v);
                                for (Chunk* c : server.world().dirty_chunks())
                                    mesher.enqueue(c->chunk_pos(), server.world());
                                DoorGroup dg;
                                dg.voxels = std::move(grp_voxels);
                                animating_doors.push_back(std::move(dg));
                                audio.play("click", glm::vec3(fhit.voxel) + glm::vec3(0.5f));
                                SDL_Log("Door: opening at (%d,%d,%d)",
                                        fhit.voxel.x, fhit.voxel.y, fhit.voxel.z);
                            }
                        // ── Close door ───────────────────────────────────
                        } else if (door_open_type_id != 0
                                   && hit_v.type_id == door_open_type_id
                                   && (hand_empty || has_id)) {
                            auto grp_voxels = flood_fill_door(fhit.voxel, door_open_type_id);
                            if (!grp_voxels.empty() && door_anim_type_id != 0) {
                                // Player hitbox check: feet at cam_pos - (0,0.5,0),
                                // AABB half-extents: radius=0.3 in XZ, height=0.9 in Y.
                                constexpr float PLR_R = 0.3f;
                                constexpr float PLR_H = 0.9f;
                                glm::vec3 feet = cam_pos - glm::vec3(0.f, 0.5f, 0.f);
                                glm::vec3 pmin = feet + glm::vec3(-PLR_R, 0.f,   -PLR_R);
                                glm::vec3 pmax = feet + glm::vec3( PLR_R, PLR_H,  PLR_R);

                                bool player_inside = false;
                                for (auto& p : grp_voxels) {
                                    // Cell AABB is [p, p+1]
                                    glm::vec3 cmin(p.x, p.y, p.z);
                                    glm::vec3 cmax(p.x+1.f, p.y+1.f, p.z+1.f);
                                    if (pmin.x < cmax.x && pmax.x > cmin.x &&
                                        pmin.y < cmax.y && pmax.y > cmin.y &&
                                        pmin.z < cmax.z && pmax.z > cmin.z) {
                                        player_inside = true;
                                        break;
                                    }
                                }

                                if (!player_inside) {
                                    // Play closing animation backwards
                                    Voxel anim_v;
                                    anim_v.type_id = door_anim_type_id;
                                    anim_v.flags   = VFLAG_SOLID | VFLAG_VERT_PLANE_Z;
                                    for (auto& p : grp_voxels)
                                        server.world().set_voxel(p, anim_v);
                                    for (Chunk* c : server.world().dirty_chunks())
                                        mesher.enqueue(c->chunk_pos(), server.world());
                                    // Door panels are now solid — trigger an immediate zone
                                    // split so the sealed side stops losing gas to space
                                    // even before the closing animation completes.
                                    server.atmos().on_door_changed(fhit.voxel);
                                    DoorGroup dg;
                                    dg.voxels  = std::move(grp_voxels);
                                    dg.frame   = renderer.door_anim_frame_count() - 1;
                                    dg.closing = true;
                                    animating_doors.push_back(std::move(dg));
                                    audio.play("click", glm::vec3(fhit.voxel) + glm::vec3(0.5f));
                                    SDL_Log("Door: closing at (%d,%d,%d)",
                                            fhit.voxel.x, fhit.voxel.y, fhit.voxel.z);
                                } else {
                                    SDL_Log("Door: cannot close — player is in doorway");
                                }
                            }
                        } else if (!hand_empty) {
                            // Use held item on turf → knock
                            glm::vec3 hit_world = glm::vec3(fhit.voxel) + glm::vec3(0.5f);
                            SDL_Log("Knock: %s on wall",
                                    active_slot->item->def
                                        ? active_slot->item->def->name.c_str() : "item");
                            audio.play("knock", hit_world);
                        }
                    }
                }  // end if (fps_lmb || e_press)
            }  // end interaction block

            // ── FPS RMB on world item → scrollable context menu ───────────────
            if (!alt_mode.active() && !build_mode && !ctx_menu.is_open()
                && input.is_pressed(Action::SecondaryInteract)) {
                constexpr float CTX_REACH = 5.f;
                float yr2 = glm::radians(cam_yaw), pr2 = glm::radians(cam_pitch);
                glm::vec3 rdir2 = {
                    std::cos(pr2) * std::sin(yr2),
                    std::sin(pr2),
                   -std::cos(pr2) * std::cos(yr2)
                };
                RayHit fhit2 = server.world().raycast(cam_pos, rdir2, CTX_REACH);
                float ict_dist = 0.f;
                EntityID ctx_ent = world_items.ray_cast_items(
                    cam_pos, rdir2, CTX_REACH,
                    fhit2.valid ? fhit2.distance : CTX_REACH, ict_dist);
                SDL_Log("CTX RMB: alt=%d build=%d open=%d ent=%u dist=%.2f",
                    (int)alt_mode.active(), (int)build_mode, (int)ctx_menu.is_open(),
                    (unsigned)ctx_ent, ict_dist);

                if (ctx_ent != NULL_ENTITY) {
                    auto* wic = server.entities().get_component<WorldItemComponent>(ctx_ent);
                    if (wic && wic->item.def) {
                        std::vector<ContextEntry> entries;
                        EntityID eid = ctx_ent;

                        // Pick Up
                        entries.push_back({"Pick Up", true, false,
                            [&player_inv, &world_items, eid, &cam_pos]() {
                                auto picked = world_items.pick_up(eid);
                                if (picked && picked->def) {
                                    auto leftover = player_inv.auto_equip(std::move(*picked));
                                    if (leftover)
                                        world_items.spawn_floating(cam_pos, std::move(*leftover));
                                }
                            }});

                        // Examine
                        {
                            const ItemDef* idef = wic->item.def;
                            std::string item_name  = idef->name;
                            float item_w    = idef->weight;
                            float item_v    = idef->volume;
                            int   item_cnt  = wic->item.count;
                            float item_intg = wic->item.integrity;
                            std::string cust = wic->item.custom_name;
                            entries.push_back({"Examine", true, false,
                                [&hud_state, item_name, item_w, item_v,
                                 item_cnt, item_intg, cust]() {
                                    std::string display = cust.empty()
                                        ? item_name : item_name + ' ' + '\"' + cust + '\"';
                                    if (item_cnt > 1) display += " x" + std::to_string(item_cnt);
                                    char buf[160];
                                    std::snprintf(buf, sizeof(buf),
                                        "[Examine] %s \xe2\x80\x94 %.2f kg / %.1f L \xe2\x80\x94 %s",
                                        display.c_str(), item_w, item_v,
                                        condition_label(item_intg));
                                    if (hud_state.radio_log.size() >= 30)
                                        hud_state.radio_log.pop_front();
                                    hud_state.radio_log.push_back(buf);
                                }});
                        }

                        // Separator + item-specific verbs
                        if (!wic->item.def->verbs.empty()) {
                            entries.push_back({"", false, true, nullptr}); // separator
                            for (const auto& verb : wic->item.def->verbs)
                                entries.push_back({verb.name, true, false,
                                    [name = verb.name]() { SDL_Log("Verb: %s", name.c_str()); }});
                        }

                        // Open menu at screen center (FPS — no cursor yet)
                        glm::vec2 menu_pos = {
                            renderer.width()  * 0.5f - 86.f,
                            renderer.height() * 0.5f - 20.f
                        };
                        SDL_Log("CTX: opening for ent=%u n=%d pos=(%.0f,%.0f)",
                            (unsigned)ctx_ent, (int)entries.size(), menu_pos.x, menu_pos.y);
                        ctx_menu.open(menu_pos, std::move(entries));
                        fps_ctx_rclick      = true;
                        fps_ctx_just_opened = true;
                        fps_ctx_entity      = ctx_ent;
                        SDL_Log("CTX: is_open=%d", (int)ctx_menu.is_open());
                    }
                }
            }

            // ── Alt-mode LMB on world item ───────────────────────────────────
            // Rising-edge press: begin a panel world-drag so the player can
            // drag the item into any inventory slot, or start an item-on-item
            // interaction when holding something.
            if (alt_mode.active() && input.is_pressed(Action::PrimaryInteract)
                && hovered_item_entity != NULL_ENTITY) {
                auto*  active_slot = player_inv.active_hand();
                bool   hand_empty  = !active_slot || !active_slot->item;
                if (hand_empty) {
                    // Start world drag: show ghost item following cursor;
                    // the panel will place it on drop.  Don't pick up yet.
                    auto* wic = server.entities().get_component<WorldItemComponent>(hovered_item_entity);
                    if (wic && wic->item.def && !inv_panel.is_dragging()) {
                        inv_panel.begin_world_drag(wic->item, alt_mode.cursor_pos());
                        alt_drag_entity = hovered_item_entity;
                    }
                } else {
                    // Use held item on hovered world item
                    auto* wic = server.entities().get_component<WorldItemComponent>(hovered_item_entity);
                    if (wic && wic->item.def && active_slot->item->def) {
                        SDL_Log("Interact: %s on %s",
                                active_slot->item->def->name.c_str(),
                                wic->item.def->name.c_str());
                        audio.play("click", cam_pos);
                    }
                }
            }

            // ── Build mode: LMB destroys, RMB places ─────────────────────────────
            if (build_mode && !alt_mode.active()) {
                float yr = glm::radians(cam_yaw), pr = glm::radians(cam_pitch);
                glm::vec3 rdir = {
                    std::cos(pr) * std::sin(yr),
                    std::sin(pr),
                   -std::cos(pr) * std::cos(yr)
                };
                // LMB: destroy voxel
                if (input.is_pressed(Action::PrimaryInteract)) {
                    RayHit bhit = server.world().raycast(cam_pos, rdir, 8.f);
                    if (bhit.valid) {
                        server.world().set_voxel(bhit.voxel, Voxel{});
                        for (Chunk* c : server.world().dirty_chunks())
                            mesher.enqueue(c->chunk_pos(), server.world());
                        server.atmos().on_voxel_changed(bhit.voxel);
                        // Propagate lighting change from removed voxel.
                        lighting.update({bhit.voxel});
                        for (Chunk* c : server.world().dirty_chunks())
                            mesher.enqueue(c->chunk_pos(), server.world());
                        SDL_Log("Build: destroyed voxel at (%d,%d,%d)",
                                bhit.voxel.x, bhit.voxel.y, bhit.voxel.z);
                    }
                }
                // RMB: place voxel on adjacent face
                if (input.is_pressed(Action::SecondaryInteract)) {
                    RayHit bhit = server.world().raycast(cam_pos, rdir, 8.f);
                    if (bhit.valid) {
                        glm::ivec3 place_pos = bhit.voxel + face_normal(bhit.face);
                        if (server.world().get_voxel(place_pos).type_id == 0) {
                            server.world().set_voxel(place_pos, build_voxel);
                            for (Chunk* c : server.world().dirty_chunks())
                                mesher.enqueue(c->chunk_pos(), server.world());
                            server.atmos().on_voxel_changed(place_pos);
                            // Propagate lighting change from placed voxel.
                            lighting.update({place_pos});
                            for (Chunk* c : server.world().dirty_chunks())
                                mesher.enqueue(c->chunk_pos(), server.world());
                            SDL_Log("Build: placed voxel at (%d,%d,%d)",
                                    place_pos.x, place_pos.y, place_pos.z);
                        }
                    }
                }
            }

            if (input.is_pressed(Action::Escape)) {
                SDL_SetWindowRelativeMouseMode(renderer.window(), false);
            }
        },

        // Variable-rate render
        [&](double alpha) {
            client.interpolate(alpha);

            // ── Camera basis + view-projection (mirrors draw_world) ──────────
            float yaw_r   = glm::radians(cam_yaw);
            float pitch_r = glm::radians(cam_pitch);
            glm::vec3 ray_dir = {
                std::cos(pitch_r) * std::sin(yaw_r),
                std::sin(pitch_r),
               -std::cos(pitch_r) * std::cos(yaw_r)
            };
            glm::mat4 vp_mat = glm::perspective(
                glm::radians(90.f),
                renderer.height() > 0 ? float(renderer.width()) / renderer.height() : 1.f,
                0.1f, 400.f)
                * glm::lookAt(cam_pos, cam_pos + ray_dir, {0.f, 1.f, 0.f});

            // ── Voxel face ray cast ──────────────────────────────────────────
            RayHit hit = server.world().raycast(cam_pos, ray_dir, 4.f);
            // Restrict turf selection to voxels adjacent (Chebyshev ≤ 1) to the
            // player's current voxel — includes all 26 immediate neighbours.
            if (hit.valid) {
                glm::vec3 feet = cam_pos - glm::vec3(0.f, 0.5f, 0.f);
                glm::ivec3 pv = {
                    static_cast<int>(std::floor(feet.x)),
                    static_cast<int>(std::floor(feet.y)),
                    static_cast<int>(std::floor(feet.z))
                };
                if (std::abs(hit.voxel.x - pv.x) > 1 ||
                    std::abs(hit.voxel.y - pv.y) > 1 ||
                    std::abs(hit.voxel.z - pv.z) > 1)
                    hit.valid = false;
            }

            // ── World item hovering ───────────────────────────────────────────
            constexpr float ITEM_REACH = 1.5f;
            {
                std::vector<EntityID> new_candidates;
                if (alt_mode.active()) {
                    new_candidates = world_items.screen_hover_all(
                        alt_mode.cursor_pos(), vp_mat,
                        renderer.width(), renderer.height());
                    // Distance-cap each candidate
                    new_candidates.erase(
                        std::remove_if(new_candidates.begin(), new_candidates.end(),
                            [&](EntityID eid) {
                                auto* tr = server.entities().get_component<TransformComponent>(eid);
                                return !tr || glm::length(tr->pos - cam_pos) > ITEM_REACH;
                            }),
                        new_candidates.end());
                } else {
                    new_candidates = world_items.ray_cast_items_all(
                        cam_pos, ray_dir, ITEM_REACH,
                        hit.valid ? hit.distance : ITEM_REACH);
                }
                // Reset scroll index if the candidate set changed identity
                if (new_candidates != item_candidates) {
                    item_candidates = std::move(new_candidates);
                    scroll_item_idx = 0;
                } else {
                    item_candidates = std::move(new_candidates);
                }
                int n = static_cast<int>(item_candidates.size());
                if (n == 0) {
                    hovered_item_entity = NULL_ENTITY;
                    scroll_item_idx     = 0;
                } else {
                    scroll_item_idx     = ((scroll_item_idx % n) + n) % n;
                    hovered_item_entity = item_candidates[scroll_item_idx];
                }
            }

            // ── Queue geometry for GPU upload (must be before begin_frame) ───
            renderer.queue_highlight(hit);
            renderer.queue_world_items(server.entities(), hovered_item_entity,
                                       cam_pos, cam_yaw, cam_pitch);

            // ── Rebuild clothing + inhand overlays for all human mobs ─────────
            {
                // item_id → worn clothing overlay dir/name
                struct CEntry { const char* id; const char* dir; const char* name; };
                static const CEntry k_clothing_map[] = {
                    // Suits
                    { "hardsuit",        "clothing/suits/spacesuit", "space"           },
                    { "body_armor",      "clothing/suits/armor",     "armor"           },
                    { "security_armor",  "clothing/suits/armor",     "armor_sec"       },
                    // Head
                    { "hardsuit_helmet", "clothing/head/spacehelm",  "space"           },
                    { "mining_helmet",   "clothing/head/utility",    "hardhat0_orange"  },
                    { "welding_helmet",  "clothing/head/utility",    "welding"         },
                    { "hard_hat",        "clothing/head/utility",    "hardhat0_yellow"  },
                    // Eyes
                    { "welding_goggles", "clothing/eyes",            "welding-g"       },
                    { "sunglasses",      "clothing/eyes",            "bigsunglasses"   },
                    // Mask
                    { "gas_mask",        "clothing/mask",            "gas_mask"        },
                    // Back
                    { "jetpack",         "clothing/back",            "jetpack"         },
                    // Belt
                    { "toolbelt",        "clothing/belt",            "ebelt"           },
                    // Gloves
                    { "rubber_gloves",   "clothing/hands",           "latex"           },
                    // Feet
                    { "magboots",        "clothing/feet",            "magboots0"       },
                    { "boots",           "clothing/feet",            "workboots"       },
                    // Ears
                    { "headset",         "clothing/ears",            "headset"         },
                };

                // item_id → inhand sprite category/name
                struct IEntry { const char* id; const char* category; const char* name; };
                static const IEntry k_inhand_map[] = {
                    { "wrench",       "tools",   "wrench"      },
                    { "screwdriver",  "tools",   "screwdriver" },
                    { "crowbar",      "tools",   "crowbar"     },
                    { "wirecutters",  "tools",   "cutters"     },
                    { "welder",       "tools",   "welder"      },
                    { "multitool",    "tools",   "multitool"   },
                    { "drill",        "tools",   "drill"       },
                    { "rcd",          "tools",   "rcd"         },
                    { "pickaxe",      "mining",  "pickaxe"     },
                    { "shovel",       "mining",  "shovel"      },
                    { "flashlight",   "devices", "flashlight"  },
                    { "radio",        "devices", "radio"       },
                    { "baseball_bat", "melee",   "baseball_bat"},
                    { "katana",       "swords",  "katana"      },
                    { "bow",          "bows",    "bow"         },
                    { "beaker",       "items",   "beaker"      },
                    { "medipen",      "medical", "medipen"     },
                    { "syringe",      "medical", "syringe_0"   },
                    { "scalpel",      "medical", "scalpel"     },
                };

                static const char* k_vis_eq_slots[] = {
                    "suit","head","glasses","mask","back","belt","gloves","shoes","ears"
                };

                // Helper: rebuild Clothing/Inhand layers in `app` when the
                // inventory fingerprint changes.  Bodypart layers are preserved.
                auto rebuild_if_changed = [&](HumanAppearance& app,
                                              const Inventory&  inv,
                                              std::string&      last_fp) {
                    std::string fp;
                    for (const char* sl : k_vis_eq_slots) {
                        const auto* slt = inv.find_slot(sl);
                        if (slt && slt->item && slt->item->def)
                            fp += sl + std::string("=") + slt->item->def->id + ";";
                    }
                    for (const char* sl : { "l_hand", "r_hand" }) {
                        const auto* slt = inv.find_slot(sl);
                        if (slt && slt->item && slt->item->def)
                            fp += sl + std::string("=") + slt->item->def->id + ";";
                    }
                    if (fp == last_fp) return;
                    last_fp = fp;

                    // Keep Bodypart layers; drop old Clothing/Inhand
                    std::vector<HumanOverlay> new_layers;
                    new_layers.reserve(app.layers.size() + 10);
                    for (const auto& ov : app.layers)
                        if (ov.kind == HumanOverlayKind::Bodypart)
                            new_layers.push_back(ov);

                    // Clothing overlays (equipment slots, drawn bottom-to-top)
                    for (const char* sl : k_vis_eq_slots) {
                        const auto* slt = inv.find_slot(sl);
                        if (!slt || !slt->item || !slt->item->def) continue;
                        for (const auto& ce : k_clothing_map) {
                            if (slt->item->def->id == ce.id) {
                                HumanOverlay ov;
                                ov.kind       = HumanOverlayKind::Clothing;
                                ov.sprite_dir = ce.dir;
                                ov.prefix     = ce.name;
                                new_layers.push_back(ov);
                                break;
                            }
                        }
                    }

                    // Inhand overlays for left and right hand slots
                    auto add_inhand = [&](const char* slot_id, bool right) {
                        const auto* slt = inv.find_slot(slot_id);
                        if (!slt || !slt->item || !slt->item->def) return;
                        for (const auto& ie : k_inhand_map) {
                            if (slt->item->def->id == ie.id) {
                                HumanOverlay ov;
                                ov.kind       = HumanOverlayKind::Inhand;
                                ov.sprite_dir = std::string("inhands/")
                                              + ie.category
                                              + (right ? "_righthand" : "_lefthand");
                                ov.prefix     = ie.name;
                                new_layers.push_back(ov);
                                break;
                            }
                        }
                    };
                    add_inhand("l_hand", false);
                    add_inhand("r_hand", true);

                    app.layers = std::move(new_layers);
                    app.dirty  = true;
                };

                // ── Local player ──────────────────────────────────────────────
                EntityID player = client.local_player();
                if (player != NULL_ENTITY) {
                    auto* app = server.entities().get_component<HumanAppearance>(player);
                    if (app) {
                        static std::string s_last_app_fp;
                        rebuild_if_changed(*app, player_inv, s_last_app_fp);
                    }
                }

                // ── All other mobs with InventoryComponent ────────────────────
                server.entities().each<InventoryComponent>(
                    [&](EntityID eid, InventoryComponent& ic) {
                        if (eid == client.local_player()) return; // handled above
                        auto* app = server.entities().get_component<HumanAppearance>(eid);
                        if (!app) return;
                        rebuild_if_changed(*app, ic.inv, ic.overlay_fp);
                    });
            }

            renderer.queue_mobs(server.entities(), cam_pos, cam_yaw, client.local_player());
            for (const auto& obj : model_objs.objects())
                renderer.queue_model(obj.name.c_str(), obj.world_pos, obj.yaw, obj.scale);
            renderer.queue_earth_background(cam_pos, cam_yaw, cam_pitch);

            renderer.begin_frame(alpha);

            // Push lighting state to renderer
            renderer.set_fullbright(fullbright_enabled);
            renderer.set_ao_mix(ao_enabled ? 1.0f : 0.0f);
            renderer.set_ambient(0.05f);  // dark station; light tubes provide illumination

            // Dynamic point light: follows the player when a flashlight item is held
            {
                bool holds_flashlight = false;
                for (const char* sl : { "l_hand", "r_hand" }) {
                    const auto* slot = player_inv.find_slot(sl);
                    if (slot && slot->item && slot->item->def &&
                        slot->item->def->id == "flashlight") {
                        holds_flashlight = true;
                        break;
                    }
                }
                static bool      s_last_held      = false;
                static glm::ivec3 s_last_light_pos = {999999, 999999, 999999};
                glm::ivec3 pv = glm::ivec3(glm::floor(cam_pos));
                EntityID plr = client.local_player();
                EntityID src = plr ? plr : static_cast<EntityID>(1u);
                if (holds_flashlight) {
                    if (!s_last_held || pv != s_last_light_pos) {
                        lighting.add_dynamic_light(pv, 12, src, {255, 200, 140});
                        s_last_light_pos = pv;
                        // Re-mesh chunks whose light_levels changed.
                        for (Chunk* c : server.world().dirty_chunks())
                            mesher.enqueue(c->chunk_pos(), server.world());
                    }
                } else if (s_last_held) {
                    lighting.remove_dynamic_light(src);
                    // Re-mesh to restore steady-state static lighting.
                    for (Chunk* c : server.world().dirty_chunks())
                        mesher.enqueue(c->chunk_pos(), server.world());
                }
                s_last_held = holds_flashlight;
            }

            renderer.draw_world(server.world(), cam_pos, cam_yaw, cam_pitch);
            renderer.draw_world_items();
            renderer.draw_mobs();
            renderer.draw_models();
            renderer.draw_viewmodel(0); // TODO: held item type id

            // UI pass
            ui_renderer.begin();

            // ── World item name labels (projected to screen) ──────────────────
            {
                auto labels = world_items.build_labels(
                    renderer.view_proj(),
                    renderer.width(), renderer.height(),
                    hovered_item_entity);
                for (const auto& lbl : labels) {
                    // Only show tooltip for the item currently being hovered
                    if (!lbl.in_front || !lbl.hovered) continue;

                    constexpr float ICON_SZ  = 20.f;
                    constexpr float ICON_GAP = 4.f;   // gap between icon and text

                    SDL_GPUTexture* lbl_icon = ui_renderer.item_icon(lbl.item_id);
                    float icon_strip = lbl_icon ? (ICON_SZ + ICON_GAP) : 0.f;

                    float text_w = static_cast<float>(lbl.name.size() * 7);
                    float pill_w = icon_strip + text_w + 14.f;
                    float pill_h = 22.f;

                    glm::vec2 pill_pos = lbl.screen_pos - glm::vec2(4.f + icon_strip, 4.f);
                    ui_renderer.rect(pill_pos, {pill_w, pill_h},
                                     {0.f, 0.f, 0.f, 0.62f}, 4.f);

                    if (lbl_icon) {
                        glm::vec2 ico_pos = {pill_pos.x + 3.f,
                                             pill_pos.y + (pill_h - ICON_SZ) * 0.5f};
                        ui_renderer.image(ico_pos, {ICON_SZ, ICON_SZ}, lbl_icon, 1.f);
                    }

                    ui_renderer.text(lbl.screen_pos, lbl.name,
                                     {1.f, 1.f, 0.4f, 1.f}, 14.f);
                }
            }

            // Always-on HUD
            {
                // ── Intent keyboard shortcuts: 1=Help 2=Disarm 3=Grab 4=Harm ──
                {
                    const bool* ks_hud = SDL_GetKeyboardState(nullptr);
                    static bool s_1p = false, s_2p = false, s_3p = false, s_4p = false;
                    bool k1 = ks_hud[SDL_SCANCODE_1], k2 = ks_hud[SDL_SCANCODE_2];
                    bool k3 = ks_hud[SDL_SCANCODE_3], k4 = ks_hud[SDL_SCANCODE_4];
                    if (k1 && !s_1p) hud_state.intent = Intent::Help;
                    if (k2 && !s_2p) hud_state.intent = Intent::Disarm;
                    if (k3 && !s_3p) hud_state.intent = Intent::Grab;
                    if (k4 && !s_4p) hud_state.intent = Intent::Harm;
                    s_1p = k1; s_2p = k2; s_3p = k3; s_4p = k4;
                }

                // HUD button clicks only work when cursor is free (alt-mode / menus)
                bool cursor_free = alt_mode.active()
                                || creative_menu.is_open()
                                || map_editor.is_open()
                                || admin_menu.is_open();
                glm::vec2 hud_mouse = cursor_free
                    ? (alt_mode.active() ? alt_mode.cursor_pos() : input.mouse_pos())
                    : glm::vec2{-9999.f, -9999.f};
                bool hud_click = cursor_free && input.is_pressed(Action::PrimaryInteract);
                {
                    std::string hud_clicked = hud.draw(hud_state, player_inv, hud_mouse, hud_click);
                    if (!hud_clicked.empty())
                        player_inv.swap(hud_clicked, player_inv.active_hand_id());
                }
            }

            // ── Crosshair ─────────────────────────────────────────────────
            if (!creative_menu.is_open() && !map_editor.is_open()) {
                const float cx = static_cast<float>(ui_renderer.fb_width())  * 0.5f;
                const float cy = static_cast<float>(ui_renderer.fb_height()) * 0.5f;
                constexpr float HALF = 6.f;   // half-size of the outer square
                constexpr float T    = 2.f;   // border thickness
                constexpr glm::vec4 CR_COL = {1.f, 1.f, 1.f, 0.5f};
                // top
                ui_renderer.rect({cx - HALF, cy - HALF},         {HALF * 2.f, T},    CR_COL, 0.f);
                // bottom
                ui_renderer.rect({cx - HALF, cy + HALF - T},     {HALF * 2.f, T},    CR_COL, 0.f);
                // left
                ui_renderer.rect({cx - HALF, cy - HALF},         {T, HALF * 2.f},    CR_COL, 0.f);
                // right
                ui_renderer.rect({cx + HALF - T, cy - HALF},     {T, HALF * 2.f},    CR_COL, 0.f);
            }

            // ── Debug overlay (F5) ────────────────────────────────────────────
            if (debug_overlay_visible) {
                DebugOverlayState dbg;
                dbg.fps      = loop.fps();
                dbg.frame_ms = (loop.fps() > 0.0) ? 1000.0 / loop.fps() : 0.0;
                dbg.tick_count = loop.tick_count();
                dbg.cam_pos  = cam_pos;
                dbg.yaw      = cam_yaw;
                dbg.pitch    = cam_pitch;

                // Velocity from physics component
                {
                    EntityID plr = client.local_player();
                    if (plr != NULL_ENTITY) {
                        auto* vc = server.entities().get_component<VelocityComponent>(plr);
                        if (vc) dbg.velocity = vc->linear;
                        auto* cc = server.entities().get_component<CharacterControllerComponent>(plr);
                        if (cc) {
                            dbg.noclip    = cc->noclip;
                            dbg.zero_g    = cc->zero_g;
                            dbg.on_ground = cc->on_ground;
                        }
                    }
                }

                // Target voxel
                dbg.has_hit = hit.valid;
                if (hit.valid) {
                    dbg.hit_voxel = hit.voxel;
                    Voxel hv = server.world().get_voxel(hit.voxel);
                    const VoxelTypeDef* hvd = voxel_reg.get(hv.type_id);
                    dbg.hit_type_name = hvd ? hvd->id : std::to_string(hv.type_id);
                }

                // Build mode
                dbg.build_mode = build_mode;
                if (build_mode) {
                    const VoxelTypeDef* bvd = voxel_reg.get(build_voxel.type_id);
                    dbg.build_type_name = bvd ? bvd->id : std::to_string(build_voxel.type_id);
                }

                // Active hand
                dbg.active_hand = (player_inv.active_hand_id() == "l_hand") ? "Left" : "Right";

                // Atmosphere at player position
                {
                    glm::ivec3 feet_voxel = {
                        static_cast<int>(std::floor(cam_pos.x)),
                        static_cast<int>(std::floor(cam_pos.y - 0.5f)),
                        static_cast<int>(std::floor(cam_pos.z))
                    };
                    dbg.zone_id = server.atmos().zone_at(feet_voxel);
                    AtmosZone* zone_ptr = server.atmos().zone(dbg.zone_id);
                    if (zone_ptr) {
                        dbg.gas_mix            = zone_ptr->gas;
                        dbg.room_cell_count    = zone_ptr->cell_count;
                        dbg.room_adj_count     = static_cast<int>(zone_ptr->adjacent_zones.size());
                        dbg.atmos_status       = zone_ptr->status;
                        dbg.pressure_loss_rate = zone_ptr->pressure_loss_rate;
                    }
                    dbg.total_rooms = server.atmos().total_rooms();

                    // Enclosure — query the air cell at feet-level
                    // Only run if the cell is actually air (avoids BFS from solid)
                    glm::ivec3 air_cell = {
                        static_cast<int>(std::floor(cam_pos.x)),
                        static_cast<int>(std::floor(cam_pos.y)),
                        static_cast<int>(std::floor(cam_pos.z))
                    };
                    dbg.enclosed = enclosure_detector.is_enclosed(air_cell);
                }

                debug_overlay.draw(dbg);
            }

            // ── Gas overlay (F4) ─────────────────────────────────────────────
            if (gas_overlay_visible) {
                GasOverlayState gs;
                gs.view_proj = vp_mat;
                gs.cam_pos   = cam_pos;
                gs.fb_w      = renderer.width();
                gs.fb_h      = renderer.height();
                gas_overlay.draw(server.atmos(), gs, sim_time);
            }

            // ── Player stats overlay (F6) ─────────────────────────────────────
            if (player_stats_visible) {
                PlayerStatsOverlayState ps;
                EntityID plr = client.local_player();

                // Species / variant from MobComponent if present, else defaults
                if (plr != NULL_ENTITY) {
                    auto* mc = server.entities().get_component<MobComponent>(plr);
                    if (mc) { ps.species = mc->species; ps.variant = mc->variant; }
                }

                // Health
                if (plr != NULL_ENTITY) {
                    auto* hp = server.entities().get_component<HealthComponent>(plr);
                    if (hp) {
                        ps.health_max = hp->health_max;
                        ps.dmg_brute  = hp->brute;
                        ps.dmg_burn   = hp->burn;
                        ps.dmg_tox    = hp->tox;
                        ps.dmg_oxy    = hp->oxy;
                        ps.dead       = hp->dead;
                    }
                }

                // Suit sensors — copy from already-computed HUD state
                ps.oxy_sat           = hud_state.oxy_sat;
                ps.suit_pressure_kpa = hud_state.suit_pressure_kpa;
                ps.tox_level         = hud_state.tox_level;
                ps.suit_temp_str     = hud_state.suit_temp_str;

                // Controller
                if (plr != NULL_ENTITY) {
                    auto* cc = server.entities().get_component<CharacterControllerComponent>(plr);
                    if (cc) {
                        ps.move_speed       = cc->move_speed;
                        ps.sprint_mult      = cc->sprint_mult;
                        ps.jump_vel         = cc->jump_vel;
                        ps.height           = cc->height;
                        ps.radius           = cc->radius;
                        ps.on_ground        = cc->on_ground;
                        ps.sprinting        = cc->sprinting;
                        ps.zero_g           = cc->zero_g;
                        ps.noclip           = cc->noclip;
                        ps.jetpack_equipped = cc->jetpack_equipped;
                        ps.grab_wall        = cc->grab_wall;
                    }
                }

                // Active hand item
                ps.active_hand_id = player_inv.active_hand_id();
                const auto* hand_slot = player_inv.active_hand();
                if (hand_slot && hand_slot->item && hand_slot->item->def)
                    ps.active_hand_name = hand_slot->item->def->name;

                player_stats_overlay.draw(ps);
            }

            // Alt-mode overlay
            float ov_alpha = alt_mode.overlay_alpha();
            if (ov_alpha > 0.01f) {
                bool rmb_pressed  = input.is_pressed (Action::SecondaryInteract);
                // ── World drag completion / cancellation ──────────────────────
                if (alt_drag_entity != NULL_ENTITY) {
                    alt_drag_entity = NULL_ENTITY;
                }

                // TODO: RMB on HUD inventory slot → item verb context menu

                // Also open ctx menu on right-click on a world face in alt-mode
                if (rmb_pressed && alt_mode.active() && hit.valid) {
                    (void)hit; // TODO: populate verb list from voxel type
                }
            } else {
                // Only auto-close if the menu was opened from alt-mode (not FPS RMB)
                if (!fps_ctx_rclick)
                    ctx_menu.close();
            }

            // Consume scroll once per frame so both the context menu and the
            // creative menu can use the same value without either zeroing it first.
            float frame_scroll = input.consume_scroll();

            {
                bool was_ctx_open = ctx_menu.is_open();
                // Suppress all dismiss signals on the exact frame the menu opens
                bool suppress = fps_ctx_just_opened;
                fps_ctx_just_opened = false;
                // In FPS mode cursor is captured — pass offscreen pos so
                // hover and click-outside logic never fires.
                // Dismiss via Escape (not RMB — that's what opened the menu).
                glm::vec2 ctx_cursor = fps_ctx_rclick
                    ? glm::vec2{-9999.f, -9999.f}
                    : input.mouse_pos();
                bool dismiss = fps_ctx_rclick
                    ? input.is_pressed(Action::Escape)
                    : (suppress ? false : input.is_pressed(Action::SecondaryInteract));
                // Clamp scroll to ±1 step per frame so the menu doesn't fly past entries
                float raw_scroll = frame_scroll;
                float clamped_scroll = (raw_scroll > 0.f) ? 1.f : (raw_scroll < 0.f) ? -1.f : 0.f;
                ctx_menu.draw(
                    ctx_cursor,
                    (fps_ctx_rclick || suppress) ? false : input.is_pressed(Action::PrimaryInteract),
                    clamped_scroll,
                    suppress ? false : input.is_pressed(Action::PickUp),  // E = confirm
                    dismiss);
                if (fps_ctx_rclick && was_ctx_open && !ctx_menu.is_open()) {
                    fps_ctx_rclick = false;
                    fps_ctx_entity = NULL_ENTITY;
                }
                // Look-away dismiss — checked AFTER draw so the menu always
                // renders for at least one frame before being auto-closed.
                // Also skip on the opening frame (suppress) to avoid immediate closure.
                if (fps_ctx_rclick && ctx_menu.is_open() && !suppress) {
                    constexpr float CTX_REACH2 = 5.f;
                    float yrc = glm::radians(cam_yaw), prc = glm::radians(cam_pitch);
                    glm::vec3 rdc = {
                        std::cos(prc) * std::sin(yrc),
                        std::sin(prc),
                       -std::cos(prc) * std::cos(yrc)
                    };
                    RayHit fhc = server.world().raycast(cam_pos, rdc, CTX_REACH2);
                    float idc = 0.f;
                    EntityID hit_ent = world_items.ray_cast_items(
                        cam_pos, rdc, CTX_REACH2,
                        fhc.valid ? fhc.distance : CTX_REACH2, idc);
                    if (hit_ent != fps_ctx_entity) {
                        ctx_menu.close();
                        fps_ctx_rclick = false;
                        fps_ctx_entity = NULL_ENTITY;
                    }
                }
            }

            // ── Admin menu (F1) ───────────────────────────────────────────────
            if (admin_menu.is_open()) {
                AdminMenuState adm_state;
                {
                    EntityID plr = client.local_player();
                    if (plr != NULL_ENTITY) {
                        auto* cc = server.entities().get_component<CharacterControllerComponent>(plr);
                        if (cc) adm_state.noclip = cc->noclip;
                    }
                }
                adm_state.build_mode    = build_mode;
                adm_state.gas_overlay   = gas_overlay_visible;
                adm_state.debug_overlay = debug_overlay_visible;
                adm_state.player_stats  = player_stats_visible;
                adm_state.verbose_log   = renderer.verbose_logging();
                adm_state.fullbright    = fullbright_enabled;
                adm_state.ambient_occlusion = ao_enabled;

                bool esc_adm = input.is_pressed(Action::Escape);
                auto ar = admin_menu.draw(input.mouse_pos(),
                                          input.consume_press(Action::PrimaryInteract),
                                          esc_adm,
                                          adm_state);

                if (ar.toggle_noclip) {
                    EntityID plr = client.local_player();
                    if (plr != NULL_ENTITY) {
                        auto* cc = server.entities().get_component<CharacterControllerComponent>(plr);
                        if (cc) {
                            cc->noclip = !cc->noclip;
                            SDL_Log("Noclip: %s", cc->noclip ? "ON" : "OFF");
                        }
                    }
                }
                if (ar.toggle_build_mode) {
                    build_mode = !build_mode;
                    SDL_Log("Build mode: %s", build_mode ? "ON" : "OFF");
                }
                if (ar.toggle_gas_overlay) {
                    gas_overlay_visible = !gas_overlay_visible;
                    SDL_Log("Gas overlay: %s", gas_overlay_visible ? "ON" : "OFF");
                }
                if (ar.toggle_debug_overlay) {
                    debug_overlay_visible = !debug_overlay_visible;
                    SDL_Log("Debug overlay: %s", debug_overlay_visible ? "ON" : "OFF");
                }
                if (ar.toggle_player_stats) {
                    player_stats_visible = !player_stats_visible;
                    SDL_Log("Player stats: %s", player_stats_visible ? "ON" : "OFF");
                }
                if (ar.toggle_verbose_log) {
                    renderer.toggle_verbose_logging();
                }
                if (ar.toggle_fullbright) {
                    fullbright_enabled = !fullbright_enabled;
                    SDL_Log("Fullbright: %s", fullbright_enabled ? "ON" : "OFF");
                }
                if (ar.toggle_ambient_occlusion) {
                    ao_enabled = !ao_enabled;
                    SDL_Log("Ambient Occlusion: %s", ao_enabled ? "ON" : "OFF");
                }
                if (ar.close_requested && !alt_mode.active() &&
                    !creative_menu.is_open() && !map_editor.is_open())
                    input.capture_cursor(renderer.window(), true);
            }

            // ── Creative menu (F2) ────────────────────────────────────────────
            if (creative_menu.is_open()) {
                bool esc_pressed = input.is_pressed(Action::Escape);
                auto cr = creative_menu.draw(
                    input.mouse_pos(),
                    input.is_pressed(Action::PrimaryInteract),
                    frame_scroll,
                    esc_pressed);
                if (cr.give_item) {
                    ItemStack st;
                    st.def       = cr.give_item;
                    st.count     = 1;
                    st.integrity = 1.f;
                    if (!player_inv.put(player_inv.active_hand_id(), st))
                        world_items.spawn_floating(cam_pos, std::move(st));
                }
                // Recapture cursor once the menu closes (escape, etc.)
                if (!creative_menu.is_open() && !alt_mode.active())
                    input.capture_cursor(renderer.window(), true);
                // Update build voxel type when the player picks one from the menu
                if (cr.place_voxel != 0) {
                    const VoxelTypeDef* vdef = voxel_reg.get(cr.place_voxel);
                    build_voxel.type_id = cr.place_voxel;
                    build_voxel.flags   = vdef ? vdef->default_flags
                                               : static_cast<uint8_t>(VFLAG_SOLID | VFLAG_OPAQUE);
                    if (vdef)
                        SDL_Log("Build voxel set to: %s", vdef->name.c_str());
                }
            }

            // ── Map editor (F7) ────────────────────────────────────────────────
            if (map_editor.is_open()) {
                const bool* ks_me = SDL_GetKeyboardState(nullptr);
                bool ctrl_held = ks_me[SDL_SCANCODE_LCTRL] || ks_me[SDL_SCANCODE_RCTRL];
                bool me_lmb  = input.is_held(Action::PrimaryInteract);
                bool me_rmb  = input.is_held(Action::SecondaryInteract);
                SDL_MouseButtonFlags mbtns = SDL_GetMouseState(nullptr, nullptr);
                bool me_mmb  = (mbtns & SDL_BUTTON_MMASK) != 0;
                static bool s_pgup_prev = false, s_pgdn_prev = false;
                static bool s_cs_prev   = false, s_cl_prev   = false;
                bool pgup_raw = ks_me[SDL_SCANCODE_PAGEUP];
                bool pgdn_raw = ks_me[SDL_SCANCODE_PAGEDOWN];
                bool cs_raw   = ctrl_held && ks_me[SDL_SCANCODE_S];
                bool cl_raw   = ctrl_held && ks_me[SDL_SCANCODE_L];
                bool me_pgup  = pgup_raw && !s_pgup_prev;
                bool me_pgdn  = pgdn_raw && !s_pgdn_prev;
                bool me_cs    = cs_raw   && !s_cs_prev;
                bool me_cl    = cl_raw   && !s_cl_prev;
                s_pgup_prev = pgup_raw;
                s_pgdn_prev = pgdn_raw;
                s_cs_prev   = cs_raw;
                s_cl_prev   = cl_raw;
                auto me_result = map_editor.draw(
                    input.mouse_pos(), me_lmb, me_rmb, me_mmb,
                    frame_scroll, me_pgup, me_pgdn, me_cs, me_cl,
                    input.is_pressed(Action::Escape));
                if (me_result.map_reloaded) {
                    // Discard all pre-reload mesher jobs so stale GPU meshes
                    // are never uploaded after the world is wiped.
                    mesher.flush();
                    // A full world.clear_all() was performed: all old chunks
                    // are gone so the renderer's stale GPU meshes must be
                    // wiped first, then every freshly-loaded chunk re-queued.
                    renderer.clear_all_meshes();

                    // Teleport the player to a safe, non-solid spawn position
                    // to prevent them getting stuck inside a newly-placed wall.
                    EntityID plr_reload = client.local_player();
                    if (plr_reload != NULL_ENTITY) {
                        auto* tr_pl = server.entities().get_component<TransformComponent>(plr_reload);
                        if (tr_pl) {
                            int px = static_cast<int>(std::floor(tr_pl->pos.x));
                            int pz = static_cast<int>(std::floor(tr_pl->pos.z));
                            bool found = false;
                            for (int sy = 1; sy <= 128 && !found; ++sy) {
                                Voxel bot = server.world().get_voxel({px, sy,     pz});
                                Voxel top = server.world().get_voxel({px, sy + 1, pz});
                                if (!(bot.flags & VFLAG_SOLID) && !(top.flags & VFLAG_SOLID)) {
                                    tr_pl->pos = { tr_pl->pos.x, float(sy), tr_pl->pos.z };
                                    auto* vel_pl = server.entities().get_component<VelocityComponent>(plr_reload);
                                    if (vel_pl) vel_pl->linear = {};
                                    found = true;
                                }
                            }
                            if (!found) {
                                // Fallback: just elevate above any potential geometry
                                tr_pl->pos.y = 130.f;
                                auto* vel_pl = server.entities().get_component<VelocityComponent>(plr_reload);
                                if (vel_pl) vel_pl->linear = {};
                            }
                        }
                    }
                }
                if (me_result.world_modified) {
                    for (Chunk* c : server.world().dirty_chunks())
                        mesher.enqueue(c->chunk_pos(), server.world());
                }
                if (me_result.needs_atmos_rebuild)
                    server.atmos().rebuild_zones();
                if (me_result.request_close) {
                    map_editor.close();
                    if (!alt_mode.active())
                        input.capture_cursor(renderer.window(), true);
                }
            }

            // End world pass before UI so swapchain texture is free for the UI pass
            renderer.end_world_pass();

            ui_renderer.end(renderer.cmd_buf(),
                            renderer.swapchain_tex(),
                            renderer.width(),
                            renderer.height());

            renderer.end_frame();
        }
    );

    // ── 16. Cleanup ───────────────────────────────────────────────────────────
    mesher.stop();
    audio.shutdown();
    client.disconnect();
    server.stop();
    ui_renderer.shutdown();
    renderer.shutdown();
    SDL_Quit();
    return 0;
}
