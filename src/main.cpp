#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "core/game_loop.h"
#include "core/world.h"
#include "core/entity_manager.h"
#include "core/signals.h"
#include "core/master_controller.h"
#include "render/renderer.h"
#include "render/chunk_mesher.h"
#include "render/lighting.h"
#include "render/ui_renderer.h"
#include "simulation/physics.h"
#include "simulation/world_items.h"
#include "simulation/mob_system.h"
#include "simulation/status_effects.h"
#include "simulation/attack_chain.h"
#include "simulation/projectile_system.h"
#include "simulation/enclosure.h"
#include "simulation/vehicle.h"
#include "simulation/vehicle_manager.h"
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
#include "ui/main_menu.h"
#include "ui/pause_menu.h"
#include "ui/character_creator.h"
#include "core/object_types.h"
#include "core/verb_dispatch.h"
#include "network/client.h"
#include "network/input_snapshot.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <unordered_set>

// Player inventory is now built by make_player_inventory() in inventory.cpp

// ────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    // ── 1. Renderer + window (must come first to show the loading screen) ────
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

    // Helper: render one loading frame with a status message so the player
    // never stares at a black screen while resources are being loaded.
    auto render_loading_frame = [&](const char* status) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {} // keep the window responsive
        renderer.begin_frame(0.0);
        if (!renderer.swapchain_tex()) { renderer.end_frame(); return; }
        renderer.end_world_pass();
        const float fw = static_cast<float>(ui_renderer.fb_width());
        const float fh = static_cast<float>(ui_renderer.fb_height());
        ui_renderer.begin();
        // Dark background
        ui_renderer.rect({0.f, 0.f}, {fw, fh}, {0.04f, 0.04f, 0.08f, 1.0f});
        // Faint scanlines for atmosphere
        for (float y = 0.f; y < fh; y += 4.f)
            ui_renderer.rect({0.f, y}, {fw, 1.f}, {0.f, 0.f, 0.f, 0.06f});
        // Title text
        constexpr const char* LOAD_TITLE = "VOXELSTATION 13";
        float tw = static_cast<float>(SDL_strlen(LOAD_TITLE)) * 12.5f;
        ui_renderer.text({(fw - tw) * 0.5f, fh * 0.40f},
                         LOAD_TITLE, {0.80f, 0.90f, 1.00f, 1.00f}, 22.f);
        // Status bar at bottom
        constexpr float BAR_H = 48.f;
        ui_renderer.rect({0.f, fh - BAR_H}, {fw, BAR_H},
                         {0.08f, 0.09f, 0.18f, 0.95f});
        ui_renderer.rect({0.f, fh - BAR_H}, {fw, 1.5f},
                         {0.25f, 0.30f, 0.50f, 0.70f});
        float sw = static_cast<float>(SDL_strlen(status)) * 7.0f;
        ui_renderer.text({(fw - sw) * 0.5f, fh - BAR_H + 14.f},
                         status, {0.70f, 0.80f, 0.95f, 0.90f}, 14.f);
        ui_renderer.end(renderer.cmd_buf(), renderer.swapchain_tex(),
                        renderer.width(), renderer.height());
        renderer.end_frame();
    };

    render_loading_frame("Validating data...");

    // ── 2. Data validation ────────────────────────────────────────────────────
    DataValidator validator;
    if (!validator.validate_all("data")) {
        SDL_Log("Data validation failed with %u error(s).",
                static_cast<unsigned>(validator.errors().size()));
        // Non-fatal during early development; continue anyway.
    }

    render_loading_frame("Loading registries...");

    // ── 3. Registry loading ───────────────────────────────────────────────────
    VoxelRegistry voxel_reg;
    voxel_reg.load_directory("data/voxel_types");

    // Initialise the global prototype/type-path registry before item loading.
    init_prototype_registry();

    // Initialise the verb handler dispatch registry (built-in handlers).
    // Game-specific handlers that need simulation-system captures are registered
    // below, after all local variables are in scope.
    init_verb_dispatch();

    // ── TG SS13 systems ───────────────────────────────────────────────────────
    // Signal bus:  must come first
    init_signals();
    // Master Controller: processing list subsystem
    init_master_controller();

    ItemRegistry item_reg;
    item_reg.load_directory("data/item_types");

    MobSpeciesRegistry mob_species_reg;
    mob_species_reg.load_directory("data/mob_species");

    render_loading_frame("Loading textures...");

    // ── 4. Renderer texture + asset loading ───────────────────────────────────
    renderer.load_tile_textures(voxel_reg, "textures");
    renderer.load_item_textures(item_reg,  "textures");
    renderer.load_mob_textures("textures");
    renderer.load_door_anim("textures/specialtile/public/glass/opening.gif",
                             voxel_reg.id_of("door_anim"));
    // Load glass overlay color + fill/coverage mask GIFs so the overlay can be
    // composited with correct per-frame transparency during door animations.
    renderer.load_door_overlay_anim(
        "textures/specialtile/public/overlays/glass_opening.gif");
    renderer.load_door_fill_anim(
        "textures/specialtile/public/glass/fill_opening.gif");
    renderer.load_human_bodyparts("legacysets/extracted");
    renderer.load_model("smes", "models/SMES.mesh", "textures/models/smes.png");

    ui_renderer.load_item_icons(item_reg, "textures");

    // ── 4. Chunk mesher ───────────────────────────────────────────────────────
    ChunkMesher mesher;
    mesher.start(4);
    mesher.set_registry(&voxel_reg);  // enables per-face texture selection

    // ── 5. Networking mode ────────────────────────────────────────────────────
    // --server host[:port]  → connect to a specific remote dedicated server
    // (default)             → auto-launch vs13_server.exe and connect to localhost
    std::string remote_host = "127.0.0.1";
    uint16_t    remote_port = 7778;
    bool        need_launch_server = true;
    for (int i = 1; i < argc; ++i) {
        if (SDL_strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            need_launch_server = false;
            std::string addr = argv[++i];
            auto colon = addr.rfind(':');
            if (colon != std::string::npos) {
                remote_host = addr.substr(0, colon);
                remote_port = static_cast<uint16_t>(std::stol(addr.substr(colon + 1)));
            } else {
                remote_host = std::move(addr);
            }
        }
    }

    if (need_launch_server) {
        render_loading_frame("Starting local server...");
#ifdef _WIN32
        // Try the current directory first, then build_ninja/ (dev builds)
        const char* search[] = { "vs13_server.exe", "build_ninja\\vs13_server.exe" };
        bool launched = false;
        for (const char* path : search) {
            STARTUPINFOA si{};
            PROCESS_INFORMATION pi{};
            si.cb = sizeof(si);
            char cmd[256];
            SDL_snprintf(cmd, sizeof(cmd), "%s", path);
            if (CreateProcessA(path, cmd, nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                launched = true;
                break;
            }
        }
        if (!launched) {
            SDL_Log("Failed to launch vs13_server.exe — is it in the same directory?");
            SDL_Quit();
            return 1;
        }
#else
        SDL_Log("Auto-server launch not yet implemented on this platform. "
                "Please start vs13_server manually and use --server 127.0.0.1:7778.");
        SDL_Quit();
        return 1;
#endif
        SDL_Delay(400); // give the server time to bind its port
    }

    // ── 6. Client ─────────────────────────────────────────────────────────────
    Client client;
    client.set_registry(&voxel_reg);
    client.set_item_registry(&item_reg);
    render_loading_frame("Connecting to server...");
    if (!client.connect(remote_host.c_str(), remote_port)) {
        SDL_Log("Failed to connect to %s:%u",
                remote_host.c_str(), static_cast<unsigned>(remote_port));
        SDL_Quit();
        return 1;
    }

    // World / entity accessors — always the client's replicated copies.
    World&         g_world    = client.world();
    EntityManager& g_entities = client.entities();

    // ── Server-side simulation stubs (not available in remote-client mode) ────
    // These are set to nullptr so client-only code compiles cleanly.
    // Any code that checks these will gracefully skip server-side features.
    AtmosSimulator*    g_atmos         = nullptr;
    EnclosureDetector  enclosure_detector{g_world};  // needs World reference
    // Vehicle / vehicle mesher stubs (server-side only)
    VehicleGrid*       test_vehicle    = nullptr;
    ChunkMesher        vehicle_mesher;
    // Melee reach constant (was missing)
    static constexpr float MELEE_REACH = 2.5f;

    // ── 7. Lighting ───────────────────────────────────────────────────────────
    LightingSystem lighting(g_world);
    lighting.set_registry(&voxel_reg);  // must be set before rebuild()
    mesher.set_lighting(&lighting);     // enables colored smooth lighting
    renderer.set_lighting(&g_world, &lighting.light_map());  // entity/model lighting
    // When any entity is destroyed, clean up its dynamic light (if any).
    g_entities.on_destroy([&](EntityID id) { lighting.remove_dynamic_light(id); });

    // ── 8. Audio ─────────────────────────────────────────────────────────────
    AudioManager audio;
    audio.init();
    audio.load_events("data/sounds.json");

    // ── 9. Input ──────────────────────────────────────────────────────────────
    InputManager input;
    input.capture_cursor(renderer.window(), false); // captured when entering the game

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
    PauseMenu      pause_menu(ui_renderer);
    GameSettings   game_settings;                  // player-controlled settings
    bool           fullbright_enabled   = false;   // toggled via F1 admin menu
    bool           ao_enabled           = true;    // AO on by default
    double         sim_time              = 0.0;  // monotonic seconds, for overlay animations

    // ── Dev-tool simulation flags (F1 admin menu) ─────────────────────────
    bool           freeze_sim           = false;   // pause server/physics tick
    bool           slow_motion          = false;   // 0.1× time scale
    bool           auto_heal            = false;   // regen HP to max every frame
    bool           zerog_override       = false;   // force zero-G regardless of atmos
    bool           infinite_oxy         = false;   // zero out oxy-damage every frame

    // ── 11. Player inventory ──────────────────────────────────────────────────
    Inventory player_inv = make_player_inventory();

    // ── 11b. World item system ────────────────────────────────────────────────
    WorldItemSystem world_items(g_world, g_entities);

    // ── 11b2. Projectile system ───────────────────────────────────────────────
    ProjectileSystem projectile_sys(g_world, g_entities);

    // ── 11c. Static model objects ─────────────────────────────────────────────
    ModelObjectManager model_objs;
    {
        glm::vec3 mn, mx;
        if (renderer.model_local_aabb("smes", mn, mx))
            model_objs.register_extents("smes", mn, mx);
    }

    // ── 11d. Map editor ───────────────────────────────────────────────────────
    std::vector<std::string> model_names{"smes"};
    MapEditor map_editor(ui_renderer, g_world, voxel_reg,
                         g_entities, world_items, item_reg,
                         mob_species_reg, model_objs, model_names);

    // ── 12. World geometry received from server via ChunkData packets ─────────
    // (No local world setup — the server owns and broadcasts the authoritative world.)

    // ── 13. HUD state ─────────────────────────────────────────────────────────
    HUDState hud_state;
    // health values are synced each tick from HealthComponent — initialise
    // to sensible defaults in case the component isn't ready yet.
    hud_state.health     = 100.f;
    hud_state.health_max = 100.f;
    // ── Physics-gun hold state
    bool  phy_holding   = false;
    float phy_hold_dist = 5.0f;

    // ── 14. Camera state ──────────────────────────────────────────────────────
    float cam_yaw   = 0.f;
    float cam_pitch = 0.f;
    float cam_roll  = 0.f;  // camera roll in degrees — used by Dizzy status effect; 0 when healthy/prone
    float prone_t   = 0.f;  // smooth 0→1 transition: 0=standing upright, 1=fully prone/crawling
    glm::vec3 cam_pos = {0.f, 1.5f, 0.f};  // player feet at y=1, eyes at +0.5

    // ── 14d. View bob & Quake-style movement sway ─────────────────────────────
    float bob_time   = 0.f;   // walk cycle phase (radians)
    float bob_blend  = 0.f;   // smoothed blend weight (0=still, 1=full walk speed)
    float bob_sway   = 0.f;   // smoothed lateral strafe-sway offset (degrees yaw)
    float player_body_yaw   = 0.f;         // body direction; decoupled from cam_yaw during 3P orbit
    bool  third_person      = false;
    static constexpr float k_3p_dist = 0.7f;  // pull-back distance in world units

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

    // Overlay atlas layer for the animating door type — used to upload
    // composited glass overlay frames (overlay color + fill alpha).
    const uint16_t door_anim_overlay_idx = [&]() -> uint16_t {
        const VoxelTypeDef* def = voxel_reg.get(door_anim_type_id);
        return def ? def->overlay_atlas_index : 0;
    }();

    // Speed multiplier for door open/close animation.
    // 1.0 = original GIF speed, 2.0 = twice as fast, 0.5 = half speed.
    constexpr float DOOR_ANIM_SPEED = 6.0f;

    // Per-panel door animation state (one entry per door group currently animating).
    struct DoorGroup {
        std::vector<glm::ivec3> voxels;
        int   frame    = 0;
        float accum_ms = 0.f;
        bool  closing  = false;  // true = playing frames in reverse toward door state
        // Context: if null, uses main server world.  Set for vehicle doors.
        World*           ctx_world  = nullptr;
        AtmosSimulator*  ctx_atmos  = nullptr;
        ChunkMesher*     ctx_mesher = nullptr;
    };
    std::vector<DoorGroup> animating_doors;

    // Flood-fill: find all adjacent voxels of target_type in the same Z-plane.
    auto flood_fill_door = [&](World& w, glm::ivec3 seed, uint16_t target_type) {
        std::vector<glm::ivec3>             group;
        std::unordered_set<glm::ivec3> visited;
        std::vector<glm::ivec3>             queue;
        auto try_push = [&](glm::ivec3 p) {
            if (p.z != seed.z) return;
            if (!visited.insert(p).second) return;
            if (w.get_voxel(p).type_id == target_type)
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

    render_loading_frame("Ready to play!");

    // ── 15. Character profile (TG SS13-style appearance preferences) ──────────
    CharacterProfile player_profile;   // default; overridden by Setup Character

    // ── 16. Main menu ─────────────────────────────────────────────────────────
    {
        MainMenu main_menu(ui_renderer);
        bool in_menu = true;
        while (in_menu) {
            SDL_Event e;
            bool lmb_down = false;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT)           return 0;
                if (e.type == SDL_EVENT_WINDOW_RESIZED)
                    ui_renderer.on_resize(e.window.data1, e.window.data2);
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                    && e.button.button == SDL_BUTTON_LEFT)
                    lmb_down = true;
            }
            float mx = 0.f, my = 0.f;
            SDL_GetMouseState(&mx, &my);

            main_menu.tick(1.0 / 60.0);

            renderer.begin_frame(0.0);
            if (renderer.swapchain_tex()) {
                renderer.end_world_pass();
                ui_renderer.begin();
                MainMenuResult mr = main_menu.draw({mx, my}, lmb_down);
                ui_renderer.end(renderer.cmd_buf(), renderer.swapchain_tex(),
                                renderer.width(), renderer.height());
                renderer.end_frame();
                if (mr.exit_clicked) return 0;
                if (mr.play_clicked) in_menu = false;

                // ── Character creation sub-screen ──────────────────────────────
                if (mr.char_create_clicked) {
                    CharacterCreator creator(ui_renderer, renderer.window(), player_profile,
                                             &mob_species_reg);
                    bool in_creator = true;
                    while (in_creator) {
                        SDL_Event ce;
                        bool clmb = false;
                        while (SDL_PollEvent(&ce)) {
                            if (ce.type == SDL_EVENT_QUIT)           return 0;
                            if (ce.type == SDL_EVENT_WINDOW_RESIZED)
                                ui_renderer.on_resize(ce.window.data1, ce.window.data2);
                            if (ce.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                                && ce.button.button == SDL_BUTTON_LEFT)
                                clmb = true;
                            creator.process_event(ce);
                        }
                        float cmx = 0.f, cmy = 0.f;
                        SDL_GetMouseState(&cmx, &cmy);
                        creator.tick(1.0 / 60.0);
                        renderer.begin_frame(0.0);
                        if (renderer.swapchain_tex()) {
                            renderer.end_world_pass();
                            ui_renderer.begin();
                            auto cr = creator.draw({cmx, cmy}, clmb);
                            ui_renderer.end(renderer.cmd_buf(), renderer.swapchain_tex(),
                                            renderer.width(), renderer.height());
                            renderer.end_frame();
                            if (cr.accepted) {
                                player_profile = creator.profile();
                                in_creator = false;
                            }
                            if (cr.back) { in_creator = false; }
                        } else {
                            renderer.end_frame();
                        }
                        SDL_Delay(16);
                    }
                }
            } else {
                renderer.end_frame();
            }
            SDL_Delay(16); // ~60 fps
        }
    }

    // ── 17. Apply character profile to the local player entity ────────────────
    {
        EntityID player_ent = client.local_player();
        if (player_ent != NULL_ENTITY) {
            // Patch the Bodypart layer with chosen gender + skin tone
            auto* app = g_entities.get_component<HumanAppearance>(player_ent);
            if (app) {
                for (auto& layer : app->layers) {
                    if (layer.kind == HumanOverlayKind::Bodypart) {
                        layer.gender = player_profile.is_male ? "_m" : "_f";
                        layer.tint   = { player_profile.skin_color.r,
                                         player_profile.skin_color.g,
                                         player_profile.skin_color.b, 255 };
                    }
                }

                // Remove any previously-applied face/hair/eye layers
                app->layers.erase(
                    std::remove_if(app->layers.begin(), app->layers.end(),
                        [](const HumanOverlay& ov) {
                            return ov.kind == HumanOverlayKind::Clothing &&
                                   (ov.sprite_dir == "human/human_face" ||
                                    ov.sprite_dir == "human/human_eyes");
                        }),
                    app->layers.end());

                // Eye overlays (keys: human/human_eyes/{prefix}{_s/_n/_e/_w})
                for (const char* eye_pfx : { "eyes_l", "eyes_r" }) {
                    HumanOverlay eye{};
                    eye.kind       = HumanOverlayKind::Clothing;
                    eye.sprite_dir = "human/human_eyes";
                    eye.prefix     = eye_pfx;
                    eye.tint       = { player_profile.eye_color.r,
                                       player_profile.eye_color.g,
                                       player_profile.eye_color.b, 255 };
                    app->layers.push_back(eye);
                }

                // Hair overlay
                if (!player_profile.hair_file.empty()) {
                    HumanOverlay hair{};
                    hair.kind       = HumanOverlayKind::Clothing;
                    hair.sprite_dir = "human/human_face";
                    hair.prefix     = player_profile.hair_file;
                    hair.tint       = { player_profile.hair_color.r,
                                        player_profile.hair_color.g,
                                        player_profile.hair_color.b, 255 };
                    app->layers.push_back(hair);
                }

                // Facial hair overlay (male only)
                if (player_profile.is_male && !player_profile.facial_file.empty()) {
                    HumanOverlay fh{};
                    fh.kind       = HumanOverlayKind::Clothing;
                    fh.sprite_dir = "human/human_face";
                    fh.prefix     = player_profile.facial_file;
                    fh.tint       = { player_profile.facial_color.r,
                                      player_profile.facial_color.g,
                                      player_profile.facial_color.b, 255 };
                    app->layers.push_back(fh);
                }

                app->dirty = true;
            }
            // Update display name shown in examine / context menu
            auto* nc = g_entities.get_component<NameComponent>(player_ent);
            if (nc) nc->name = player_profile.name;

            // Re-apply species stats (player was spawned as human by default;
            // if a different species was chosen, overwrite with its stats now)
            const MobSpeciesDef* sp_def = mob_species_reg.get(player_profile.species);
            if (sp_def) {
                auto* cc  = g_entities.get_component<CharacterControllerComponent>(player_ent);
                auto* hpc = g_entities.get_component<HealthComponent>(player_ent);
                if (cc) {
                    cc->move_speed  = sp_def->move_speed;
                    cc->sprint_mult = sp_def->sprint_mult;
                    cc->jump_vel    = sp_def->jump_vel;
                    cc->height      = std::min(sp_def->height - 0.1f, 0.9f);
                    cc->radius      = sp_def->radius * 0.75f;
                }
                if (hpc) {
                    hpc->health_max = sp_def->health_max;
                    // Reset all damage buckets and stamina to full-health
                    hpc->brute = hpc->burn = hpc->tox = hpc->oxy = hpc->clone = 0.f;
                    hpc->stam_damage = 0.f;
                    hpc->recalculate();
                }
            }
            // Update species tag
            auto* tag = g_entities.get_component<MobPlayerTag>(player_ent);
            if (tag) tag->species = player_profile.species;
        }
    }

    // Send the final character appearance to the server so other clients see
    // the correct eye colour / hair after the character creator has run.
    if (client.connected()) {
        Client::AppearanceInfo ai;
        ai.eye_r     = player_profile.eye_color.r;
        ai.eye_g     = player_profile.eye_color.g;
        ai.eye_b     = player_profile.eye_color.b;
        ai.hair_file = player_profile.hair_file;
        ai.hair_r    = player_profile.hair_color.r;
        ai.hair_g    = player_profile.hair_color.g;
        ai.hair_b    = player_profile.hair_color.b;
        ai.is_male   = player_profile.is_male;
        client.set_appearance(ai);
        client.send_appearance_update();
    }

    // Capture cursor now that the player has entered the game.
    input.capture_cursor(renderer.window(), true);

    // ── Spawn test guns and ammo near player start ────────────────────────────
    {
        auto spawn_item = [&](const char* item_id, glm::vec3 pos) {
            const ItemDef* def = item_reg.get(item_id);
            if (!def) return;
            ItemStack st; st.def = def; st.count = 1; st.integrity = 1.f;
            world_items.spawn_floating(pos, std::move(st));
        };
        spawn_item("sec_pistol",   { 2.f, 1.f,  0.f });
        spawn_item("pistol_mag",   { 2.f, 1.f,  0.5f});
        spawn_item("pistol_mag",   { 2.f, 1.f,  1.0f});
        spawn_item("revolver",     { 3.f, 1.f,  0.f });
        spawn_item("revolver_ammo",{ 3.f, 1.f,  0.5f});
        spawn_item("revolver_ammo",{ 3.f, 1.f,  1.0f});
        spawn_item("shotgun",      { 4.f, 1.f,  0.f });
        spawn_item("shotgun_shell",{ 4.f, 1.f,  0.5f});
        spawn_item("shotgun_shell",{ 4.f, 1.f,  1.0f});
        spawn_item("c20r_smg",     { 5.f, 1.f,  0.f });
        spawn_item("smg_mag",      { 5.f, 1.f,  0.5f});
        spawn_item("laser_gun",    { 6.f, 1.f,  0.f });
        spawn_item("laser_pistol", { 6.f, 1.f,  0.5f});
    }

    // ── 16. Game loop ─────────────────────────────────────────────────────────

    // ── Register game-specific verb handlers (override stubs from init) ───────
    // Registered here so they can capture local simulation variables by reference.
    // All captured references are valid for the entire game loop lifetime.
    {
        auto& vd = verb_dispatch();

        // verb_examine: richer output using local hud_state
        vd.register_handler("verb_examine", [&hud_state](const VerbContext& ctx) {
            if (!ctx.item_def) { ctx.log("[Examine] You see something."); return; }
            const ItemStack* st = ctx.item_stack;
            std::string disp = ctx.item_def->name;
            if (st && !st->custom_name.empty()) disp += " \"" + st->custom_name + "\"";
            if (st && st->count > 1) disp += " x" + std::to_string(st->count);
            char buf[280];
            std::snprintf(buf, sizeof(buf),
                "[Examine] %s \xe2\x80\x94 %.2f kg / %.1f L \xe2\x80\x94 %s%s",
                disp.c_str(), ctx.item_def->weight, ctx.item_def->volume,
                st ? condition_label(st->integrity) : "unknown condition",
                ctx.item_def->type_path.empty() ? ""
                    : ("  (" + ctx.item_def->type_path + ")").c_str());
            if (hud_state.radio_log.size() >= 30) hud_state.radio_log.pop_front();
            hud_state.radio_log.push_back(buf);
        });

        // verb_throw: lob the target entity in the look direction
        vd.register_handler("verb_throw", [&world_items, &cam_pos, &cam_yaw,
                                            &cam_pitch, &hud_state](const VerbContext& ctx) {
            if (ctx.target_ent == NULL_ENTITY) { ctx.log("[Throw] Nothing to throw."); return; }
            auto picked = world_items.pick_up(ctx.target_ent);
            if (!picked) { ctx.log("[Throw] Can't pick that up."); return; }
            float yr = glm::radians(cam_yaw), pr = glm::radians(cam_pitch);
            glm::vec3 throw_dir = {
                std::cos(pr) * std::sin(yr),
                std::sin(pr),
               -std::cos(pr) * std::cos(yr)
            };
            if (throw_dir.y < -0.3f) throw_dir.y = -0.3f;
            throw_dir = glm::normalize(throw_dir);
            world_items.spawn_floating(cam_pos + throw_dir,
                                       std::move(*picked), throw_dir * 8.f);
            ctx.log("[Throw] You throw the "
                + (ctx.item_def ? ctx.item_def->name : "item") + ".");
        });

        // verb_open: toggle container open/closed
        vd.register_handler("verb_open", [](const VerbContext& ctx) {
            if (!ctx.item_stack || !ctx.item_def || !ctx.item_def->is_container) {
                ctx.log("[Open] Cannot open this."); return;
            }
            ctx.item_stack->container_open = !ctx.item_stack->container_open;
            ctx.log(ctx.item_stack->container_open
                ? "[Open] You open the "  + ctx.item_def->name + "."
                : "[Close] You close the " + ctx.item_def->name + ".");
        });

        // verb_stun: stun baton – feedback message only; real stamina/stun damage is
        // applied by attack_chain via the "stam:80" and "force:5" tags on the item.
        vd.register_handler("verb_stun", [](const VerbContext& ctx) {
            ctx.log("[Stun Baton] You swing the stun baton.");
        });

        // verb_apply_bandage: gauze roll — heals brute damage on self or target mob.
        // ctx.target_ent may be a mob entity when triggered from the mob context menu.
        vd.register_handler("verb_apply_bandage",
            [&g_entities, &player_inv, &client](const VerbContext& ctx) {
                // Determine target: explicit entity (mob) or actor (self)
                EntityID target = (ctx.target_ent != NULL_ENTITY)
                                      ? ctx.target_ent
                                      : ctx.actor;
                auto* hp = g_entities.get_component<HealthComponent>(target);
                if (!hp) { ctx.log("[Bandage] No valid target."); return; }
                if (hp->brute <= 0.f) {
                    ctx.log("[Bandage] No brute injuries to treat."); return;
                }

                constexpr float BRUTE_HEAL = 15.f;
                hp->heal_type("brute", BRUTE_HEAL);

                // Consume one from the stack via inventory
                Inventory* inv = &player_inv;
                auto* hand = inv->active_hand();
                if (hand && hand->item && hand->item->def
                        && hand->item->def->id == "gauze") {
                    hand->item->count -= 1;
                    if (hand->item->count <= 0)
                        hand->item = std::nullopt;
                }

                const bool is_self = (target == ctx.actor);
                char buf[128];
                if (is_self)
                    std::snprintf(buf, sizeof(buf),
                        "[Bandage] You dress your wounds. (brute now %.0f)", hp->brute);
                else {
                    auto* nc = g_entities.get_component<NameComponent>(target);
                    std::string tname = nc ? nc->name : "them";
                    std::snprintf(buf, sizeof(buf),
                        "[Bandage] You bandage %s. (brute now %.0f)",
                        tname.c_str(), hp->brute);
                }
                ctx.log(buf);
            });

        // verb_take_pill: consume a pill from the active hand to heal all damage types.
        vd.register_handler("verb_take_pill",
            [&player_inv, &client, &g_entities](const VerbContext& ctx) {
                EntityID actor = ctx.actor;
                if (actor == NULL_ENTITY) actor = client.local_player();
                auto* hp = g_entities.get_component<HealthComponent>(actor);
                if (!hp || hp->dead) { ctx.log("[Pill] Can't take that now."); return; }

                constexpr float PILL_HEAL = 10.f;
                hp->heal(PILL_HEAL);

                auto* hand = player_inv.active_hand();
                if (hand && hand->item && hand->item->def
                        && hand->item->def->id == "pill") {
                    hand->item->count -= 1;
                    if (hand->item->count <= 0)
                        hand->item = std::nullopt;
                }

                char buf[96];
                std::snprintf(buf, sizeof(buf),
                    "[Pill] You swallow a pill. (hp %.0f/%.0f)",
                    hp->current(), hp->health_max);
                ctx.log(buf);
            });

        // verb_transfuse: blood pack — heals oxy damage on self or a target mob.
        vd.register_handler("verb_transfuse",
            [&g_entities, &player_inv, &client](const VerbContext& ctx) {
                EntityID target = (ctx.target_ent != NULL_ENTITY)
                                      ? ctx.target_ent
                                      : ctx.actor;
                auto* hp = g_entities.get_component<HealthComponent>(target);
                if (!hp) { ctx.log("[Blood Pack] No valid target."); return; }

                constexpr float OXY_HEAL = 25.f;
                hp->heal_type("oxy", OXY_HEAL);

                auto* hand = player_inv.active_hand();
                if (hand && hand->item && hand->item->def
                        && hand->item->def->id == "blood_pack") {
                    hand->item->count -= 1;
                    if (hand->item->count <= 0)
                        hand->item = std::nullopt;
                }

                const bool is_self = (target == ctx.actor);
                char buf[128];
                if (is_self)
                    std::snprintf(buf, sizeof(buf),
                        "[Blood Pack] You administer a blood transfusion. (oxy now %.0f)",
                        hp->oxy);
                else {
                    auto* nc = g_entities.get_component<NameComponent>(target);
                    std::string tname = nc ? nc->name : "them";
                    std::snprintf(buf, sizeof(buf),
                        "[Blood Pack] You transfuse blood into %s. (oxy now %.0f)",
                        tname.c_str(), hp->oxy);
                }
                ctx.log(buf);
            });

        // verb_defib: defibrillator — revive a dead mob if still revivable.
        vd.register_handler("verb_defib",
            [&g_entities](const VerbContext& ctx) {
                EntityID target = ctx.target_ent;
                if (target == NULL_ENTITY) {
                    ctx.log("[Defib] No target in range."); return;
                }
                auto* hp = g_entities.get_component<HealthComponent>(target);
                if (!hp || !hp->dead) {
                    ctx.log("[Defib] Target is not dead."); return;
                }
                auto* co = g_entities.get_component<CorpseComponent>(target);
                if (co && !co->can_be_revived) {
                    ctx.log("[Defib] Too much time has passed — revival impossible.");
                    return;
                }

                // Heal enough damage to bring health above 0 and put in softcrit
                hp->heal_type("oxy",   std::max(0.f, hp->oxy   - hp->health_max * 0.4f));
                hp->heal_type("brute", std::max(0.f, hp->brute - hp->health_max * 0.6f));
                hp->heal_type("burn",  std::max(0.f, hp->burn  - hp->health_max * 0.6f));
                // Ensure health is at least 1 above zero for the revive
                if (hp->current() <= 0.f) {
                    hp->heal_type("brute", -hp->current() + 1.f);
                }
                hp->dead = false;
                hp->recalculate();

                g_entities.remove_component<CorpseComponent>(target);
                auto* cc_r = g_entities.get_component<CharacterControllerComponent>(target);
                if (cc_r) cc_r->mob_state = MobState::Softcrit;
                signals().send_signal(target, COMSIG_MOB_LIVING_REVIVE, SigDeath{});

                auto* nc = g_entities.get_component<NameComponent>(target);
                std::string tname = nc ? nc->name : "them";
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                    "[Defib] %s has been revived! (hp %.0f/%.0f)",
                    tname.c_str(), hp->current(), hp->health_max);
                ctx.log(buf);
            });

        // verb_inject / verb_fill_syringe: chemistry stubs (reagent system not yet wired)
        vd.register_handler("verb_inject", [](const VerbContext& ctx) {
            ctx.log("[Syringe] The reagent system is not yet connected.");
        });
        vd.register_handler("verb_fill_syringe", [](const VerbContext& ctx) {
            ctx.log("[Syringe] Nothing to fill from here.");
        });
        // verb_pour: chemistry stub
        vd.register_handler("verb_pour", [](const VerbContext& ctx) {
            ctx.log("[Beaker] The chemistry system is not yet connected.");
        });
        // verb_seal: stasis bag stub
        vd.register_handler("verb_seal", [](const VerbContext& ctx) {
            ctx.log("[Stasis Bag] Not yet implemented.");
        });
        // verb_close: close container (mirrors verb_open toggle)
        vd.register_handler("verb_close", [](const VerbContext& ctx) {
            if (!ctx.item_stack) return;
            ctx.item_stack->container_open = false;
            ctx.log("[Close] You close the "
                + (ctx.item_def ? ctx.item_def->name : "container") + ".");
        });
        // verb_incise / verb_stab: surgery stubs
        vd.register_handler("verb_incise", [](const VerbContext& ctx) {
            ctx.log("[Scalpel] Surgery system not yet implemented.");
        });
        vd.register_handler("verb_stab", [](const VerbContext& ctx) {
            ctx.log("[Scalpel] You stab with the scalpel.");
        });

        // verb_melee: generic melee swing — feedback only.
        // Real damage is applied by attack_chain when the player left-clicks a mob
        // while holding the weapon (force:<N> tag drives the damage).
        vd.register_handler("verb_melee", [](const VerbContext& ctx) {
            const char* wname = (ctx.item_def ? ctx.item_def->name.c_str() : "weapon");
            char buf[128];
            std::snprintf(buf, sizeof(buf), "[Melee] You swing the %s.", wname);
            ctx.log(buf);
        });

        // verb_fire: fire a ranged weapon (gun, energy gun, bow, etc.)
        // Spawns a ProjectileComponent entity in the aimed direction.
        // Energy weapons use energy_max tag as ammo pool.
        // Ballistic guns (ammo_type: tag) require a physical magazine in magazine_slot.
        vd.register_handler("verb_fire",
            [&player_inv, &cam_pos, &cam_yaw, &cam_pitch,
             &projectile_sys](const VerbContext& ctx) {

            auto* hand = player_inv.active_hand();
            if (!hand || !hand->item || !hand->item->def) {
                ctx.log("[Fire] Nothing in hand."); return;
            }
            ItemStack&     gun = *hand->item;
            const ItemDef* def  = gun.def;

            // Must have the "gun" tag
            bool is_gun = false;
            for (const auto& t : def->tags) if (t == "gun") { is_gun = true; break; }
            if (!is_gun) { ctx.log("[Fire] That's not a firearm."); return; }

            // Determine ammo source: ballistic (magazine_slot) vs energy (ammo_remaining)
            std::string required_ammo_type;
            for (const auto& t : def->tags)
                if (t.rfind("ammo_type:", 0) == 0) { required_ammo_type = t.substr(10); break; }

            if (!required_ammo_type.empty()) {
                // ── Ballistic gun: requires a loaded physical magazine ────────
                if (gun.magazine_slot.empty()) {
                    ctx.log("[Fire] *click* No magazine loaded."); return;
                }
                ItemStack& mag = gun.magazine_slot[0];
                if (mag.ammo_remaining <= 0) {
                    ctx.log("[Fire] *click* Magazine empty. Eject and reload."); return;
                }
                mag.ammo_remaining--;
                // fall through to spawn projectile; log at end using mag.ammo_remaining
            } else {
                // ── Energy gun: lazy-init and decrement ammo_remaining ────────
                if (gun.ammo_remaining < 0) {
                    gun.ammo_remaining = 0;
                    for (const auto& t : def->tags) {
                        if (t.rfind("energy_max:", 0) == 0) {
                            try { gun.ammo_remaining = std::stoi(t.substr(11)); } catch (...) {}
                            break;
                        }
                    }
                }
                if (gun.ammo_remaining == 0) {
                    ctx.log("[Fire] *click* Out of energy."); return;
                }
                gun.ammo_remaining--;
            }

            // Parse projectile properties from tags
            float       proj_dmg   = 20.f;
            float       proj_speed = 30.f;
            float       proj_stun  = 0.f;
            float       proj_stam  = 0.f;
            std::string proj_type  = "bullet";
            for (const auto& t : def->tags) {
                if (t.rfind("proj_dmg:",   0) == 0) { try { proj_dmg   = std::stof(t.substr(9));  } catch (...) {} }
                if (t.rfind("proj_speed:", 0) == 0) { try { proj_speed = std::stof(t.substr(11)); } catch (...) {} }
                if (t.rfind("proj_stun:",  0) == 0) { try { proj_stun  = std::stof(t.substr(10)); } catch (...) {} }
                if (t.rfind("proj_stam:",  0) == 0) { try { proj_stam  = std::stof(t.substr(10)); } catch (...) {} }
                if (t.rfind("proj_type:",  0) == 0) { proj_type = t.substr(10); }
            }

            // Energy projectiles deal burn; bullets deal brute
            std::string dmg_type = (proj_type == "laser"    ||
                                    proj_type == "taser"    ||
                                    proj_type == "disabler") ? "burn" : "brute";

            // Build aim direction from current camera angles
            const float yr = glm::radians(cam_yaw);
            const float pr = glm::radians(cam_pitch);
            glm::vec3 dir{
                std::cos(pr) * std::sin(yr),
                std::sin(pr),
               -std::cos(pr) * std::cos(yr)
            };

            // Spawn projectile slightly in front of the muzzle (0.6 m)
            ProjectileComponent pc{};
            pc.direction   = glm::normalize(dir);
            pc.speed       = proj_speed;
            pc.damage      = proj_dmg;
            pc.damage_type = dmg_type;
            pc.owner       = ctx.actor;
            pc.stun_dur    = proj_stun;
            pc.stam_damage = proj_stam;
            projectile_sys.spawn(cam_pos + dir * 0.6f, pc);

            char buf[128];
            int ammo_left = required_ammo_type.empty()
                ? gun.ammo_remaining
                : (gun.magazine_slot.empty() ? 0 : gun.magazine_slot[0].ammo_remaining);
            std::snprintf(buf, sizeof(buf), "[Fire] You fire the %s. (%d rounds left)",
                def->name.c_str(), ammo_left);
            ctx.log(buf);
        });

        // verb_reload: insert a compatible magazine from the other hand into the gun.
        vd.register_handler("verb_reload",
            [&player_inv, &world_items, &cam_pos](const VerbContext& ctx) {

            auto* hand = player_inv.active_hand();
            if (!hand || !hand->item || !hand->item->def) {
                ctx.log("[Reload] Nothing in active hand."); return;
            }
            ItemStack&     gun = *hand->item;
            const ItemDef* def = gun.def;

            // Find the required ammo_type from the gun's tags
            std::string required_ammo_type;
            for (const auto& t : def->tags)
                if (t.rfind("ammo_type:", 0) == 0) { required_ammo_type = t.substr(10); break; }
            if (required_ammo_type.empty()) {
                ctx.log("[Reload] This weapon doesn't use magazines."); return;
            }

            if (!gun.magazine_slot.empty()) {
                ctx.log("[Reload] Magazine already loaded. Eject it first."); return;
            }

            // Find compatible magazine in the other hand
            const std::string& active_id = player_inv.active_hand_id();
            const char* other_id = (active_id == "l_hand") ? "r_hand" : "l_hand";
            auto* other_hand = player_inv.find_slot(other_id);
            if (!other_hand || !other_hand->item || !other_hand->item->def) {
                ctx.log("[Reload] Other hand is empty."); return;
            }

            ItemStack& mag_item = *other_hand->item;
            bool compatible = false;
            int  ammo_count = 0;
            for (const auto& t : mag_item.def->tags) {
                if (t == ("ammo_type:" + required_ammo_type)) compatible = true;
                if (t.rfind("ammo_count:", 0) == 0)
                    try { ammo_count = std::stoi(t.substr(11)); } catch (...) {}
            }
            if (!compatible) {
                ctx.log("[Reload] Wrong magazine type for this weapon."); return;
            }

            // Lazy-init ammo in the magazine if never set
            if (mag_item.ammo_remaining < 0)
                mag_item.ammo_remaining = ammo_count;

            gun.magazine_slot.push_back(std::move(mag_item));
            other_hand->item.reset();

            char buf[128];
            std::snprintf(buf, sizeof(buf), "[Reload] Magazine loaded. (%d/%d rounds)",
                gun.magazine_slot[0].ammo_remaining, ammo_count);
            ctx.log(buf);
        });

        // verb_eject_mag: remove the loaded magazine from the gun into the other hand
        // (or spawn at feet if the other hand is occupied).
        vd.register_handler("verb_eject_mag",
            [&player_inv, &world_items, &cam_pos](const VerbContext& ctx) {

            auto* hand = player_inv.active_hand();
            if (!hand || !hand->item || !hand->item->def) {
                ctx.log("[Eject] Nothing in active hand."); return;
            }
            ItemStack& gun = *hand->item;

            if (gun.magazine_slot.empty()) {
                ctx.log("[Eject] No magazine loaded."); return;
            }

            ItemStack mag = std::move(gun.magazine_slot[0]);
            gun.magazine_slot.clear();

            // Try to place in the other hand first, then any slot
            const std::string& active_id = player_inv.active_hand_id();
            const char* other_id = (active_id == "l_hand") ? "r_hand" : "l_hand";
            auto* other_hand = player_inv.find_slot(other_id);
            if (other_hand && !other_hand->item) {
                other_hand->item = std::move(mag);
                ctx.log("[Eject] Magazine ejected to other hand.");
            } else {
                auto leftover = player_inv.auto_equip(std::move(mag));
                if (leftover) {
                    world_items.spawn_floating(cam_pos, std::move(*leftover));
                    ctx.log("[Eject] Magazine ejected — no free slot, dropped to ground.");
                } else {
                    ctx.log("[Eject] Magazine ejected.");
                }
            }
        });
    }

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
            // Rotate raw mouse delta by -cam_roll so that mouse axes always align
            // with the camera's local up/right, not world up/right.
            // e.g. when rolled 90° (prone on side), mouse-up cranes the head up
            // (which is a yaw change in world space, not a pitch change).
            auto apply_look = [&](float mx, float my) {
                const float SENSITIVITY = game_settings.mouse_sensitivity;
                if (game_settings.invert_mouse_y) my = -my;

                // Counter-rotate input by -cam_roll (used by Dizzy; 0 when healthy).
                float cr = std::cos(glm::radians(-cam_roll));
                float sr = std::sin(glm::radians(-cam_roll));
                float eff_x = mx * cr - my * sr;
                float eff_y = mx * sr + my * cr;

                // Yaw: when prone, limit to ±90° from body direction (can't look backward).
                float new_yaw = cam_yaw + eff_x * SENSITIVITY;
                if (prone_t > 0.01f) {
                    float yaw_limit = glm::mix(360.f, 90.f, prone_t);
                    float delta = new_yaw - player_body_yaw;
                    // Normalise delta to [-180, 180]
                    while (delta >  180.f) delta -= 360.f;
                    while (delta < -180.f) delta += 360.f;
                    delta = glm::clamp(delta, -yaw_limit, yaw_limit);
                    new_yaw = player_body_yaw + delta;
                }
                cam_yaw = new_yaw;

                // Pitch limits: standing ±89°; prone allows same down range but tighter up.
                float new_pitch  = cam_pitch - eff_y * SENSITIVITY;
                float pitch_max  = glm::mix( 89.f,  70.f, prone_t);
                float pitch_min  = -89.f;
                cam_pitch = glm::clamp(new_pitch, pitch_min, pitch_max);
            };

            bool any_menu_open = creative_menu.is_open() || map_editor.is_open() || admin_menu.is_open() || pause_menu.is_open();
            if (!alt_mode.active() && !any_menu_open) {
                glm::vec2 mdelta = input.mouse_delta();
                apply_look(mdelta.x, mdelta.y);
                // Body tracks camera yaw only when upright — when prone the body
                // stays fixed so the yaw constraint has a stable reference.
                if (prone_t < 0.1f)
                    player_body_yaw = cam_yaw;
            } else if (third_person && !any_menu_open) {
                // Alt held in 3P: drag orbits camera around player without rotating the body
                glm::vec2 mdelta = input.mouse_delta();
                apply_look(mdelta.x, mdelta.y);
                // player_body_yaw intentionally NOT updated here
            }

            // C: toggle third-person camera
            {
                static bool s_cam_prev = false;
                bool c_now = input.is_held(Action::ToggleCamera);
                if (c_now && !s_cam_prev) {
                    third_person = !third_person;
                    game_settings.third_person = third_person;
                    if (!third_person) {
                        // Snap camera back to body direction when returning to 1P
                        cam_yaw = player_body_yaw;
                    }
                }
                s_cam_prev = c_now;
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
                // Z — toggle resting (lie down / stand up)
                if (input.consume_press(Action::Rest)) {
                    if (player != NULL_ENTITY) {
                        auto* cc_z = g_entities.get_component<CharacterControllerComponent>(player);
                        if (cc_z) {
                            // Write base_mob_state so the state survives the per-frame reset.
                            cc_z->base_mob_state = (cc_z->base_mob_state == MobState::Normal)
                                                   ? MobState::Resting
                                                   : MobState::Normal;
                            cc_z->mob_state = cc_z->base_mob_state;
                            SDL_Log("Rest: %s",
                                    cc_z->base_mob_state == MobState::Normal ? "standing" : "resting");
                        }
                    }
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
            MobState local_mob_state = MobState::Normal;
            if (player != NULL_ENTITY) {
                auto* cc_r = g_entities.get_component<CharacterControllerComponent>(player);
                if (cc_r) {
                    noclip_active   = cc_r->noclip;
                    zero_g_active   = cc_r->zero_g;
                    local_mob_state = cc_r->mob_state;

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
            // Hardcrit: fully immobile
            if (local_mob_state == MobState::Hardcrit) can_move = false;
            if (can_move) {
                if (input.is_held(Action::MoveForward)) wish += fwd;
                if (input.is_held(Action::MoveBack))    wish -= fwd;
                if (input.is_held(Action::MoveRight))   wish += right;
                if (input.is_held(Action::MoveLeft))    wish -= right;
            }
            // Resting / softcrit: crawl at reduced speed (half of walking)
            static constexpr float k_crawl_mult = 0.50f;
            if (local_mob_state == MobState::Resting ||
                local_mob_state == MobState::Softcrit) {
                wish *= k_crawl_mult;
            }

            // Submit movement input to server
            if (player != NULL_ENTITY) {
                InputSnapshot snap{};
                snap.wish_dir = wish;
                snap.yaw      = player_body_yaw;
                snap.sprint   = input.is_held(Action::Sprint);
                client.send_input(snap);
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

            // Client tick: process incoming network packets and update
            // replicated world state.  Dev tools: freeze pauses tick entirely;
            // slow-motion reduces dt.
            if (!freeze_sim && !pause_menu.is_open()) {
                double eff_dt = slow_motion ? dt * 0.1 : dt;
                client.tick(eff_dt);

                // Tick projectile system (client-side prediction: visual only)
                projectile_sys.tick(eff_dt, signals());

                // Enqueue chunks dirtied by incoming network data and update
                // lighting.  Full rebuild only on the first batch (initial
                // world load); subsequent changes use the incremental path.
                auto net_dirty = g_world.dirty_chunks();
                if (!net_dirty.empty()) {
                    static bool s_remote_lighting_built = false;
                    if (!s_remote_lighting_built) {
                        lighting.rebuild();
                        s_remote_lighting_built = true;
                    } else {
                        // Incremental: re-propagate only from emissive voxels
                        // inside the newly-arrived chunks.
                        std::vector<glm::ivec3> emissive_pos;
                        for (const Chunk* ch : net_dirty) {
                            glm::ivec3 origin = ch->chunk_pos()
                                              * static_cast<int>(CHUNK_SIZE);
                            for (int lz = 0; lz < CHUNK_SIZE; ++lz)
                            for (int ly = 0; ly < CHUNK_SIZE; ++ly)
                            for (int lx = 0; lx < CHUNK_SIZE; ++lx) {
                                const Voxel& v = ch->get(lx, ly, lz);
                                if (v.flags & VFLAG_LIGHT_SRC)
                                    emissive_pos.push_back(
                                        origin + glm::ivec3(lx, ly, lz));
                            }
                        }
                        if (!emissive_pos.empty())
                            lighting.update(emissive_pos);
                    }
                    mesher.enqueue_batch(net_dirty, g_world);
                    // Clear dirty flags so the same chunks aren't re-enqueued
                    for (Chunk* c : net_dirty) c->clear_dirty();
                }
            }

            // ── Zero-G: anywhere outside a pressurised room is zero-G ──────────
            // Untracked cells (null zone) = open space → zero-G ON.
            // Only cells inside an explicit non-space atmos zone have gravity.
            if (player != NULL_ENTITY) {
                auto* cc2 = g_entities.get_component<CharacterControllerComponent>(player);
                auto* tr2 = g_entities.get_component<TransformComponent>(player);
                if (cc2 && tr2) {
                    bool in_space;
                    if (zerog_override) {
                        in_space = true;
                    } else {
                        glm::ivec3 cell = {
                            static_cast<int>(std::floor(tr2->pos.x)),
                            static_cast<int>(std::floor(tr2->pos.y)),
                            static_cast<int>(std::floor(tr2->pos.z))
                        };
                        AtmosZoneID az        = (g_atmos ? g_atmos->zone_at(cell) : AtmosZoneID{});
                        const AtmosZone* zptr = (g_atmos ? g_atmos->zone(az) : nullptr);
                        // In space if: no tracked zone (open void) OR zone is explicitly space
                        in_space = !zptr || zptr->is_space;
                    }
                    if (in_space != cc2->zero_g) {
                        cc2->zero_g = in_space;
                        SDL_Log("Zero-G: %s", in_space ? "entered space" : "left space");
                    }
                }
            }

            // ── Dev-tool: auto-heal & infinite oxygen ────────────────────────────
            if ((auto_heal || infinite_oxy) && player != NULL_ENTITY) {
                auto* hp = g_entities.get_component<HealthComponent>(player);
                if (hp) {
                    if (auto_heal) {
                        // Clear all damage buckets + stamina, resync crit/dead flags
                        hp->brute = hp->burn = hp->tox = hp->oxy = hp->clone = 0.f;
                        hp->stam_damage = 0.f;
                        hp->stam_ko = false;
                        hp->recalculate();
                    } else if (infinite_oxy) {
                        hp->oxy = 0.f;
                        hp->recalculate(); // re-check dead/crit after oxy cleared
                    }
                }
            }

            // Sync camera to physics-driven player position
            // Also sync the player body yaw to the camera horizontal direction so
            // the body always faces where the player is looking.
            if (player != NULL_ENTITY) {
                auto* tr = g_entities.get_component<TransformComponent>(player);
                if (tr) {
                    float yaw_r      = glm::radians(cam_yaw);
                    float pitch_r    = glm::radians(cam_pitch);
                    // Use body yaw for head offset so it tracks the sprite, not the camera.
                    float body_yaw_r = glm::radians(player_body_yaw);
                    glm::vec3 body_horiz_fwd = { std::sin(body_yaw_r), 0.f, -std::cos(body_yaw_r) };

                    tr->yaw = player_body_yaw;  // body faces its own direction, not the orbit angle

                    // Eye height: drops from face-height (0.50 m) to crawl-height (0.14 m).
                    // 0.14 keeps the camera above floor geometry without Z-fighting.
                    float eye_h = glm::mix(0.50f, 0.14f, prone_t);

                    // Head forward offset along body direction:
                    // upright: slight 0.05 m offset (centred on body).
                    // prone:   0.50 m forward — head is in front of the body origin.
                    float fwd_offset = glm::mix(0.05f, 0.50f, prone_t);

                    if (third_person) {
                        glm::vec3 cam_fwd3 = {
                            std::cos(pitch_r) * std::sin(yaw_r),
                            std::sin(pitch_r),
                           -std::cos(pitch_r) * std::cos(yaw_r)
                        };
                        glm::vec3 eye = tr->pos + glm::vec3(0.f, eye_h, 0.f)
                                      + body_horiz_fwd * fwd_offset;
                        cam_pos = eye - cam_fwd3 * k_3p_dist;
                    } else {
                        cam_pos = tr->pos + glm::vec3(0.f, eye_h, 0.f)
                                + body_horiz_fwd * fwd_offset;
                    }
                }
            }

            // ── View bob & sway: advance cycle from player velocity ─────────
            {
                float horiz_speed = 0.f;
                bool  grounded    = false;
                if (player != NULL_ENTITY) {
                    auto* vc3  = g_entities.get_component<VelocityComponent>(player);
                    auto* cc3  = g_entities.get_component<CharacterControllerComponent>(player);
                    if (vc3)  horiz_speed = glm::length(glm::vec2(vc3->linear.x, vc3->linear.z));
                    if (cc3)  grounded    = cc3->on_ground;
                }

                // Blend weight: ramp up/down quickly when starting/stopping.
                // Cap lower when sprinting so the faster cycle stays subtle.
                constexpr float BOB_FULL_SPEED   = 4.5f;   // m/s that gives blend=1 while walking
                constexpr float BOB_SPRINT_CAP   = 0.2f;  // max blend while sprinting
                bool is_sprinting = false;
                if (player != NULL_ENTITY) {
                    auto* cc3s = g_entities.get_component<CharacterControllerComponent>(player);
                    if (cc3s) is_sprinting = cc3s->sprinting;
                }
                float blend_max    = is_sprinting ? BOB_SPRINT_CAP : 1.f;
                float target_blend = (!third_person && grounded)
                                   ? glm::clamp(horiz_speed / BOB_FULL_SPEED, 0.f, blend_max) : 0.f;
                float blend_alpha  = 1.f - std::exp(-8.f * static_cast<float>(dt));
                bob_blend = bob_blend + (target_blend - bob_blend) * blend_alpha;

                // Advance bob cycle proportional to distance walked
                constexpr float BOB_FREQ = 7.0f;  // radians per metre
                if (!third_person && grounded)
                    bob_time += horiz_speed * static_cast<float>(dt) * BOB_FREQ;

                // Quake-style strafe sway: lean slightly in strafe direction
                float strafe_input = 0.f;
                if (!third_person) {
                    if (input.is_held(Action::MoveRight)) strafe_input += 1.f;
                    if (input.is_held(Action::MoveLeft))  strafe_input -= 1.f;
                }
                float sway_target = strafe_input * 1.5f;  // max ±1.5 degree yaw nudge
                float sway_alpha  = 1.f - std::exp(-5.f * static_cast<float>(dt));
                bob_sway = bob_sway + (sway_target - bob_sway) * sway_alpha;
            }

            // Upload any finished chunk meshes in one batched GPU submission
            auto finished_meshes = mesher.collect_finished();
            renderer.upload_meshes_batch(finished_meshes);
            for (auto& mesh : finished_meshes)
                renderer.get_or_create_mesh(mesh.chunk_pos) = std::move(mesh);

            // Collect + upload finished vehicle chunk meshes
            if (test_vehicle) {
                auto vm = vehicle_mesher.collect_finished();
                if (!vm.empty())
                    renderer.upload_vehicle_meshes_batch(test_vehicle->id(), vm);
            }

            // Lighting update for dirtied chunks
            // lighting.update(…)

            // Audio listener
            glm::vec3 fwd3 = {std::sin(glm::radians(cam_yaw)),
                               std::sin(glm::radians(cam_pitch)),
                              -std::cos(glm::radians(cam_yaw))};
            audio.set_listener(cam_pos, glm::normalize(fwd3), {0,1,0});
            audio.set_master_volume(game_settings.master_volume);
            audio.set_sfx_volume(game_settings.sfx_volume);
            // Read actual gas pressure at player position for audio
            {
                glm::ivec3 atmos_cell = {
                    static_cast<int>(std::floor(cam_pos.x)),
                    static_cast<int>(std::floor(cam_pos.y)),
                    static_cast<int>(std::floor(cam_pos.z))
                };
                AtmosZoneID az = (g_atmos ? g_atmos->zone_at(atmos_cell) : AtmosZoneID{});
                AtmosZone* az_ptr = (g_atmos ? g_atmos->zone(az) : nullptr);
                float local_kpa = az_ptr ? az_ptr->gas.total_pressure() : 0.f;
                audio.set_local_pressure(local_kpa > 0.f ? local_kpa : 101.325f);
            }
            audio.update(static_cast<float>(dt));

            // Update HUD state — format elapsed round time as MM:SS
            {
                int total_secs = static_cast<int>(sim_time);
                int mm = total_secs / 60;
                int ss = total_secs % 60;
                char buf[8];
                SDL_snprintf(buf, sizeof(buf), "%02d:%02d", mm, ss);
                hud_state.clock_str = buf;
            }
            hud_state.cam_pitch          = cam_pitch;
            hud_state.active_hand_is_left = (player_inv.active_hand_id() == "l_hand");
            hud_state.is_running          = input.is_held(Action::Sprint);

            // ── Prone transition: animate prone_t 0→1 when mob_state != Normal ─
            {
                bool is_prone = false;
                if (player != NULL_ENTITY) {
                    auto* cc_p = g_entities.get_component<CharacterControllerComponent>(player);
                    if (cc_p && cc_p->mob_state != MobState::Normal)
                        is_prone = true;
                }
                float prone_target = is_prone ? 1.f : 0.f;
                // Stand-up is slightly slower than lying down for a weightier feel.
                float prone_rate  = is_prone ? 8.f : 6.f;
                float prone_alpha = 1.f - std::exp(-prone_rate * static_cast<float>(dt));
                prone_t = glm::clamp(prone_t + (prone_target - prone_t) * prone_alpha, 0.f, 1.f);
            }

            // ── Player health from HealthComponent ───────────────────────────
            if (player != NULL_ENTITY) {
                auto* hp = g_entities.get_component<HealthComponent>(player);
                if (hp) {
                    hud_state.health     = hp->current();
                    hud_state.health_max = hp->health_max;
                    hud_state.dmg_brute  = hp->brute;
                    hud_state.dmg_burn   = hp->burn;
                    hud_state.dmg_tox    = hp->tox;
                    hud_state.dmg_oxy    = hp->oxy;
                    hud_state.dmg_clone  = hp->clone;
                    hud_state.dead       = hp->dead;
                    hud_state.in_crit    = hp->crit;

                    // Use HealthComponent flags — thresholds match TG (HEALTH_THRESHOLD_CRIT=50)
                    auto* cc_hp = g_entities.get_component<CharacterControllerComponent>(player);
                    if (cc_hp) {
                        MobState cs = cc_hp->mob_state;
                        if (hp->dead) {
                            // Dead — fully incapacitated
                            cc_hp->mob_state = MobState::Hardcrit;
                        } else if (hp->crit && cs == MobState::Normal) {
                            // Softcrit: health <= 50 (TG: HEALTH_THRESHOLD_CRIT) — barely conscious
                            cc_hp->mob_state = MobState::Softcrit;
                        } else if (!hp->crit && !hp->dead &&
                                   (cs == MobState::Softcrit || cs == MobState::Hardcrit)) {
                            // Healed out of crit — restore to voluntary base state so that
                            // a player who was Resting doesn't snap to standing on recovery.
                            cc_hp->mob_state = cc_hp->base_mob_state;
                        }
                    }
                }
            }

            // ── Suit sensors: read atmos at player feet ───────────────────────
            {
                glm::ivec3 feet_cell = {
                    static_cast<int>(std::floor(cam_pos.x)),
                    static_cast<int>(std::floor(cam_pos.y - 0.5f)),
                    static_cast<int>(std::floor(cam_pos.z))
                };
                AtmosZoneID   az      = (g_atmos ? g_atmos->zone_at(feet_cell) : AtmosZoneID{});
                const AtmosZone* zptr = (g_atmos ? g_atmos->zone(az) : nullptr);
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
                    // Use player's actual world position (feet level) for drop position
                    glm::vec3 drop_origin = cam_pos;
                    if (player != NULL_ENTITY) {
                        auto* tr = g_entities.get_component<TransformComponent>(player);
                        if (tr) drop_origin = tr->pos;
                    }
                    // Cast downward from player feet to find a floor face
                    RayHit down = g_world.raycast(drop_origin, {0,-1,0}, 3.f);
                    if (down.valid) {
                        // Drop with scatter so items don't all pile at turf centre
                        world_items.spawn_scattered(down.voxel, down.face, std::move(*maybe_item));
                    } else {
                        // No floor nearby – spawn floating at player feet
                        world_items.spawn_floating(
                            drop_origin, std::move(*maybe_item));
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
                    // Resolve per-group context (vehicle or main world)
                    World&          grp_world  = grp.ctx_world  ? *grp.ctx_world  : g_world;
                    AtmosSimulator* grp_atmos_p = grp.ctx_atmos ? grp.ctx_atmos : g_atmos;
                    ChunkMesher&    grp_mesher = grp.ctx_mesher ? *grp.ctx_mesher : mesher;
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
                                    grp_world.set_voxel(p, open_v);
                                grp_mesher.enqueue_batch(grp_world.dirty_chunks(), grp_world);
                            }
                            // Door is now gas-passable — notify atmos so zones merge.
                            if (!grp.voxels.empty() && grp_atmos_p)
                                grp_atmos_p->on_door_changed(grp.voxels.front());
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
                                    grp_world.set_voxel(p, closed_v);
                                grp_mesher.enqueue_batch(grp_world.dirty_chunks(), grp_world);
                            }
                            // Door is now fully sealed — notify atmos to finalise zone split.
                            if (!grp.voxels.empty() && grp_atmos_p)
                                grp_atmos_p->on_door_changed(grp.voxels.front());
                            it = animating_doors.erase(it);
                            continue;
                        }
                    }

                    last_frame = grp.frame;
                    ++it;
                }
                if (last_frame >= 0) {
                    renderer.update_tile_layer(door_anim_type_id,
                                               renderer.door_anim_frame_pixels(last_frame));
                    // Composite and upload the glass overlay for this frame:
                    // RGB from glass_opening.gif + alpha from fill_opening.gif coverage mask
                    // so the overlay is semi-transparent throughout the animation.
                    if (door_anim_overlay_idx != 0 && renderer.door_overlay_anim_loaded())
                        renderer.composite_door_overlay_frame(last_frame, door_anim_overlay_idx);
                }
            }

            // -- Physics gun: grab / fling the test vehicle ------------------
            {
                auto* ps = player_inv.active_hand();
                bool has_phy = ps && ps->item && ps->item->def
                               && ps->item->def->id == "physics_tool";
                if (!has_phy) phy_holding = false;

                if (has_phy && test_vehicle) {
                    float pyr = glm::radians(cam_yaw), ppr = glm::radians(cam_pitch);
                    glm::vec3 prdir = {
                        std::cos(ppr) * std::sin(pyr),
                        std::sin(ppr),
                       -std::cos(ppr) * std::cos(pyr)
                    };
                    if (phy_holding) {
                        glm::vec3 centre = test_vehicle->world_pos_f() + glm::vec3{2.f,1.5f,4.f};
                        test_vehicle->set_world_vel((cam_pos + prdir*phy_hold_dist - centre) * 8.f);
                    }
                    bool phy_lmb = !alt_mode.active() && !build_mode
                                   && !ctx_menu.is_open() && !creative_menu.is_open()
                                   && input.is_pressed(Action::PrimaryInteract);
                    if (phy_lmb) {
                        if (phy_holding) {
                            test_vehicle->set_world_vel(prdir * 18.f);
                            if (hud_state.radio_log.size() >= 30) hud_state.radio_log.pop_front();
                            hud_state.radio_log.push_back("[Physics Gun] Flung!");
                            phy_holding = false;
                        } else {
                            glm::vec3 centre = test_vehicle->world_pos_f() + glm::vec3{2.f,1.5f,4.f};
                            glm::vec3 to_v   = centre - cam_pos;
                            float     dist   = glm::length(to_v);
                            if (dist > 0.f && dist <= 20.f &&
                                glm::dot(glm::normalize(to_v), prdir) >= 0.4f) {
                                phy_holding   = true;
                                phy_hold_dist = glm::clamp(dist, 3.0f, 12.0f);
                                test_vehicle->set_world_vel({0.f, 0.f, 0.f});
                                if (hud_state.radio_log.size() >= 30) hud_state.radio_log.pop_front();
                                hud_state.radio_log.push_back("[Physics Gun] Grabbed -- LMB to fling!");
                            }
                        }
                        input.consume_press(Action::PrimaryInteract);
                    }
                }
            }

            // ── LMB / E: interact with world based on active hand ─────────────
            // Empty hand  + item  → pick up
            // Empty hand  + turf  → nothing (can't pick up turf)
            // Empty hand  + mob   → punch / interact
            // Held item   + item  → item-on-item interaction
            // Held item   + mob   → attack_chain (TG SS13 click path)
            // Held item   + turf  → knock on the wall with held item
            {
                bool fps_lmb = !alt_mode.active() && !build_mode && !ctx_menu.is_open() && !creative_menu.is_open() && input.is_pressed(Action::PrimaryInteract);
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

                    RayHit fhit = g_world.raycast(cam_pos, rdir, ITEM_REACH);
                    float item_dist = 0.f;
                    EntityID item_ent = world_items.ray_cast_items(
                        cam_pos, rdir, ITEM_REACH,
                        fhit.valid ? fhit.distance : ITEM_REACH, item_dist);

                    // ── Vehicle world dual-raycast ────────────────────────────
                    // Determine which world (main or vehicle) the player is
                    // targeting; all subsequent interaction code uses active_*.
                    World*           active_world  = &g_world;
                    AtmosSimulator*  active_atmos  = g_atmos;
                    ChunkMesher*     active_mesher = &mesher;
                    WorldItemSystem* active_wis    = &world_items;
                    glm::vec3        active_offset {};
                    if (test_vehicle) {
                        glm::vec3 veh_off  = test_vehicle->world_pos_f();
                        glm::vec3 local_cam = cam_pos - veh_off;
                        RayHit v_fhit = test_vehicle->world().raycast(local_cam, rdir, ITEM_REACH);
                        float v_item_dist = 0.f;
                        EntityID v_item_ent = test_vehicle->world_items().ray_cast_items(
                            local_cam, rdir, ITEM_REACH,
                            v_fhit.valid ? v_fhit.distance : ITEM_REACH, v_item_dist);
                        // Switch context if vehicle hit is closer
                        bool veh_closer = v_fhit.valid && (!fhit.valid || v_fhit.distance < fhit.distance);
                        bool veh_item_closer = (v_item_ent != NULL_ENTITY)
                            && (item_ent == NULL_ENTITY || v_item_dist < item_dist);
                        if (veh_closer || veh_item_closer) {
                            active_world  = &test_vehicle->world();
                            active_atmos  = &test_vehicle->atmos();
                            active_mesher = &vehicle_mesher;
                            active_wis    = &test_vehicle->world_items();
                            active_offset = veh_off;
                            if (veh_closer)      { fhit      = v_fhit;      }
                            if (veh_item_closer) { item_ent  = v_item_ent;
                                                   item_dist = v_item_dist; }
                        }
                    }

                    // ── Mob target detection (TG attack_chain) ────────────────
                    // Simple nearest-entity ray test: find any mob with MobTypeTag
                    // whose centre is within MELEE_REACH of cam_pos and within ±45°
                    // of the ray direction (cone test).
                    EntityID mob_target = NULL_ENTITY;
                    float    mob_target_dist = MELEE_REACH;
                    if (player != NULL_ENTITY) {
                        g_entities.each<MobTypeTag>([&](EntityID eid, MobTypeTag&) {
                            if (eid == player) return;
                            auto* tr = g_entities.get_component<TransformComponent>(eid);
                            if (!tr) return;
                            // Use mob centre (waist height ≈ +0.5)
                            glm::vec3 mob_centre = tr->pos + glm::vec3(0.f, 0.5f, 0.f);
                            glm::vec3 to_mob = mob_centre - cam_pos;
                            float dist = glm::length(to_mob);
                            if (dist > MELEE_REACH || dist <= 0.f) return;
                            // Cone check: dot with ray dir > 0.7 (~45°)
                            if (glm::dot(glm::normalize(to_mob), rdir) < 0.7f) return;
                            if (dist < mob_target_dist) {
                                mob_target = eid;
                                mob_target_dist = dist;
                            }
                        });
                    }

                    // ── Attack chain (LMB on mob) ──────────────────────────────
                    if (fps_lmb && mob_target != NULL_ENTITY
                        && (item_ent == NULL_ENTITY || mob_target_dist < item_dist)) {

                        // ── Medical item use-on-mob (overrides attack) ─────────
                        // Holding a medical item + looking at a mob → apply it
                        // rather than attacking.  Intent "Help" is assumed when
                        // the item has the "medical" tag and the target is alive
                        // or revivable.
                        bool used_as_medicine = false;
                        if (!hand_empty && active_slot->item->def) {
                            const ItemDef* held_def = active_slot->item->def;
                            bool has_medical_tag = false;
                            for (const auto& t : held_def->tags)
                                if (t == "medical") { has_medical_tag = true; break; }

                            if (has_medical_tag && !held_def->verbs.empty()) {
                                // Find the primary action verb (first non-Examine verb)
                                const std::string* primary_handler = nullptr;
                                for (const auto& v : held_def->verbs) {
                                    if (v.handler != "verb_examine") {
                                        primary_handler = &v.handler;
                                        break;
                                    }
                                }
                                if (primary_handler && verb_dispatch().has(*primary_handler)) {
                                    VerbContext vctx;
                                    vctx.actor      = player;
                                    vctx.actor_pos  = cam_pos;
                                    vctx.target_ent = mob_target;
                                    vctx.item_def   = active_slot->item->def;
                                    vctx.item_stack = &(*active_slot->item);
                                    vctx.hud_log    = &hud_state.radio_log;
                                    verb_dispatch().invoke(*primary_handler, vctx);
                                    used_as_medicine = true;
                                }
                            }
                        }

                        if (!used_as_medicine) {
                        // Build attack context
                        AttackContext ctx;
                        ctx.attacker  = player;
                        ctx.target    = mob_target;
                        ctx.click_pos = cam_pos + rdir * mob_target_dist;
                        ctx.reach     = MELEE_REACH + 0.1f; // small tolerance
                        if (!hand_empty && active_slot->item->def) {
                            ctx.weapon = &(*active_slot->item);
                        }
                        AttackResult ar = attack_chain(ctx, g_entities, signals());
                        (void)ar;
                        // Log + audio feedback
                        const char* result_str = [&]{
                            switch(ar) {
                                case AttackResult::Missed:         return "missed";
                                case AttackResult::HitMob:         return "hit";
                                case AttackResult::BlockedByItem:  return "blocked";
                                case AttackResult::Disarmed:       return "disarmed";
                                default:                           return "?";
                            }
                        }();
                        SDL_Log("Attack: %s target=%u (%.2f brute) → %s",
                            hand_empty ? "fist" : active_slot->item->def->name.c_str(),
                            mob_target, ctx.damage_dealt, result_str);
                        if (ctx.hit)
                            audio.play("click", cam_pos);
                        } // end !used_as_medicine
                    } else if (item_ent != NULL_ENTITY) {
                        if (hand_empty || e_press) {
                            // Pick up — auto_equip tries all slots; re-spawn if no room
                            auto picked = active_wis->pick_up(item_ent);
                            if (picked && picked->def) {
                                auto leftover = player_inv.auto_equip(std::move(*picked));
                                if (leftover)
                                    world_items.spawn_floating(cam_pos, std::move(*leftover));
                            }
                        } else {
                            // Use held item on world item
                            // entity could be in either main or vehicle entity manager
                            auto* wic = active_world == &g_world
                                ? g_entities.get_component<WorldItemComponent>(item_ent)
                                : (test_vehicle ? test_vehicle->entities().get_component<WorldItemComponent>(item_ent) : nullptr);
                            if (wic && wic->item.def) {
                                SDL_Log("Interact: %s on %s",
                                        active_slot->item->def->name.c_str(),
                                        wic->item.def->name.c_str());
                                audio.play("click", cam_pos);
                            }
                        }
                    } else if (fhit.valid) {
                        Voxel hit_v = active_world->get_voxel(fhit.voxel);
                        bool has_id = active_slot && active_slot->item && active_slot->item->def
                            && [&]{
                                for (auto& t : active_slot->item->def->tags)
                                    if (t == "id_card") return true;
                                return false;
                            }();

                        // ── Open door ───────────────────────────────────
                        if (door_type_id != 0 && hit_v.type_id == door_type_id
                            && (hand_empty || has_id)) {
                            auto grp_voxels = flood_fill_door(*active_world, fhit.voxel, door_type_id);
                            if (!grp_voxels.empty() && door_anim_type_id != 0) {
                                Voxel anim_v;
                                anim_v.type_id = door_anim_type_id;
                                anim_v.flags   = VFLAG_SOLID | VFLAG_VERT_PLANE_Z;
                                for (auto& p : grp_voxels)
                                    active_world->set_voxel(p, anim_v);
                                active_mesher->enqueue_batch(active_world->dirty_chunks(), *active_world);
                                DoorGroup dg;
                                dg.voxels = std::move(grp_voxels);
                                if (active_world != &g_world) {
                                    dg.ctx_world  = active_world;
                                    dg.ctx_atmos  = active_atmos;
                                    dg.ctx_mesher = active_mesher;
                                }
                                animating_doors.push_back(std::move(dg));
                                audio.play("click", glm::vec3(fhit.voxel) + glm::vec3(0.5f));
                                SDL_Log("Door: opening at (%d,%d,%d)",
                                        fhit.voxel.x, fhit.voxel.y, fhit.voxel.z);
                            }
                        // ── Close door ───────────────────────────────────
                        } else if (door_open_type_id != 0
                                   && hit_v.type_id == door_open_type_id
                                   && (hand_empty || has_id)) {
                            auto grp_voxels = flood_fill_door(*active_world, fhit.voxel, door_open_type_id);
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
                                        active_world->set_voxel(p, anim_v);
                                    active_mesher->enqueue_batch(active_world->dirty_chunks(), *active_world);
                                    // Door panels are now solid — trigger an immediate zone
                                    // split so the sealed side stops losing gas to space
                                    // even before the closing animation completes.
                                    if (active_atmos) active_atmos->on_door_changed(fhit.voxel);
                                    DoorGroup dg;
                                    dg.voxels  = std::move(grp_voxels);
                                    dg.frame   = renderer.door_anim_frame_count() - 1;
                                    dg.closing = true;
                                    if (active_world != &g_world) {
                                        dg.ctx_world  = active_world;
                                        dg.ctx_atmos  = active_atmos;
                                        dg.ctx_mesher = active_mesher;
                                    }
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
                RayHit fhit2 = g_world.raycast(cam_pos, rdir2, CTX_REACH);
                float ict_dist = 0.f;
                EntityID ctx_ent = world_items.ray_cast_items(
                    cam_pos, rdir2, CTX_REACH,
                    fhit2.valid ? fhit2.distance : CTX_REACH, ict_dist);
                SDL_Log("CTX RMB: alt=%d build=%d open=%d ent=%u dist=%.2f",
                    (int)alt_mode.active(), (int)build_mode, (int)ctx_menu.is_open(),
                    (unsigned)ctx_ent, ict_dist);

                // ── Mob target detection for RMB context menu ─────────────────
                // Same cone test used for LMB melee, but we extend to CTX_REACH
                // so the player can examine or grab mobs from further away.
                EntityID rmb_mob = NULL_ENTITY;
                float    rmb_mob_dist = CTX_REACH;
                if (player != NULL_ENTITY) {
                    g_entities.each<MobTypeTag>([&](EntityID eid, MobTypeTag&) {
                        if (eid == player) return;
                        auto* tr = g_entities.get_component<TransformComponent>(eid);
                        if (!tr) return;
                        glm::vec3 to_mob = (tr->pos + glm::vec3(0.f, 0.5f, 0.f)) - cam_pos;
                        float dist = glm::length(to_mob);
                        if (dist > CTX_REACH || dist <= 0.f) return;
                        if (glm::dot(glm::normalize(to_mob), rdir2) < 0.7f) return;
                        if (dist < rmb_mob_dist) { rmb_mob = eid; rmb_mob_dist = dist; }
                    });
                }

                // ── Mob context menu (takes priority over world item if nearer) ─
                if (rmb_mob != NULL_ENTITY
                    && (ctx_ent == NULL_ENTITY || rmb_mob_dist <= ict_dist)) {
                    // Collect data from components before entering lambdas
                    auto* mob_nc = g_entities.get_component<NameComponent>(rmb_mob);
                    auto* mob_hp = g_entities.get_component<HealthComponent>(rmb_mob);
                    auto* mob_se = g_entities.get_component<StatusEffectsComponent>(rmb_mob);
                    auto* mob_gc = g_entities.get_component<GrabbedComponent>(rmb_mob);
                    auto* mob_co = g_entities.get_component<CorpseComponent>(rmb_mob);
                    auto* mob_dg = g_entities.get_component<DragComponent>(rmb_mob);

                    std::string mob_name = mob_nc ? mob_nc->name : "Unknown";
                    std::string mob_desc = mob_nc ? mob_nc->desc : "";
                    float hp_cur   = mob_hp ? mob_hp->current()  : 0.f;
                    float hp_max   = mob_hp ? mob_hp->health_max : 100.f;
                    bool  is_dead  = mob_hp && mob_hp->dead;
                    bool  is_stun  = mob_se && mob_se->is_stunned();
                    bool  is_kd    = mob_se && mob_se->is_knocked_down();
                    bool already_grabbed     = mob_gc && mob_gc->grabber == player;
                    bool dragged_by_me       = mob_dg && mob_dg->dragger == player;
                    float corpse_age         = mob_co ? mob_co->time_since_death : 0.f;
                    bool  can_revive         = is_dead && (!mob_co || mob_co->can_be_revived);
                    std::string cause_str    = mob_co ? mob_co->cause_of_death : "";
                    EntityID rmb_mob_id = rmb_mob;

                    std::vector<ContextEntry> entries;

                    // ── Examine ────────────────────────────────────────────────
                    entries.push_back({"Examine", true, false,
                        [&hud_state, mob_name, mob_desc,
                         hp_cur, hp_max, is_dead, is_stun, is_kd,
                         corpse_age, cause_str, can_revive]() {
                            const char* hp_label =
                                is_dead  ? "dead" :
                                hp_cur <= hp_max * 0.25f ? "critically injured" :
                                hp_cur <= hp_max * 0.5f  ? "severely injured"   :
                                hp_cur <= hp_max * 0.75f ? "injured"            :
                                                           "healthy";
                            const char* stun_str = is_stun ? " [STUNNED]" : (is_kd ? " [DOWNED]" : "");
                            char buf[320];
                            if (is_dead) {
                                int age_min = static_cast<int>(corpse_age) / 60;
                                int age_sec = static_cast<int>(corpse_age) % 60;
                                std::snprintf(buf, sizeof(buf),
                                    "[Examine] %s \xe2\x80\x94 dead for %dm%ds%s, cause: %s.",
                                    mob_name.c_str(), age_min, age_sec,
                                    can_revive ? " [REVIVABLE]" : " [BEYOND SAVING]",
                                    cause_str.empty() ? "unknown" : cause_str.c_str());
                            } else {
                                std::snprintf(buf, sizeof(buf),
                                    "[Examine] %s \xe2\x80\x94 %s (%.0f/%.0f)%s%s",
                                    mob_name.c_str(), hp_label,
                                    hp_cur, hp_max, stun_str,
                                    mob_desc.empty() ? "" : (" | " + mob_desc).c_str());
                            }
                            if (hud_state.radio_log.size() >= 30)
                                hud_state.radio_log.pop_front();
                            hud_state.radio_log.push_back(buf);
                        }});

                    // ── Use held medical item on mob ──────────────────────────
                    {
                        auto* hand_slot_ctx = player_inv.active_hand();
                        bool  hand_has_item = hand_slot_ctx && hand_slot_ctx->item
                                              && hand_slot_ctx->item->def;
                        if (hand_has_item) {
                            const ItemDef* hdef = hand_slot_ctx->item->def;
                            bool is_med = false;
                            for (const auto& t : hdef->tags)
                                if (t == "medical") { is_med = true; break; }
                            if (is_med && !hdef->verbs.empty()) {
                                // Primary verb (first non-Examine)
                                const ItemVerb* pverb = nullptr;
                                for (const auto& v : hdef->verbs)
                                    if (v.handler != "verb_examine") { pverb = &v; break; }
                                if (pverb && verb_dispatch().has(pverb->handler)) {
                                    std::string vhnd    = pverb->handler;
                                    std::string vlabel  = "Use " + hdef->name;
                                    entries.push_back({"", false, true, nullptr}); // separator
                                    entries.push_back({vlabel, true, false,
                                        [vhnd, rmb_mob_id, player, &cam_pos,
                                         &player_inv, &hud_state, &g_entities]() {
                                            auto* hs = player_inv.active_hand();
                                            VerbContext vctx;
                                            vctx.actor      = player;
                                            vctx.actor_pos  = cam_pos;
                                            vctx.target_ent = rmb_mob_id;
                                            if (hs && hs->item) {
                                                vctx.item_def   = hs->item->def;
                                                vctx.item_stack = &(*hs->item);
                                            }
                                            vctx.hud_log = &hud_state.radio_log;
                                            verb_dispatch().invoke(vhnd, vctx);
                                        }});
                                }
                            }
                        }
                    }

                    // ── Grab / Upgrade Grab ────────────────────────────────────
                    if (!is_dead) {
                        const char* grab_label = already_grabbed ? "Upgrade Grab" : "Grab";
                        entries.push_back({grab_label, true, false,
                            [&g_entities, rmb_mob_id, player, &hud_state]() {
                                bool ok = upgrade_grab(player, rmb_mob_id,
                                                       g_entities, signals());
                                if (hud_state.radio_log.size() >= 30)
                                    hud_state.radio_log.pop_front();
                                hud_state.radio_log.push_back(
                                    ok ? "[Grab] Grab upgraded." : "[Grab] Already at max.");
                            }});
                    }

                    // ── Resist (player is being grabbed by this mob) ────────────
                    {
                        auto* self_gc = g_entities.get_component<GrabbedComponent>(player);
                        if (self_gc && self_gc->grabber == rmb_mob) {
                            EntityID mob_id2 = rmb_mob;
                            entries.push_back({"Resist", true, false,
                                [&g_entities, mob_id2, player, &hud_state]() {
                                    release_grab(mob_id2, player,
                                                 g_entities, signals());
                                    if (hud_state.radio_log.size() >= 30)
                                        hud_state.radio_log.pop_front();
                                    hud_state.radio_log.push_back("[Resist] Broke free!");
                                }});
                        }
                    }

                    // ── Release Grab (if we're grabbing the mob) ───────────────
                    if (already_grabbed) {
                        entries.push_back({"Release", true, false,
                            [&g_entities, rmb_mob_id, player, &hud_state]() {
                                release_grab(player, rmb_mob_id,
                                             g_entities, signals());
                                if (hud_state.radio_log.size() >= 30)
                                    hud_state.radio_log.pop_front();
                                hud_state.radio_log.push_back("[Grab] Released.");
                            }});
                    }

                    // ── Corpse interactions (Drag / CPR) ──────────────────────
                    if (is_dead) {
                        entries.push_back({"", false, true, nullptr}); // separator

                        // Drag / Drop body
                        if (!dragged_by_me) {
                            entries.push_back({"Drag Body", true, false,
                                [&g_entities, rmb_mob_id, player, &hud_state]() {
                                    g_entities.remove_component<DragComponent>(rmb_mob_id);
                                    DragComponent dc{};
                                    dc.dragger = player;
                                    g_entities.add_component<DragComponent>(rmb_mob_id, dc);
                                    if (hud_state.radio_log.size() >= 30)
                                        hud_state.radio_log.pop_front();
                                    hud_state.radio_log.push_back("[Drag] Dragging body.");
                                }});
                        } else {
                            entries.push_back({"Drop Body", true, false,
                                [&g_entities, rmb_mob_id, &hud_state]() {
                                    g_entities.remove_component<DragComponent>(rmb_mob_id);
                                    if (hud_state.radio_log.size() >= 30)
                                        hud_state.radio_log.pop_front();
                                    hud_state.radio_log.push_back("[Drag] Body dropped.");
                                }});
                        }

                        // CPR — only within 2 m and while still revivable
                        if (can_revive && rmb_mob_dist <= 2.f) {
                            entries.push_back({"Perform CPR", true, false,
                                [&g_entities, rmb_mob_id, &hud_state]() {
                                    auto* hp_c = g_entities.get_component<HealthComponent>(rmb_mob_id);
                                    if (!hp_c || !hp_c->dead) return;
                                    bool success = (std::rand() % 10) < 3;  // 30% chance
                                    if (success) {
                                        hp_c->oxy   = std::max(0.f, hp_c->oxy   - 60.f);
                                        hp_c->brute = std::max(0.f, hp_c->brute -  5.f);
                                        if (hp_c->current() > 0.f) {
                                            hp_c->dead = false;
                                            g_entities.remove_component<CorpseComponent>(rmb_mob_id);
                                            auto* cc_r = g_entities.get_component<CharacterControllerComponent>(rmb_mob_id);
                                            if (cc_r) cc_r->mob_state = MobState::Softcrit;
                                            signals().send_signal(rmb_mob_id, COMSIG_MOB_LIVING_REVIVE, SigDeath{});
                                            if (hud_state.radio_log.size() >= 30)
                                                hud_state.radio_log.pop_front();
                                            hud_state.radio_log.push_back("[CPR] They're breathing again!");
                                            return;
                                        }
                                    }
                                    if (hud_state.radio_log.size() >= 30)
                                        hud_state.radio_log.pop_front();
                                    hud_state.radio_log.push_back("[CPR] No response.");
                                }});
                        }
                    }

                    if (!entries.empty()) {
                        glm::vec2 mob_menu_pos = {
                            renderer.width()  * 0.5f - 86.f,
                            renderer.height() * 0.5f - 20.f
                        };
                        ctx_menu.open(mob_menu_pos, std::move(entries));
                        fps_ctx_rclick      = true;
                        fps_ctx_just_opened = true;
                    }

                } else if (ctx_ent != NULL_ENTITY) {
                    auto* wic = g_entities.get_component<WorldItemComponent>(ctx_ent);
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
                            for (const auto& verb : wic->item.def->verbs) {
                                EntityID veid     = ctx_ent;
                                std::string vhnd  = verb.handler;
                                entries.push_back({verb.name, true, false,
                                    [veid, vhnd, &g_entities, &world_items,
                                     &cam_pos, &cam_yaw, &cam_pitch,
                                     &client, &hud_state]() {
                                        auto* wic2 = g_entities
                                            .get_component<WorldItemComponent>(veid);
                                        VerbContext vctx;
                                        vctx.actor      = client.local_player();
                                        vctx.actor_pos  = cam_pos;
                                        vctx.target_ent = veid;
                                        if (wic2) {
                                            vctx.item_def   = wic2->item.def;
                                            vctx.item_stack = &wic2->item;
                                        }
                                        vctx.hud_log = &hud_state.radio_log;
                                        verb_dispatch().invoke(vhnd, vctx);
                                    }});
                            }
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
                } else if (fhit2.valid) {
                    // ── Tool-on-voxel interactions ────────────────────────────
                    auto*              hand_slot  = player_inv.active_hand();
                    bool               hand_empty = !hand_slot || !hand_slot->item;
                    const ItemDef*     held_def   = (!hand_empty && hand_slot->item->def)
                                                        ? hand_slot->item->def : nullptr;
                    Voxel              hv  = g_world.get_voxel(fhit2.voxel);
                    const VoxelTypeDef* hvd = voxel_reg.get(hv.type_id);

                    // Helper: does the held item have a given tag?
                    auto held_has_tag = [&](const char* tag) -> bool {
                        if (!held_def) return false;
                        for (const auto& t : held_def->tags)
                            if (t == tag) return true;
                        return false;
                    };

                    // Helper: log a message to the HUD radio log
                    auto hud_log = [&](const char* msg) {
                        if (hud_state.radio_log.size() >= 30)
                            hud_state.radio_log.pop_front();
                        hud_state.radio_log.push_back(msg);
                    };

                    // Helper: apply a voxel change and update all systems
                    auto apply_voxel = [&](glm::ivec3 pos, Voxel v) {
                        // Read old voxel before overwriting so we can skip
                        // unnecessary lighting BFS (the hot case: two non-emissive,
                        // same-opacity tiles swapped — e.g. floor_tile → plating).
                        Voxel old_v = g_world.get_voxel(pos);
                        g_world.set_voxel(pos, v);
                        if (g_atmos) g_atmos->on_voxel_changed(pos);

                        // Only run the BFS when something light-relevant changed:
                        //   • old or new voxel emits light, OR
                        //   • opacity changed (affects how light propagates through here)
                        bool was_emitter     = (old_v.flags & VFLAG_LIGHT_SRC) != 0;
                        bool is_emitter      = (v.flags     & VFLAG_LIGHT_SRC) != 0;
                        bool opacity_changed = (old_v.flags & VFLAG_OPAQUE)
                                            != (v.flags     & VFLAG_OPAQUE);
                        if (was_emitter || is_emitter || opacity_changed)
                            lighting.update({pos});

                        mesher.enqueue_batch(g_world.dirty_chunks(), g_world);
                    };

                    bool is_plating = hvd && (hvd->id == "floor_plating"
                                          || hvd->id == "plating_monk");

                    // ── 1. Crowbar: pry any voxel that declares a pry_item ───
                    if (held_def && held_def->id == "crowbar"
                            && hvd && !hvd->pry_item.empty()) {
                        glm::ivec3 pry_pos   = fhit2.voxel;
                        std::string pry_item = hvd->pry_item;
                        uint16_t plating_id = voxel_reg.id_of("floor_plating");
                        Voxel pv;
                        pv.type_id = plating_id;
                        pv.flags   = VFLAG_SOLID | VFLAG_OPAQUE;
                        apply_voxel(pry_pos, pv);
                        const ItemDef* tile_def = item_reg.get(pry_item);
                        if (tile_def) {
                            ItemStack st;
                            st.def   = tile_def;
                            st.count = 1;
                            world_items.spawn_scattered(
                                pry_pos, FaceDir::PosY, std::move(st));
                        }
                        hud_log("[Crowbar] You pry up the floor tile.");
                    }
                    // ── 2. Tile item on plating → place floor voxel ──────────
                    else if (is_plating && held_def
                             && !held_def->places_voxel.empty()) {
                        uint16_t new_vox_id = voxel_reg.id_of(held_def->places_voxel);
                        if (new_vox_id != 0) {
                            const VoxelTypeDef* nvd = voxel_reg.get(new_vox_id);
                            Voxel fv;
                            fv.type_id = new_vox_id;
                            fv.flags   = nvd ? nvd->default_flags
                                             : static_cast<uint16_t>(VFLAG_SOLID | VFLAG_OPAQUE);
                            apply_voxel(fhit2.voxel, fv);
                            hand_slot->item->count--;
                            if (hand_slot->item->count <= 0)
                                hand_slot->item = std::nullopt;
                            hud_log("[Tile] You lay down the floor tile.");
                        }
                    }
                    // ── 3. Cable coil on plating → lay wire ──────────────────
                    else if (is_plating && held_has_tag("cable")) {
                        uint16_t wire_id = voxel_reg.id_of("wire");
                        if (wire_id != 0) {
                            const VoxelTypeDef* wvd = voxel_reg.get(wire_id);
                            Voxel wv;
                            wv.type_id = wire_id;
                            wv.flags   = wvd ? wvd->default_flags
                                             : static_cast<uint16_t>(VFLAG_BITMASK_FLAT);
                            apply_voxel(fhit2.voxel, wv);
                            hand_slot->item->count--;
                            if (hand_slot->item->count <= 0)
                                hand_slot->item = std::nullopt;
                            hud_log("[Wire] You lay down the cable.");
                        }
                    }
                    // ── 4. Wirecutters on wire → remove, drop coil ───────────
                    else if (hvd && hvd->id == "wire"
                             && held_def && held_def->id == "wirecutters") {
                        uint16_t plating_id = voxel_reg.id_of("floor_plating");
                        Voxel pv;
                        pv.type_id = plating_id;
                        pv.flags   = VFLAG_SOLID | VFLAG_OPAQUE;
                        apply_voxel(fhit2.voxel, pv);
                        const ItemDef* coil_def = item_reg.get("cable_coil");
                        if (coil_def) {
                            ItemStack cs;
                            cs.def   = coil_def;
                            cs.count = 1;
                            world_items.spawn_scattered(
                                fhit2.voxel, FaceDir::PosY, std::move(cs));
                        }
                        hud_log("[Wirecutters] You cut the cable.");
                    }
                    // ── 5. Pipe section on plating → lay pipe ────────────────
                    else if (is_plating && held_has_tag("pipe")) {
                        uint16_t pipe_id = voxel_reg.id_of("pipe");
                        if (pipe_id != 0) {
                            const VoxelTypeDef* pvd = voxel_reg.get(pipe_id);
                            Voxel pipev;
                            pipev.type_id = pipe_id;
                            pipev.flags   = pvd ? pvd->default_flags
                                               : static_cast<uint16_t>(VFLAG_BITMASK_FLAT);
                            apply_voxel(fhit2.voxel, pipev);
                            hand_slot->item->count--;
                            if (hand_slot->item->count <= 0)
                                hand_slot->item = std::nullopt;
                            hud_log("[Pipe] You lay down the pipe section.");
                        }
                    }
                    // ── 6. Wrench on pipe → remove, drop section ─────────────
                    else if (hvd && hvd->id == "pipe"
                             && held_def && held_def->id == "wrench") {
                        uint16_t plating_id = voxel_reg.id_of("floor_plating");
                        Voxel pv;
                        pv.type_id = plating_id;
                        pv.flags   = VFLAG_SOLID | VFLAG_OPAQUE;
                        apply_voxel(fhit2.voxel, pv);
                        const ItemDef* pipe_def = item_reg.get("pipe_section");
                        if (pipe_def) {
                            ItemStack ps;
                            ps.def   = pipe_def;
                            ps.count = 1;
                            world_items.spawn_scattered(
                                fhit2.voxel, FaceDir::PosY, std::move(ps));
                        }
                        hud_log("[Wrench] You remove the pipe section.");
                    }
                } else {
                    // ── Held item verb menu (nothing in world was targeted) ───
                    auto* hand_slot_h = player_inv.active_hand();
                    if (hand_slot_h && hand_slot_h->item && hand_slot_h->item->def
                        && !hand_slot_h->item->def->verbs.empty()
                        && !ctx_menu.is_open()) {
                        std::vector<ContextEntry> entries;
                        for (const auto& verb : hand_slot_h->item->def->verbs) {
                            std::string vhnd = verb.handler;
                            entries.push_back({verb.name, true, false,
                                [vhnd, &player_inv, &cam_pos, &hud_state, &client]() {
                                    auto* hs = player_inv.active_hand();
                                    VerbContext vctx;
                                    vctx.actor      = client.local_player();
                                    vctx.actor_pos  = cam_pos;
                                    vctx.target_ent = NULL_ENTITY;
                                    if (hs && hs->item) {
                                        vctx.item_def   = hs->item->def;
                                        vctx.item_stack = &(*hs->item);
                                    }
                                    vctx.hud_log = &hud_state.radio_log;
                                    verb_dispatch().invoke(vhnd, vctx);
                                }});
                        }
                        glm::vec2 menu_pos = {
                            renderer.width()  * 0.5f - 86.f,
                            renderer.height() * 0.5f - 20.f
                        };
                        ctx_menu.open(menu_pos, std::move(entries));
                        fps_ctx_rclick      = true;
                        fps_ctx_just_opened = true;
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
                    auto* wic = g_entities.get_component<WorldItemComponent>(hovered_item_entity);
                    if (wic && wic->item.def && !inv_panel.is_dragging()) {
                        inv_panel.begin_world_drag(wic->item, alt_mode.cursor_pos());
                        alt_drag_entity = hovered_item_entity;
                    }
                } else {
                    // Use held item on hovered world item
                    auto* wic = g_entities.get_component<WorldItemComponent>(hovered_item_entity);
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
                    RayHit bhit = g_world.raycast(cam_pos, rdir, 8.f);
                    if (bhit.valid) {
                        // Snapshot light level before clearing the voxel.
                        // set_voxel(Voxel{}) zeroes light_level, which makes
                        // bfs_remove_colored skip removal (lvl==0 early-exit).
                        // Restoring it here lets the BFS correctly flood-remove.
                        uint8_t old_light = g_world.get_voxel(bhit.voxel).light_level;
                        g_world.set_voxel(bhit.voxel, Voxel{});
                        if (old_light > 0)
                            g_world.set_light_level(bhit.voxel, old_light);
                        if (g_atmos) g_atmos->on_voxel_changed(bhit.voxel);
                        // Propagate lighting change from removed voxel, then
                        // enqueue all dirty chunks in one pass (avoids double
                        // snapshot before lighting has touched neighbour chunks).
                        lighting.update({bhit.voxel});
                        mesher.enqueue_batch(g_world.dirty_chunks(), g_world);
                        SDL_Log("Build: destroyed voxel at (%d,%d,%d)",
                                bhit.voxel.x, bhit.voxel.y, bhit.voxel.z);
                    }
                }
                // RMB: place voxel on adjacent face
                if (input.is_pressed(Action::SecondaryInteract)) {
                    RayHit bhit = g_world.raycast(cam_pos, rdir, 8.f);
                    if (bhit.valid) {
                        glm::ivec3 place_pos = bhit.voxel + face_normal(bhit.face);
                        if (g_world.get_voxel(place_pos).type_id == 0) {
                            g_world.set_voxel(place_pos, build_voxel);
                            if (g_atmos) g_atmos->on_voxel_changed(place_pos);
                            // Propagate lighting change from placed voxel, then
                            // enqueue all dirty chunks in one pass.
                            lighting.update({place_pos});
                            mesher.enqueue_batch(g_world.dirty_chunks(), g_world);
                            SDL_Log("Build: placed voxel at (%d,%d,%d)",
                                    place_pos.x, place_pos.y, place_pos.z);
                        }
                    }
                }
            }

            // Escape: open pause menu if nothing else is consuming it.
            // (Context menu, admin, creative, map editor all handle their own Escape above.)
            if (input.is_pressed(Action::Escape)) {
                bool any_esc_handled = creative_menu.is_open() || map_editor.is_open()
                                     || admin_menu.is_open()  || ctx_menu.is_open();
                if (!any_esc_handled && !pause_menu.is_open()) {
                    pause_menu.open();
                    input.capture_cursor(renderer.window(), false);
                    // Consume the press so the render callback's pause menu draw
                    // does NOT see it and immediately close the menu on the same frame.
                    input.consume_press(Action::Escape);
                }
            }
        },

        // Variable-rate render
        [&](double alpha) {
            client.interpolate(alpha);

            // Re-derive cam_pos from the player's interpolated position so the
            // camera moves smoothly between ticks rather than stepping at 60 Hz.
            {
                EntityID plr_r = client.local_player();
                if (!third_person && plr_r != NULL_ENTITY) {
                    auto* tr_r = g_entities.get_component<TransformComponent>(plr_r);
                    if (tr_r) {
                        float body_yaw_r2  = glm::radians(player_body_yaw);
                        glm::vec3 body_fwd2 = { std::sin(body_yaw_r2), 0.f, -std::cos(body_yaw_r2) };
                        float eye_h2   = glm::mix(0.50f, 0.14f, prone_t);
                        float fwd_off2 = glm::mix(0.05f, 0.50f, prone_t);
                        cam_pos = tr_r->pos + glm::vec3(0.f, eye_h2, 0.f) + body_fwd2 * fwd_off2;
                    }
                }
            }

            // ── Camera basis + view-projection (mirrors draw_world) ──────────
            float yaw_r   = glm::radians(cam_yaw);
            float pitch_r = glm::radians(cam_pitch);
            float roll_r  = glm::radians(cam_roll);
            glm::vec3 ray_dir = {
                std::cos(pitch_r) * std::sin(yaw_r),
                std::sin(pitch_r),
               -std::cos(pitch_r) * std::cos(yaw_r)
            };
            // Roll-aware camera up — mirrors Renderer::draw_world.
            glm::vec3 world_up_r = {0.f, 1.f, 0.f};
            glm::vec3 right_raw_r;
            if (std::abs(glm::dot(ray_dir, world_up_r)) > 0.99f)
                right_raw_r = {std::sin(yaw_r + glm::half_pi<float>()), 0.f,
                               -std::cos(yaw_r + glm::half_pi<float>())};
            else
                right_raw_r = glm::normalize(glm::cross(ray_dir, world_up_r));
            glm::vec3 up_no_roll_r = glm::normalize(glm::cross(right_raw_r, ray_dir));
            glm::vec3 cam_up_r = up_no_roll_r * std::cos(roll_r)
                               - right_raw_r  * std::sin(roll_r);
            glm::mat4 vp_mat = glm::perspective(
                glm::radians(game_settings.fov),
                renderer.height() > 0 ? float(renderer.width()) / renderer.height() : 1.f,
                0.1f, 400.f)
                * glm::lookAt(cam_pos, cam_pos + ray_dir, cam_up_r);

            // ── Voxel face ray cast ──────────────────────────────────────────
            RayHit hit = g_world.raycast(cam_pos, ray_dir, 4.f);
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
                                auto* tr = g_entities.get_component<TransformComponent>(eid);
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
            renderer.queue_world_items(g_entities, hovered_item_entity,
                                       cam_pos, cam_yaw, cam_pitch);
            // Append vehicle-local items at their world-space positions
            if (test_vehicle)
                renderer.queue_world_items(test_vehicle->entities(), NULL_ENTITY,
                                           cam_pos, cam_yaw, cam_pitch,
                                           test_vehicle->world_pos_f(), /*clear_first=*/false);

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
                // category is the path segment between "inhands/" and "_lefthand"/"_righthand"
                // e.g. "equipment/tools" → legacysets/extracted/mob/inhands/equipment/tools_lefthand
                struct IEntry { const char* id; const char* category; const char* name; };
                static const IEntry k_inhand_map[] = {
                    { "wrench",       "equipment/tools",   "wrench"      },
                    { "screwdriver",  "equipment/tools",   "screwdriver" },
                    { "crowbar",      "equipment/tools",   "crowbar"     },
                    { "wirecutters",  "equipment/tools",   "cutters"     },
                    { "welder",       "equipment/tools",   "welder"      },
                    { "multitool",    "equipment/tools",   "multitool"   },
                    { "drill",        "equipment/tools",   "drill"       },
                    { "rcd",          "equipment/tools",   "rcd"         },
                    { "pickaxe",      "equipment/mining",  "pickaxe"     },
                    { "shovel",       "equipment/mining",  "shovel"      },
                    { "flashlight",   "items/devices",     "flashlight"  },
                    { "radio",        "items/devices",     "radio"       },
                    { "baseball_bat", "weapons/melee",     "baseball_bat"},
                    { "katana",       "weapons/swords",    "katana"      },
                    { "bow",          "weapons/bows",      "bow"         },
                    { "beaker",       "items",             "beaker"      },
                    { "medipen",      "equipment/medical", "medipen"     },
                    { "syringe",      "equipment/medical", "syringe_0"   },
                    { "scalpel",      "equipment/medical", "scalpel"     },
                };

                static const char* k_vis_eq_slots[] = {
                    "suit","head","glasses","mask","back","belt","gloves","shoes","ears"
                };

                // Helper: rebuild Clothing/Inhand layers in `app` when the
                // inventory fingerprint changes.  Bodypart layers are preserved.
                auto rebuild_if_changed = [&](HumanAppearance& app,
                                              const Inventory&  inv,
                                              std::string&      last_fp) {
                    // Pre-reserve to avoid repeated reallocations on every
                    // render frame.  Use append() throughout to prevent the
                    // temporary std::string objects created by operator+.
                    std::string fp;
                    fp.reserve(256);
                    for (const char* sl : k_vis_eq_slots) {
                        const auto* slt = inv.find_slot(sl);
                        if (slt && slt->item && slt->item->def) {
                            fp.append(sl);
                            fp += '=';
                            fp.append(slt->item->def->id);
                            fp += ';';
                        }
                    }
                    for (const char* sl : { "l_hand", "r_hand" }) {
                        const auto* slt = inv.find_slot(sl);
                        if (slt && slt->item && slt->item->def) {
                            fp.append(sl);
                            fp += '=';
                            fp.append(slt->item->def->id);
                            fp += ';';
                        }
                    }
                    // For the local player, include character profile in fingerprint
                    // so hair/skin changes trigger a rebuild.
                    if (&inv == &player_inv) {
                        fp.append("|hair=");  fp.append(player_profile.hair_file);
                        fp.append("|hcol=");  fp.append(std::to_string(player_profile.hair_col_idx));
                        fp.append("|facial=");fp.append(player_profile.facial_file);
                        fp.append("|fcol=");  fp.append(std::to_string(player_profile.facial_col_idx));
                        fp.append("|skin=");  fp.append(std::to_string(player_profile.skin_idx));
                        fp.append("|eye=");   fp.append(std::to_string(player_profile.eye_col_idx));
                        fp.append("|gend=");  fp += (player_profile.is_male ? 'm' : 'f');
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

                    // Hair and facial hair from the character profile (local player only).
                    // These are re-added every rebuild so they survive clothing changes.
                    if (&inv == &player_inv) {
                        // Eye overlays (left + right, tinted with eye colour)
                        for (const char* eyefix : { "eyes_l", "eyes_r" }) {
                            HumanOverlay ov;
                            ov.kind       = HumanOverlayKind::Clothing;
                            ov.sprite_dir = "human/human_eyes";
                            ov.prefix     = eyefix;
                            ov.tint       = { player_profile.eye_color.r,
                                              player_profile.eye_color.g,
                                              player_profile.eye_color.b, 255 };
                            new_layers.push_back(ov);
                        }
                        if (!player_profile.hair_file.empty()) {
                            HumanOverlay ov;
                            ov.kind       = HumanOverlayKind::Clothing;
                            ov.sprite_dir = "human/human_face";
                            ov.prefix     = player_profile.hair_file;
                            ov.tint       = { player_profile.hair_color.r,
                                              player_profile.hair_color.g,
                                              player_profile.hair_color.b, 255 };
                            new_layers.push_back(ov);
                        }
                        if (!player_profile.facial_file.empty()) {
                            HumanOverlay ov;
                            ov.kind       = HumanOverlayKind::Clothing;
                            ov.sprite_dir = "human/human_face";
                            ov.prefix     = player_profile.facial_file;
                            ov.tint       = { player_profile.facial_color.r,
                                              player_profile.facial_color.g,
                                              player_profile.facial_color.b, 255 };
                            new_layers.push_back(ov);
                        }
                    }
                    app.layers = std::move(new_layers);
                    app.dirty  = true;
                };

                // ── Local player ──────────────────────────────────────────────
                EntityID player = client.local_player();
                if (player != NULL_ENTITY) {
                    auto* app = g_entities.get_component<HumanAppearance>(player);
                    if (app) {
                        static std::string s_last_app_fp;
                        std::string fp_before = s_last_app_fp;
                        rebuild_if_changed(*app, player_inv, s_last_app_fp);
                        // Send equipment update to server whenever inventory changed
                        if (s_last_app_fp != fp_before && client.connected()) {
                            std::vector<std::pair<std::string,std::string>> slots;
                            for (const char* sl : k_vis_eq_slots) {
                                const auto* slt = player_inv.find_slot(sl);
                                if (slt && slt->item && slt->item->def)
                                    slots.emplace_back(sl, slt->item->def->id);
                            }
                            for (const char* sl : { "l_hand", "r_hand" }) {
                                const auto* slt = player_inv.find_slot(sl);
                                if (slt && slt->item && slt->item->def)
                                    slots.emplace_back(sl, slt->item->def->id);
                            }
                            client.set_equipped_slots(slots);
                            client.send_appearance_update();
                        }
                    }
                }

                // ── All other mobs with InventoryComponent ────────────────────
                g_entities.each<InventoryComponent>(
                    [&](EntityID eid, InventoryComponent& ic) {
                        if (eid == client.local_player()) return; // handled above
                        auto* app = g_entities.get_component<HumanAppearance>(eid);
                        if (!app) return;
                        rebuild_if_changed(*app, ic.inv, ic.overlay_fp);
                    });
            }

            for (const auto& obj : model_objs.objects())
                renderer.queue_model(obj.name.c_str(), obj.world_pos, obj.yaw, obj.scale);
            renderer.queue_earth_background(cam_pos, cam_yaw, cam_pitch);

            // ── Apply view bob & sway offsets (render only, not physics) ────
            // Computed here, before begin_frame, so queue_mobs uses the same
            // camera the GPU will actually render with this frame.
            glm::vec3 render_cam_pos   = cam_pos;
            float     render_cam_yaw   = cam_yaw;
            float     render_cam_pitch = cam_pitch;
            if (!third_person) {
                constexpr float BOB_Y_AMP     = 0.022f;
                constexpr float BOB_PITCH_AMP = 0.45f;
                constexpr float BOB_YAW_AMP   = 0.28f;
                float b = bob_blend;
                render_cam_pos.y    += std::sin(bob_time)        * BOB_Y_AMP     * b;
                render_cam_pitch    += std::sin(bob_time)        * BOB_PITCH_AMP * b;
                render_cam_yaw      += std::sin(bob_time * 0.5f) * BOB_YAW_AMP  * b
                                     + bob_sway;
            }

            // Queue mobs now so begin_frame can upload them in the same frame.
            renderer.queue_mobs(g_entities, render_cam_pos, render_cam_yaw,
                                 third_person ? NULL_ENTITY : client.local_player());
            if (test_vehicle)
                renderer.queue_mobs(test_vehicle->entities(), render_cam_pos, render_cam_yaw,
                                    NULL_ENTITY, test_vehicle->world_pos_f(),
                                    /*clear_first=*/false);

            renderer.begin_frame(alpha);

            // Push lighting state to renderer
            renderer.set_fov(game_settings.fov);
            renderer.set_fullbright(fullbright_enabled);
            renderer.set_ao_mix(ao_enabled ? 1.0f : 0.0f);
            renderer.set_ambient(0.05f);  // dark station; light tubes provide illumination
            renderer.set_psx_wobble(game_settings.psx_wobble);
            renderer.set_psx_snap_res(game_settings.psx_snap_res);
            renderer.set_affine_mix(game_settings.affine_enabled ? game_settings.affine_mix : 0.0f);
            // Y-shear is pitch-driven; set value once render_cam_pitch is known (below)

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
                        mesher.enqueue_batch(g_world.dirty_chunks(), g_world);
                    }
                    // Mirror into each vehicle's lighting (vehicle-local position).
                    // If the player is outside the vehicle's voxel bounds, clamp
                    // to the nearest surface point of the nearest occupied chunk so
                    // the BFS has a valid starting position and can flood inward.
                    if (test_vehicle) {
                        World& vw3 = test_vehicle->world();
                        glm::ivec3 vlocal = glm::ivec3(glm::floor(
                            cam_pos - test_vehicle->world_pos_f()));

                        if (!vw3.get_chunk(World::to_chunk_pos(vlocal))) {
                            glm::ivec3 clamped = vlocal;
                            float best = 1e30f;
                            vw3.for_each_chunk([&](const Chunk& ch) {
                                glm::ivec3 cmin = ch.chunk_pos() * CHUNK_SIZE;
                                glm::ivec3 cmax = cmin + glm::ivec3(CHUNK_SIZE - 1);
                                glm::ivec3 nearest = {
                                    std::max(cmin.x, std::min(cmax.x, vlocal.x)),
                                    std::max(cmin.y, std::min(cmax.y, vlocal.y)),
                                    std::max(cmin.z, std::min(cmax.z, vlocal.z)),
                                };
                                float d = glm::length(glm::vec3(nearest - vlocal));
                                if (d < best) { best = d; clamped = nearest; }
                            });
                            vlocal = clamped;
                        }

                        static glm::ivec3 s_last_veh_pos = {999999, 999999, 999999};
                        if (!s_last_held || vlocal != s_last_veh_pos) {
                            test_vehicle->lighting().add_dynamic_light(
                                vlocal, 12, src, {255, 200, 140});
                            s_last_veh_pos = vlocal;
                            vehicle_mesher.enqueue_batch(vw3.dirty_chunks(), vw3, true);
                        }
                    }
                } else if (s_last_held) {
                    lighting.remove_dynamic_light(src);
                    // Re-mesh to restore steady-state static lighting.
                    mesher.enqueue_batch(g_world.dirty_chunks(), g_world);
                    // Remove from vehicle lighting too
                    if (test_vehicle) {
                        test_vehicle->lighting().remove_dynamic_light(src);
                        World& vw3 = test_vehicle->world();
                        vehicle_mesher.enqueue_batch(vw3.dirty_chunks(), vw3, true);
                    }
                }
                s_last_held = holds_flashlight;
            }

            // ── Apply view bob & sway offsets (render only, not physics) ────
            // NOTE: render_cam_pos/yaw/pitch were already computed before begin_frame.
            // This block is preserved for status effects and yshear that follow.

            // ── Status-effect camera distortions (render only) ─────────────────
            // Read active status effects on the local player and apply visual-only
            // perturbations to the render camera.
            bool se_blind  = false;
            bool se_jitter = false;
            bool se_dizzy  = false;
            float dizzy_str = 0.f;
            {
                EntityID plr_se = client.local_player();
                if (plr_se != NULL_ENTITY) {
                    auto* se_pl = g_entities.get_component<StatusEffectsComponent>(plr_se);
                    if (se_pl) {
                        se_blind  = se_pl->is_blinded();
                        se_jitter = se_pl->is_jittering();
                        se_dizzy  = se_pl->is_dizzy();
                        if (se_dizzy) {
                            const auto* deff = se_pl->get(StatusEffectType::Dizzy);
                            dizzy_str = deff ? deff->strength : 1.f;
                        }
                    }
                }
            }
            // Jitter camera shake is intentionally disabled.
            // Dizzy: slow oscillating screen roll (up to ±12 degrees)
            float render_cam_roll = cam_roll;
            if (se_dizzy && !third_person) {
                constexpr float DIZZY_ROLL_MAX = 12.f;
                render_cam_roll += std::sin(static_cast<float>(sim_time) * 1.1f)
                                 * DIZZY_ROLL_MAX * dizzy_str;
            }

            // Y-shear: Build-engine style linear horizon shift — pitch/89 * 0.6
            // At max pitch the horizon sits ~60% of the way to the screen edge,
            // matching the "horiz" pan look of Duke Nukem 3D / Build engine games.
            {
                float yshear = 0.f;
                if (game_settings.psx_yshear)
                    yshear = (render_cam_pitch / 89.f) * 0.6f;
                renderer.set_psx_yshear(yshear);
            }

            // Queue mobs using the final render camera (after bob/sway) so the
            // local player body billboard uses the same yaw the camera renders with.
            renderer.draw_world(g_world, render_cam_pos, render_cam_yaw,
                render_cam_pitch, render_cam_roll);
            renderer.draw_world_items();
            renderer.draw_mobs();
            renderer.draw_models();
            if (test_vehicle)
                renderer.draw_vehicle(test_vehicle->id(), test_vehicle->world_pos_f());
            renderer.draw_viewmodel(0);
            // Pass the held item's type_id when the item pipeline is ready.
            // For now type_id=0 silently skips the draw; the call at least
            // exercises the code path so future shader work compiles cleanly.
            // When item meshes are wired in, replace 0 with:
            //   const auto* aslot = player_inv.active_hand();
            //   uint16_t held_id = (aslot && aslot->item && aslot->item->def)
            //       ? voxel_reg.id_of(aslot->item->def->id) : 0;
            //   renderer.draw_viewmodel(held_id);

            // UI pass
            ui_renderer.begin();

            // ── Status-effect fullscreen overlays ─────────────────────────────
            // Drawn first (bottom of UI stack) so all HUD elements render on top.
            if (se_blind) {
                // Full blindness: pitch-black screen
                ui_renderer.rect({0.f, 0.f},
                    {static_cast<float>(renderer.width()), static_cast<float>(renderer.height())},
                    {0.f, 0.f, 0.f, 0.96f});
            }

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
                bool hud_click = cursor_free && input.consume_press(Action::PrimaryInteract);
                {
                    std::string hud_clicked = hud.draw(hud_state, player_inv, hud_mouse, hud_click);
                    if (!hud_clicked.empty()) {
                        if (hud_clicked == "swap_hand") {
                            player_inv.cycle_active_hand();
                        } else {
                        const std::string& active = player_inv.active_hand_id();
                        auto* eq_slot   = player_inv.find_slot(hud_clicked);
                        auto* hand_slot = player_inv.find_slot(active);
                        bool did_swap = false;
                        // If an equipment slot has an item but the active hand holds
                        // something the slot won't accept, the normal swap fails
                        // silently and the player can't take the clothing off.
                        // Find a free hand or pocket to receive the equipped item instead.
                        if (eq_slot && eq_slot->item && hand_slot && hand_slot->item) {
                            const ItemDef* hand_def = hand_slot->item->def;
                            if (hand_def && !eq_slot->accepts(*hand_def)) {
                                static const char* k_unequip_targets[] =
                                    {"l_hand", "r_hand", "l_pocket", "r_pocket"};
                                for (const char* tgt : k_unequip_targets) {
                                    if (std::string(tgt) == hud_clicked) continue;
                                    auto* tgt_slot = player_inv.find_slot(tgt);
                                    if (tgt_slot && !tgt_slot->item) {
                                        player_inv.swap(hud_clicked, tgt);
                                        did_swap = true;
                                        break;
                                    }
                                }
                            }
                        }
                        if (!did_swap)
                            player_inv.swap(hud_clicked, active);
                        } // end non-swap_hand
                    }
                }
            }

            // ── Crosshair ─────────────────────────────────────────────────
            if (!creative_menu.is_open() && !map_editor.is_open()
                && game_settings.show_crosshair) {
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

                // Third-person indicator
                if (third_person) {
                    constexpr glm::vec4 k_3p_col = {0.70f, 0.88f, 1.00f, 0.85f};
                    ui_renderer.text({cx - 7.f, cy + HALF + 5.f}, "3P", k_3p_col, 11.f);
                }
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
                        auto* vc = g_entities.get_component<VelocityComponent>(plr);
                        if (vc) dbg.velocity = vc->linear;
                        auto* cc = g_entities.get_component<CharacterControllerComponent>(plr);
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
                    Voxel hv = g_world.get_voxel(hit.voxel);
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
                    dbg.zone_id = (g_atmos ? g_atmos->zone_at(feet_voxel) : AtmosZoneID{});
                    AtmosZone* zone_ptr = (g_atmos ? g_atmos->zone(dbg.zone_id) : nullptr);
                    if (zone_ptr) {
                        dbg.gas_mix            = zone_ptr->gas;
                        dbg.room_cell_count    = zone_ptr->cell_count;
                        dbg.room_adj_count     = static_cast<int>(zone_ptr->adjacent_zones.size());
                        dbg.atmos_status       = zone_ptr->status;
                        dbg.pressure_loss_rate = zone_ptr->pressure_loss_rate;
                    }
                    dbg.total_rooms = (g_atmos ? g_atmos->total_rooms() : 0);

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
                if (g_atmos) gas_overlay.draw(*g_atmos, gs, sim_time);
            }

            // ── Player stats overlay (F6) ─────────────────────────────────────
            if (player_stats_visible) {
                PlayerStatsOverlayState ps;
                EntityID plr = client.local_player();

                // Species / variant from MobComponent if present, else defaults
                if (plr != NULL_ENTITY) {
                    auto* mc = g_entities.get_component<MobComponent>(plr);
                    if (mc) { ps.species = mc->species; ps.variant = mc->variant; }
                }

                // Health
                if (plr != NULL_ENTITY) {
                    auto* hp = g_entities.get_component<HealthComponent>(plr);
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
                    auto* cc = g_entities.get_component<CharacterControllerComponent>(plr);
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

                // RMB on a HUD inventory slot → item verb context menu
                if (rmb_pressed) {
                    // Find which slot (if any) is under the alt-mode cursor.
                    // The HUD draws slots in a fixed layout; we poll the slot
                    // the cursor is over via the HUD's draw result on alt-cursor.
                    bool hud_rmb_handled = false;
                    // Item verb list: build from the hovered world-item entity
                    if (hovered_item_entity != NULL_ENTITY) {
                        auto* wic = g_entities.get_component<WorldItemComponent>(hovered_item_entity);
                        if (wic && wic->item.def && !wic->item.def->verbs.empty()) {
                            std::vector<ContextEntry> entries;
                            for (const auto& verb : wic->item.def->verbs) {
                                EntityID veid    = hovered_item_entity;
                                std::string vhnd = verb.handler;
                                entries.push_back({verb.name, true, false,
                                    [veid, vhnd, &g_entities, &world_items,
                                     &cam_pos, &cam_yaw, &cam_pitch,
                                     &client, &hud_state]() {
                                        auto* wic2 = g_entities
                                            .get_component<WorldItemComponent>(veid);
                                        VerbContext vctx;
                                        vctx.actor      = client.local_player();
                                        vctx.actor_pos  = cam_pos;
                                        vctx.target_ent = veid;
                                        if (wic2) {
                                            vctx.item_def   = wic2->item.def;
                                            vctx.item_stack = &wic2->item;
                                        }
                                        vctx.hud_log = &hud_state.radio_log;
                                        verb_dispatch().invoke(vhnd, vctx);
                                    }});
                            }
                            ctx_menu.open(alt_mode.cursor_pos(), std::move(entries));
                            fps_ctx_rclick     = false;
                            fps_ctx_just_opened = true;
                            hud_rmb_handled     = true;
                        }
                    }
                    (void)hud_rmb_handled;

                    // RMB on a world face in alt-mode → populate verb list from voxel type
                    if (!hud_rmb_handled && alt_mode.active() && hit.valid && !ctx_menu.is_open()) {
                        Voxel hv = g_world.get_voxel(hit.voxel);
                        const VoxelTypeDef* hvd = voxel_reg.get(hv.type_id);
                        std::vector<ContextEntry> voxel_entries;
                        if (hvd) {
                            if (!hvd->on_hit.empty())
                                voxel_entries.push_back({hvd->on_hit, true, false,
                                    [name = hvd->on_hit]() { SDL_Log("Voxel verb: %s", name.c_str()); }});
                            if (!hvd->on_walk.empty() && hvd->on_walk != hvd->on_hit)
                                voxel_entries.push_back({hvd->on_walk, true, false,
                                    [name = hvd->on_walk]() { SDL_Log("Voxel verb: %s", name.c_str()); }});
                            if (voxel_entries.empty())
                                voxel_entries.push_back({hvd->name.empty() ? hvd->id : hvd->name,
                                    false, false, {}});
                        }
                        if (!voxel_entries.empty()) {
                            ctx_menu.open(alt_mode.cursor_pos(), std::move(voxel_entries));
                            fps_ctx_rclick      = false;
                            fps_ctx_just_opened = true;
                        }
                    }
                }
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
                    ? input.consume_press(Action::Escape)
                    : (suppress ? false : input.consume_press(Action::SecondaryInteract));
                // Clamp scroll to ±1 step per frame so the menu doesn't fly past entries
                float raw_scroll = frame_scroll;
                float clamped_scroll = (raw_scroll > 0.f) ? 1.f : (raw_scroll < 0.f) ? -1.f : 0.f;
                ctx_menu.draw(
                    ctx_cursor,
                    (fps_ctx_rclick || suppress) ? false : input.consume_press(Action::PrimaryInteract),
                    clamped_scroll,
                    suppress ? false : input.consume_press(Action::PickUp),  // E = confirm
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
                    RayHit fhc = g_world.raycast(cam_pos, rdc, CTX_REACH2);
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
                        auto* cc = g_entities.get_component<CharacterControllerComponent>(plr);
                        if (cc) adm_state.noclip = cc->noclip;
                        auto* hp = g_entities.get_component<HealthComponent>(plr);
                        if (hp) adm_state.godmode = hp->godmode;
                    }
                }
                adm_state.build_mode        = build_mode;
                adm_state.gas_overlay       = gas_overlay_visible;
                adm_state.debug_overlay     = debug_overlay_visible;
                adm_state.player_stats      = player_stats_visible;
                adm_state.verbose_log       = renderer.verbose_logging();
                adm_state.fullbright        = fullbright_enabled;
                adm_state.ambient_occlusion = ao_enabled;
                adm_state.wireframe         = renderer.wireframe();
                adm_state.freeze_sim        = freeze_sim;
                adm_state.slow_motion       = slow_motion;
                adm_state.auto_heal         = auto_heal;
                adm_state.zerog_override    = zerog_override;
                adm_state.infinite_oxy      = infinite_oxy;

                bool esc_adm = input.is_pressed(Action::Escape);
                auto ar = admin_menu.draw(input.mouse_pos(),
                                          input.consume_press(Action::PrimaryInteract),
                                          esc_adm,
                                          adm_state);

                if (ar.toggle_noclip) {
                    // Toggle noclip on client entity for immediate UI feedback,
                    // then send to server so physics honours it.
                    EntityID plr = client.local_player();
                    if (plr != NULL_ENTITY) {
                        auto* cc = g_entities.get_component<CharacterControllerComponent>(plr);
                        if (cc) cc->noclip = !cc->noclip;
                    }
                    client.send_admin_cmd(AdminCmdType::ToggleNoclip);
                }
                if (ar.toggle_godmode) {
                    EntityID plr = client.local_player();
                    if (plr != NULL_ENTITY) {
                        auto* hp = g_entities.get_component<HealthComponent>(plr);
                        if (hp) hp->godmode = !hp->godmode;
                    }
                    client.send_admin_cmd(AdminCmdType::ToggleGodmode);
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
                if (ar.toggle_wireframe) {
                    renderer.set_wireframe(!renderer.wireframe());
                    SDL_Log("Wireframe: %s", renderer.wireframe() ? "ON" : "OFF");
                }
                // ── Simulation / dev-tool toggles ──────────────────────────
                // freeze_sim and slow_motion are client-side (affect client tick rate).
                if (ar.toggle_freeze_sim) {
                    freeze_sim = !freeze_sim;
                    SDL_Log("Freeze Simulation (client): %s", freeze_sim ? "ON" : "OFF");
                }
                if (ar.toggle_slow_motion) {
                    slow_motion = !slow_motion;
                    SDL_Log("Slow Motion (client): %s", slow_motion ? "ON" : "OFF");
                }
                // auto_heal, zerog_override, infinite_oxy are applied server-side.
                if (ar.toggle_auto_heal) {
                    auto_heal = !auto_heal;
                    client.send_admin_cmd(AdminCmdType::ToggleAutoHeal);
                    SDL_Log("Auto-Heal: %s", auto_heal ? "ON" : "OFF");
                }
                if (ar.toggle_zerog_override) {
                    zerog_override = !zerog_override;
                    client.send_admin_cmd(AdminCmdType::ToggleZeroGOverride);
                    SDL_Log("Zero-G Override: %s", zerog_override ? "ON" : "OFF");
                }
                if (ar.toggle_infinite_oxy) {
                    infinite_oxy = !infinite_oxy;
                    client.send_admin_cmd(AdminCmdType::ToggleInfiniteOxy);
                    SDL_Log("Infinite Oxygen: %s", infinite_oxy ? "ON" : "OFF");
                }
                // ── One-shot actions ────────────────────────────────────────
                if (ar.action_full_heal)
                    client.send_admin_cmd(AdminCmdType::ActionFullHeal);
                if (ar.action_kill_player)
                    client.send_admin_cmd(AdminCmdType::ActionKillPlayer);
                if (ar.action_teleport_origin)
                    client.send_admin_cmd(AdminCmdType::ActionTeleportOrigin);
                if (ar.action_force_atmos)
                    client.send_admin_cmd(AdminCmdType::ActionForceAtmos);
                if (ar.action_spawn_items)
                    client.send_admin_cmd(AdminCmdType::ActionSpawnItems);
                if (ar.close_requested && !alt_mode.active() &&
                    !creative_menu.is_open() && !map_editor.is_open())
                    input.capture_cursor(renderer.window(), true);
            }

            // ── Creative menu (F2) ────────────────────────────────────────────
            if (creative_menu.is_open()) {
                bool esc_pressed = input.is_pressed(Action::Escape);
                auto cr = creative_menu.draw(
                    input.mouse_pos(),
                    input.consume_press(Action::PrimaryInteract),
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
                        auto* tr_pl = g_entities.get_component<TransformComponent>(plr_reload);
                        if (tr_pl) {
                            int px = static_cast<int>(std::floor(tr_pl->pos.x));
                            int pz = static_cast<int>(std::floor(tr_pl->pos.z));
                            bool found = false;
                            for (int sy = 1; sy <= 128 && !found; ++sy) {
                                Voxel bot = g_world.get_voxel({px, sy,     pz});
                                Voxel top = g_world.get_voxel({px, sy + 1, pz});
                                if (!(bot.flags & VFLAG_SOLID) && !(top.flags & VFLAG_SOLID)) {
                                    tr_pl->pos = { tr_pl->pos.x, float(sy), tr_pl->pos.z };
                                    auto* vel_pl = g_entities.get_component<VelocityComponent>(plr_reload);
                                    if (vel_pl) vel_pl->linear = {};
                                    found = true;
                                }
                            }
                            if (!found) {
                                // Fallback: just elevate above any potential geometry
                                tr_pl->pos.y = 130.f;
                                auto* vel_pl = g_entities.get_component<VelocityComponent>(plr_reload);
                                if (vel_pl) vel_pl->linear = {};
                            }
                        }
                    }
                }
                if (me_result.world_modified) {
                    mesher.enqueue_batch(g_world.dirty_chunks(), g_world);
                }
                if (me_result.needs_atmos_rebuild)
                    if (g_atmos) g_atmos->rebuild_zones();
                if (me_result.request_close) {
                    map_editor.close();
                    if (!alt_mode.active())
                        input.capture_cursor(renderer.window(), true);
                }
            }

            // ── Pause menu overlay (drawn on top of all other UI) ─────────
            if (pause_menu.is_open()) {
                // Sync live flags → game_settings so the menu reflects reality
                game_settings.fullbright        = fullbright_enabled;
                game_settings.ambient_occlusion = ao_enabled;
                game_settings.wireframe         = renderer.wireframe();
                game_settings.psx_wobble        = renderer.psx_wobble();
                game_settings.psx_snap_res       = renderer.psx_snap_res();
                game_settings.show_debug_overlay = debug_overlay_visible;
                game_settings.show_gas_overlay   = gas_overlay_visible;
                game_settings.show_player_stats  = player_stats_visible;
                game_settings.third_person       = third_person;
                game_settings.freeze_sim         = freeze_sim;

                SDL_MouseButtonFlags mb_pm = SDL_GetMouseState(nullptr, nullptr);
                bool lmb_held_pm  = (mb_pm & SDL_BUTTON_LMASK) != 0;
                bool lmb_press_pm = input.consume_press(Action::PrimaryInteract);
                bool esc_pm       = input.is_pressed(Action::Escape);

                auto pm_result = pause_menu.draw(
                    input.mouse_pos(), lmb_press_pm, lmb_held_pm,
                    esc_pm, game_settings);

                // Sync game_settings → live flags after any in-menu changes
                fullbright_enabled    = game_settings.fullbright;
                ao_enabled            = game_settings.ambient_occlusion;
                renderer.set_wireframe(game_settings.wireframe);
                debug_overlay_visible = game_settings.show_debug_overlay;
                gas_overlay_visible   = game_settings.show_gas_overlay;
                player_stats_visible  = game_settings.show_player_stats;
                if (third_person != game_settings.third_person) {
                    third_person = game_settings.third_person;
                    if (!third_person) cam_yaw = player_body_yaw;
                }
                freeze_sim = game_settings.freeze_sim;

                if (pm_result.resume_clicked)
                    input.capture_cursor(renderer.window(), true);
                if (pm_result.exit_game_clicked)
                    loop.stop();
                if (pm_result.exit_to_main_clicked)
                    loop.stop();  // breaks the game loop so we return to main()
            }

            // ── FPS counter (always visible, top-right corner) ──────────────────
            {
                char fps_buf[24];
                SDL_snprintf(fps_buf, sizeof(fps_buf), "%.0f fps", loop.fps());
                const float fw      = static_cast<float>(ui_renderer.fb_width());
                const float text_w  = static_cast<float>(SDL_strlen(fps_buf)) * 7.7f;
                constexpr float PAD = 4.f;
                constexpr float H   = 16.f;
                ui_renderer.rect({fw - text_w - PAD * 3.f, PAD},
                                 {text_w + PAD * 2.f, H},
                                 {0.f, 0.f, 0.f, 0.55f}, 2.f);
                ui_renderer.text({fw - text_w - PAD * 2.f, PAD + 2.f},
                                 fps_buf,
                                 {0.90f, 1.00f, 0.55f, 0.90f}, 12.f);
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
    ui_renderer.shutdown();
    renderer.shutdown();
    SDL_Quit();
    return 0;
}
