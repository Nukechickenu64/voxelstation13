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

// ── Hotbar inventory helper ───────────────────────────────────────────────────
static std::vector<InventorySlot> make_human_slots()
{
    return {
        {"head",    {"hat","helmet","mask"},   std::nullopt, {}, false},
        {"eyes",    {"glasses","goggles"},      std::nullopt, {}, false},
        {"ears",    {"headset","earmuffs"},     std::nullopt, {}, false},
        {"mask",    {"mask","respirator"},      std::nullopt, {}, false},
        {"uniform", {"uniform","jumpsuit"},     std::nullopt, {}, false},
        {"suit",    {"suit","hardsuit"},        std::nullopt, {}, false},
        {"gloves",  {"gloves"},                 std::nullopt, {}, false},
        {"boots",   {"boots"},                  std::nullopt, {}, false},
        {"back",    {"bag","tank"},             std::nullopt, {}, false},
        {"belt",    {"belt","holster"},         std::nullopt, {}, false},
        {"l_hand",  {"*"},                      std::nullopt, {}, false},
        {"r_hand",  {"*"},                      std::nullopt, {}, false},
        {"id_card", {"id_card"},                std::nullopt, {}, false},
        {"pda",     {"pda"},                    std::nullopt, {}, false},
    };
}

// ────────────────────────────────────────────────────────────────────────────
int main(int /*argc*/, char* /*argv*/[])
{
    // ── 1. Data validation ────────────────────────────────────────────────────
    DataValidator validator;
    if (!validator.validate_all("data")) {
        SDL_Log("Data validation failed with %zu error(s).",
                validator.errors().size());
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
    Inventory player_inv(make_human_slots());

    // ── 12. Test world geometry ───────────────────────────────────────────────
    {
        // Flat floor of solid voxels at y=0, air above
        uint16_t floor_id = voxel_reg.id_of("floor");
        if (floor_id == 0) floor_id = 1; // fallback if data not loaded
        Voxel floor_voxel;
        floor_voxel.type_id     = floor_id;
        floor_voxel.flags       = VFLAG_SOLID | VFLAG_OPAQUE;
        for (int x = -8; x <= 8; ++x)
            for (int z = -8; z <= 8; ++z)
                server.world().set_voxel({x, 0, z}, floor_voxel);

        // Some walls around the perimeter
        uint16_t wall_id = voxel_reg.id_of("wall");
        if (wall_id == 0) wall_id = 2;
        Voxel wall_voxel;
        wall_voxel.type_id = wall_id;
        wall_voxel.flags   = VFLAG_SOLID | VFLAG_OPAQUE;
        for (int i = -8; i <= 8; ++i) {
            for (int y = 1; y <= 3; ++y) {
                server.world().set_voxel({ i, y, -8}, wall_voxel);
                server.world().set_voxel({ i, y,  8}, wall_voxel);
                server.world().set_voxel({-8, y,  i}, wall_voxel);
                server.world().set_voxel({ 8, y,  i}, wall_voxel);
            }
        }

        // Enqueue all dirty chunks for meshing
        for (Chunk* c : server.world().dirty_chunks())
            mesher.enqueue(c->chunk_pos(), server.world());
    }

    // ── 13. HUD state ─────────────────────────────────────────────────────────
    HUDState hud_state;
    hud_state.health     = 100.f;
    hud_state.health_max = 100.f;

    // ── 14. Camera state ──────────────────────────────────────────────────────
    float cam_yaw   = 0.f;
    float cam_pitch = 0.f;
    glm::vec3 cam_pos = {0.f, 1.75f, 0.f}; // eye height ~1.75 m

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

            // Send movement to server physics
            EntityID player = client.local_player();
            if (player != NULL_ENTITY) {
                server.world(); // just accessing to avoid unused warning
                // server's physics is ticked inside server.tick()
                // For loopback we apply movement directly
                auto* tr = server.entities().get_component<TransformComponent>(player);
                if (tr) cam_pos = tr->pos + glm::vec3(0, 1.75f, 0);
            }

            // Server tick
            server.tick(dt);
            client.tick(dt);

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
            GasMixture local_gas = server.world().get_voxel(
                glm::ivec3(glm::floor(cam_pos))).type_id == 0
                    ? GasMixture{} : GasMixture{};
            audio.set_local_pressure(101.325f); // TODO: read from atmos zone
            audio.update(static_cast<float>(dt));

            // Update HUD state
            hud_state.clock_str = "00:00"; // TODO: round timer
            hud_state.examine_label = "";  // TODO: set from ray-hit voxel type name

            // Inventory: hotbar scroll
            if (input.is_pressed(Action::DropItem)) {
                auto item = player_inv.take(player_inv.active_hand_id());
                // TODO: spawn item entity in world at player's feet
                (void)item;
            }
            if (input.is_pressed(Action::Escape)) {
                SDL_SetWindowRelativeMouseMode(renderer.window(), false);
            }
        },

        // Variable-rate render
        [&](double alpha) {
            client.interpolate(alpha);

            renderer.begin_frame(alpha);

            // Ray cast for face selection
            float yaw_r = glm::radians(cam_yaw), pitch_r = glm::radians(cam_pitch);
            glm::vec3 ray_dir = {
                std::cos(pitch_r) * std::sin(yaw_r),
                std::sin(pitch_r),
               -std::cos(pitch_r) * std::cos(yaw_r)
            };
            RayHit hit = server.world().raycast(
                cam_pos - glm::vec3(0, 1.75f, 0) + glm::vec3(0, 1.f, 0),
                ray_dir, 4.f);

            renderer.draw_world(server.world(), cam_pos, cam_yaw, cam_pitch);
            renderer.draw_face_highlight(hit);
            renderer.draw_viewmodel(0); // TODO: held item type id

            // UI pass
            ui_renderer.begin();

            // Always-on HUD
            hud.draw(hud_state, player_inv, input.active_hotbar_slot());

            // Alt-mode overlay
            float ov_alpha = alt_mode.overlay_alpha();
            if (ov_alpha > 0.01f) {
                glm::vec2 cursor = alt_mode.cursor_pos();
                // detect mouse state from input
                bool lmb_down     = input.is_held    (Action::PrimaryInteract);
                bool lmb_released = input.is_released(Action::PrimaryInteract);
                bool rmb_pressed  = input.is_pressed (Action::SecondaryInteract);

                auto interaction = inv_panel.draw(
                    player_inv, cursor, lmb_down, lmb_released, ov_alpha);

                if (interaction.type == PanelInteraction::Type::RightClick) {
                    // Build verb list for the item in that slot
                    const auto* slot = player_inv.find_slot(interaction.slot_id);
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

            ui_renderer.end(nullptr); // TODO: pass real SDL_GPUCommandBuffer

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
