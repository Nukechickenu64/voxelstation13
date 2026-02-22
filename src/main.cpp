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
#include "input/input_manager.h"
#include "input/alt_mode.h"
#include "inventory/inventory.h"
#include "inventory/item_registry.h"
#include "audio/audio_manager.h"
#include "data/voxel_registry.h"
#include "data/data_validator.h"
#include "ui/hud.h"
#include "ui/inventory_panel.h"
#include "ui/context_menu.h"
#include "network/server.h"
#include "network/client.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

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

    ItemRegistry item_reg;
    item_reg.load_directory("data/item_types");

    // ── 3. Renderer + window ──────────────────────────────────────────────────
    Renderer renderer;
    if (!renderer.init("VoxelStation 13", 1280, 720)) {
        SDL_Log("Renderer init failed.");
        return 1;
    }
    renderer.load_tile_textures(voxel_reg, "textures");
    renderer.load_item_textures(item_reg,  "textures");
    renderer.load_mob_textures("textures");

    UIRenderer ui_renderer(renderer.gpu());
    if (!ui_renderer.init(renderer.window(), renderer.width(), renderer.height())) {
        SDL_Log("UIRenderer init failed.");
        return 1;
    }

    // ── 4. Chunk mesher ───────────────────────────────────────────────────────
    ChunkMesher mesher;
    mesher.start(2);

    // ── 5. Server (local single-player) ──────────────────────────────────────
    Server server;
    server.start(0); // port 0 = loopback / no networking

    // ── 6. Client ─────────────────────────────────────────────────────────────
    Client client;
    client.connect_local(server);

    // ── 7. Lighting ───────────────────────────────────────────────────────────
    LightingSystem lighting(server.world());
    lighting.rebuild();

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

    // ── 11. Player inventory ──────────────────────────────────────────────────
    Inventory player_inv = make_player_inventory();

    // ── 11b. World item system ────────────────────────────────────────────────
    WorldItemSystem world_items(server.world(), server.entities());

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

        // Place the player above the floor so they aren't embedded in it
        EntityID player_ent = client.local_player();
        if (player_ent != NULL_ENTITY) {
            auto* tr = server.entities().get_component<TransformComponent>(player_ent);
            if (tr) tr->pos = {0.f, 1.f, 0.f};
        }

        // Enqueue all dirty chunks for meshing
        for (Chunk* c : server.world().dirty_chunks())
            mesher.enqueue(c->chunk_pos(), server.world());

        // Spawn a couple of test items on the floor for M1 testing
        auto spawn_test_item = [&](const char* item_id, int x, int z) {
            const ItemDef* def = item_reg.get(item_id);
            if (!def) return;
            ItemStack stack; stack.def = def; stack.count = 1;
            world_items.spawn({x, 0, z}, FaceDir::PosY, std::move(stack));
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
            MobComponent mob{};
            mob.species = "human";
            mob.variant = "female";
            server.entities().add_component<MobComponent>(dummy, mob);
        }
    }

    // ── 13. HUD state ─────────────────────────────────────────────────────────
    HUDState hud_state;
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

    // ── 15. Game loop ─────────────────────────────────────────────────────────
    GameLoop loop(1.0 / 60.0);

    loop.run(
        // Fixed-timestep update
        [&](double dt) {
            // Poll SDL events
            SDL_Event e;
            input.begin_frame();
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

            // Movement wish direction (camera-relative XZ)
            glm::vec3 wish = {};
            float yaw_rad = glm::radians(cam_yaw);
            glm::vec3 fwd = {std::sin(yaw_rad), 0.f, -std::cos(yaw_rad)};
            glm::vec3 right = glm::cross(fwd, {0,1,0});
            if (input.is_held(Action::MoveForward)) wish += fwd;
            if (input.is_held(Action::MoveBack))    wish -= fwd;
            if (input.is_held(Action::MoveRight))   wish += right;
            if (input.is_held(Action::MoveLeft))    wish -= right;

            // Submit movement input to server
            EntityID player = client.local_player();
            if (player != NULL_ENTITY) {
                server.queue_player_input(player, PlayerInput{
                    wish,
                    input.is_held   (Action::Crouch),
                    input.is_held   (Action::Sprint),
                });
            }

            // Switch active hand (Space)
            if (input.is_pressed(Action::SwitchHand))
                player_inv.cycle_active_hand();

            // Scroll wheel cycles through overlapping items in the selection area
            float scroll = input.scroll_delta();
            if (scroll != 0.f && !item_candidates.empty()) {
                scroll_item_idx -= (scroll > 0.f ? 1 : -1);
                int n = static_cast<int>(item_candidates.size());
                scroll_item_idx = ((scroll_item_idx % n) + n) % n;
            }

            // Server tick (applies pending inputs, steps physics + simulations)
            server.tick(dt);
            client.tick(dt);

            // Toggle zero-G when player crosses the airlock outer door (z > 12)
            if (player != NULL_ENTITY) {
                auto* cc2 = server.entities().get_component<CharacterControllerComponent>(player);
                auto* tr2 = server.entities().get_component<TransformComponent>(player);
                if (cc2 && tr2) {
                    bool in_space = (tr2->pos.z > 12.f);
                    if (in_space != cc2->zero_g) {
                        cc2->zero_g = in_space;
                        SDL_Log("Airlock: player %s space (zero_g=%s)",
                                in_space ? "entered" : "left",
                                in_space ? "on" : "off");
                    }
                }
            }

            // Sync camera to physics-driven player position
            if (player != NULL_ENTITY) {
                auto* tr = server.entities().get_component<TransformComponent>(player);
                if (tr) cam_pos = tr->pos + glm::vec3(0, 0.5f, 0);
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
            audio.set_local_pressure(101.325f); // TODO: read from atmos zone
            audio.update(static_cast<float>(dt));

            // Update HUD state
            hud_state.clock_str          = "00:00"; // TODO: round timer
            hud_state.cam_pitch          = cam_pitch;
            hud_state.active_hand_is_left = (player_inv.active_hand_id() == "l_hand");

            // ── Item drop (X) ─────────────────────────────────────────────────
            if (input.is_pressed(Action::DropItem)) {
                auto maybe_item = player_inv.take(player_inv.active_hand_id());
                if (maybe_item) {
                    // Cast downward from player feet to find a floor face
                    RayHit down = server.world().raycast(cam_pos, {0,-1,0}, 3.f);
                    if (down.valid) {
                        world_items.spawn(down.voxel, down.face, std::move(*maybe_item));
                    } else {
                        // No floor nearby – spawn floating at player feet
                        world_items.spawn_floating(
                            cam_pos - glm::vec3(0, 0.5f, 0), std::move(*maybe_item));
                    }
                }
            }

            // ── LMB / E: interact with world based on active hand ─────────────
            // Empty hand  + item  → pick up
            // Empty hand  + turf  → nothing (can't pick up turf)
            // Held item   + item  → item-on-item interaction
            // Held item   + turf  → knock on the wall with held item
            {
                bool fps_lmb = !alt_mode.active() && input.is_pressed(Action::PrimaryInteract);
                bool e_press = !alt_mode.active() && input.is_pressed(Action::PickUp);
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
                        if (hand_empty) {
                            // Pick up
                            auto picked = world_items.pick_up(item_ent);
                            if (picked && picked->def)
                                player_inv.put(player_inv.active_hand_id(), std::move(*picked));
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
                    } else if (fhit.valid && !hand_empty) {
                        // Use held item on turf → knock
                        glm::vec3 hit_world = glm::vec3(fhit.voxel) + glm::vec3(0.5f);
                        SDL_Log("Knock: %s on wall",
                                active_slot->item->def
                                    ? active_slot->item->def->name.c_str() : "item");
                        audio.play("knock", hit_world);
                    }
                }
            }

            // ── Alt-mode LMB click on world item ─────────────────────────────
            if (alt_mode.active() && input.is_pressed(Action::PrimaryInteract)
                && hovered_item_entity != NULL_ENTITY) {
                auto*  active_slot = player_inv.active_hand();
                bool   hand_empty  = !active_slot || !active_slot->item;
                if (hand_empty) {
                    auto picked = world_items.pick_up(hovered_item_entity);
                    hovered_item_entity = NULL_ENTITY;
                    if (picked && picked->def) {
                        auto* hand = player_inv.active_hand();
                        if (hand && !hand->item)
                            player_inv.put(player_inv.active_hand_id(), std::move(*picked));
                        else {
                            auto* slot = player_inv.find_empty_accepting(*picked->def);
                            if (slot) player_inv.put(slot->id, std::move(*picked));
                        }
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
            renderer.queue_mobs(server.entities(), cam_pos, cam_yaw);
            renderer.queue_earth_background(cam_pos, cam_yaw, cam_pitch);

            renderer.begin_frame(alpha);

            renderer.draw_world(server.world(), cam_pos, cam_yaw, cam_pitch);
            renderer.draw_face_highlight(hit);
            renderer.draw_world_items();
            renderer.draw_mobs();
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
                    // Small background pill
                    glm::vec2 ts = {static_cast<float>(lbl.name.size() * 7 + 10), 16.f};
                    ui_renderer.rect(lbl.screen_pos - glm::vec2(4, 2), ts,
                                     {0.f, 0.f, 0.f, 0.55f}, 3.f);
                    ui_renderer.text(lbl.screen_pos, lbl.name,
                                     {1.f, 1.f, 0.4f, 1.f}, 14.f);
                }
            }

            // Always-on HUD
            hud.draw(hud_state, player_inv);

            // Alt-mode overlay
            float ov_alpha = alt_mode.overlay_alpha();
            if (ov_alpha > 0.01f) {
                glm::vec2 cursor = alt_mode.cursor_pos();
                // detect mouse state from input
                bool lmb_down     = input.is_held    (Action::PrimaryInteract);
                bool lmb_released = input.is_released(Action::PrimaryInteract);
                bool rmb_pressed  = input.is_pressed (Action::SecondaryInteract);

                bool shift_held = input.is_held(Action::Sprint);
                auto interaction = inv_panel.draw(
                    player_inv, cursor, lmb_down, lmb_released, shift_held, ov_alpha);

                if (interaction.type == PanelInteraction::Type::RightClick) {
                    // Build verb list for the item in that slot
                    const auto* slot = player_inv.find_slot_deep(interaction.slot_id);
                    if (slot && slot->item && slot->item->def) {
                        std::vector<ContextEntry> entries;
                        for (const auto& verb : slot->item->def->verbs) {
                            entries.push_back({verb.name, true, false,
                                [name = verb.name]() {
                                    SDL_Log("Verb: %s", name.c_str());
                                }});
                        }
                        ctx_menu.open(cursor, std::move(entries));
                    }
                }

                // Also open ctx menu on right-click in world face
                if (rmb_pressed && !alt_mode.active() && hit.valid) {
                    (void)hit; // TODO: populate verb list from voxel type
                }
            } else {
                ctx_menu.close();
            }

            ctx_menu.draw(input.mouse_pos(),
                          input.is_pressed(Action::PrimaryInteract));

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
