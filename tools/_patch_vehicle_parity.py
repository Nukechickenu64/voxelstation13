#!/usr/bin/env python3
"""
Patch main.cpp to make vehicle worlds fully interactive:
  1. Vehicle lighting setup after hull build
  2. DoorGroup extended with context pointers
  3. flood_fill_door generalised to take World&
  4. Door animation loop uses per-group context pointers
  5. Interaction block does dual raycast (main + vehicle worlds)
  6. Door / item interactions routed to correct world
  7. Vehicle world items queued for rendering
"""
import sys, os, re

MAIN = os.path.join(os.path.dirname(__file__), '..', 'src', 'main.cpp')
MAIN = os.path.normpath(MAIN)

with open(MAIN, 'r', encoding='utf-8') as f:
    src = f.read()

original = src  # keep for diff reporting
edits = []

# ─── helper ──────────────────────────────────────────────────────────────────
def replace_once(text, old, new, tag):
    idx = text.find(old)
    if idx == -1:
        print(f'FAIL [{tag}]: pattern not found', file=sys.stderr)
        sys.exit(1)
    edits.append(tag)
    return text[:idx] + new + text[idx+len(old):]

# ═══════════════════════════════════════════════════════════════════════════════
# 1. After vehicle hull build: init vehicle lighting
# ═══════════════════════════════════════════════════════════════════════════════
OLD1 = '''\
            vehicle_mesher.start(1);
            vehicle_mesher.set_registry(&voxel_reg);
            vehicle_mesher.enqueue_batch(vw.dirty_chunks(), vw);
        }'''

NEW1 = '''\
            vehicle_mesher.start(1);
            vehicle_mesher.set_registry(&voxel_reg);
            // Set up vehicle lighting so interiors are illuminated
            test_vehicle->set_registry(&voxel_reg);
            test_vehicle->lighting().rebuild();
            vehicle_mesher.set_lighting(&test_vehicle->lighting());
            vehicle_mesher.enqueue_batch(vw.dirty_chunks(), vw);
        }'''

src = replace_once(src, OLD1, NEW1, 'vehicle_lighting_setup')

# ═══════════════════════════════════════════════════════════════════════════════
# 2. Extend DoorGroup with context pointers
# ═══════════════════════════════════════════════════════════════════════════════
OLD2 = '''\
    struct DoorGroup {
        std::vector<glm::ivec3> voxels;
        int   frame    = 0;
        float accum_ms = 0.f;
        bool  closing  = false;  // true = playing frames in reverse toward door state
    };'''

NEW2 = '''\
    struct DoorGroup {
        std::vector<glm::ivec3> voxels;
        int   frame    = 0;
        float accum_ms = 0.f;
        bool  closing  = false;  // true = playing frames in reverse toward door state
        // Context: if null, uses main server world.  Set for vehicle doors.
        World*           ctx_world  = nullptr;
        AtmosSimulator*  ctx_atmos  = nullptr;
        ChunkMesher*     ctx_mesher = nullptr;
    };'''

src = replace_once(src, OLD2, NEW2, 'doorgroup_ctx')

# ═══════════════════════════════════════════════════════════════════════════════
# 3. Generalise flood_fill_door to take World&
# ═══════════════════════════════════════════════════════════════════════════════
OLD3 = '''\
    auto flood_fill_door = [&](glm::ivec3 seed, uint16_t target_type) {
        std::vector<glm::ivec3>             group;
        std::unordered_set<glm::ivec3> visited;
        std::vector<glm::ivec3>             queue;
        auto try_push = [&](glm::ivec3 p) {
            if (p.z != seed.z) return;
            if (!visited.insert(p).second) return;
            if (server.world().get_voxel(p).type_id == target_type)
                queue.push_back(p);
        };'''

NEW3 = '''\
    auto flood_fill_door = [&](World& w, glm::ivec3 seed, uint16_t target_type) {
        std::vector<glm::ivec3>             group;
        std::unordered_set<glm::ivec3> visited;
        std::vector<glm::ivec3>             queue;
        auto try_push = [&](glm::ivec3 p) {
            if (p.z != seed.z) return;
            if (!visited.insert(p).second) return;
            if (w.get_voxel(p).type_id == target_type)
                queue.push_back(p);
        };'''

src = replace_once(src, OLD3, NEW3, 'flood_fill_door_param')

# ═══════════════════════════════════════════════════════════════════════════════
# 4. Door animation loop: resolve per-group context at start of each iteration
# ═══════════════════════════════════════════════════════════════════════════════
OLD4 = '''\
                    DoorGroup& grp = *it;
                    grp.accum_ms += dt_ms;'''

NEW4 = '''\
                    DoorGroup& grp = *it;
                    // Resolve per-group context (vehicle or main world)
                    World&          grp_world  = grp.ctx_world  ? *grp.ctx_world  : server.world();
                    AtmosSimulator& grp_atmos  = grp.ctx_atmos  ? *grp.ctx_atmos  : server.atmos();
                    ChunkMesher&    grp_mesher = grp.ctx_mesher ? *grp.ctx_mesher : mesher;
                    grp.accum_ms += dt_ms;'''

src = replace_once(src, OLD4, NEW4, 'dooranim_ctx_resolve')

# 4a. Replace server.world() / mesher / server.atmos() in the opening branch
OLD4a = '''\
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
                                mesher.enqueue_batch(server.world().dirty_chunks(), server.world());
                            }
                            // Door is now gas-passable — notify atmos so zones merge.
                            if (!grp.voxels.empty())
                                server.atmos().on_door_changed(grp.voxels.front());'''

NEW4a = '''\
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
                            if (!grp.voxels.empty())
                                grp_atmos.on_door_changed(grp.voxels.front());'''

src = replace_once(src, OLD4a, NEW4a, 'dooranim_open_ctx')

# 4b. Replace server.world() / mesher / server.atmos() in the closing branch
OLD4b = '''\
                        if (grp.frame < 0) {
                            // All reversed frames played → switch voxels to door (closed)
                            if (door_type_id != 0) {
                                Voxel closed_v;
                                closed_v.type_id = door_type_id;
                                closed_v.flags   = VFLAG_SOLID | VFLAG_VERT_PLANE_Z;
                                for (auto& p : grp.voxels)
                                    server.world().set_voxel(p, closed_v);
                                mesher.enqueue_batch(server.world().dirty_chunks(), server.world());
                            }
                            // Door is now fully sealed — notify atmos to finalise zone split.
                            if (!grp.voxels.empty())
                                server.atmos().on_door_changed(grp.voxels.front());'''

NEW4b = '''\
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
                            if (!grp.voxels.empty())
                                grp_atmos.on_door_changed(grp.voxels.front());'''

src = replace_once(src, OLD4b, NEW4b, 'dooranim_close_ctx')

# ═══════════════════════════════════════════════════════════════════════════════
# 5 + 6. Interaction block: dual raycast + active world context
# ═══════════════════════════════════════════════════════════════════════════════
OLD5 = '''\
                    RayHit fhit = server.world().raycast(cam_pos, rdir, ITEM_REACH);
                    float item_dist = 0.f;
                    EntityID item_ent = world_items.ray_cast_items(
                        cam_pos, rdir, ITEM_REACH,
                        fhit.valid ? fhit.distance : ITEM_REACH, item_dist);'''

NEW5 = '''\
                    RayHit fhit = server.world().raycast(cam_pos, rdir, ITEM_REACH);
                    float item_dist = 0.f;
                    EntityID item_ent = world_items.ray_cast_items(
                        cam_pos, rdir, ITEM_REACH,
                        fhit.valid ? fhit.distance : ITEM_REACH, item_dist);

                    // ── Vehicle world dual-raycast ────────────────────────────
                    // Determine which world (main or vehicle) the player is
                    // targeting; all subsequent interaction code uses active_*.
                    World*           active_world  = &server.world();
                    AtmosSimulator*  active_atmos  = &server.atmos();
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
                    }'''

src = replace_once(src, OLD5, NEW5, 'dual_raycast')

# ─── 6a. item pickup: use active_wis ───────────────────────────────────────
OLD6a = '''\
                    } else if (item_ent != NULL_ENTITY) {
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
                            auto* wic = server.entities().get_component<WorldItemComponent>(item_ent);'''

NEW6a = '''\
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
                            auto* wic = active_world == &server.world()
                                ? server.entities().get_component<WorldItemComponent>(item_ent)
                                : (test_vehicle ? test_vehicle->entities().get_component<WorldItemComponent>(item_ent) : nullptr);'''

src = replace_once(src, OLD6a, NEW6a, 'item_pickup_ctx')

# ─── 6b. fhit block: use active_world ──────────────────────────────────────
OLD6b = '''\
                    } else if (fhit.valid) {
                        Voxel hit_v = server.world().get_voxel(fhit.voxel);'''

NEW6b = '''\
                    } else if (fhit.valid) {
                        Voxel hit_v = active_world->get_voxel(fhit.voxel);'''

src = replace_once(src, OLD6b, NEW6b, 'fhit_active_world')

# ─── 6c. Door OPEN: flood_fill, set_voxel, mesher, atmos ───────────────────
OLD6c = '''\
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
                                mesher.enqueue_batch(server.world().dirty_chunks(), server.world());
                                DoorGroup dg;
                                dg.voxels = std::move(grp_voxels);
                                animating_doors.push_back(std::move(dg));'''

NEW6c = '''\
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
                                if (active_world != &server.world()) {
                                    dg.ctx_world  = active_world;
                                    dg.ctx_atmos  = active_atmos;
                                    dg.ctx_mesher = active_mesher;
                                }
                                animating_doors.push_back(std::move(dg));'''

src = replace_once(src, OLD6c, NEW6c, 'door_open_ctx')

# ─── 6d. Door CLOSE: flood_fill, set_voxel, mesher, on_door_changed ────────
OLD6d = '''\
                        } else if (door_open_type_id != 0
                                   && hit_v.type_id == door_open_type_id
                                   && (hand_empty || has_id)) {
                            auto grp_voxels = flood_fill_door(fhit.voxel, door_open_type_id);'''

NEW6d = '''\
                        } else if (door_open_type_id != 0
                                   && hit_v.type_id == door_open_type_id
                                   && (hand_empty || has_id)) {
                            auto grp_voxels = flood_fill_door(*active_world, fhit.voxel, door_open_type_id);'''

src = replace_once(src, OLD6d, NEW6d, 'door_close_floodfill')

# Replace the close door's set_voxel + player check section
OLD6e = '''\
                                if (!player_inside) {
                                    // Play closing animation backwards
                                    Voxel anim_v;
                                    anim_v.type_id = door_anim_type_id;
                                    anim_v.flags   = VFLAG_SOLID | VFLAG_VERT_PLANE_Z;
                                    for (auto& p : grp_voxels)
                                        server.world().set_voxel(p, anim_v);
                                    mesher.enqueue_batch(server.world().dirty_chunks(), server.world());
                                    // Door panels are now solid — trigger an immediate zone
                                    // split so the sealed side stops losing gas to space
                                    // even before the closing animation completes.
                                    server.atmos().on_door_changed(fhit.voxel);
                                    DoorGroup dg;
                                    dg.voxels  = std::move(grp_voxels);
                                    dg.frame   = renderer.door_anim_frame_count() - 1;
                                    dg.closing = true;
                                    animating_doors.push_back(std::move(dg));'''

NEW6e = '''\
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
                                    active_atmos->on_door_changed(fhit.voxel);
                                    DoorGroup dg;
                                    dg.voxels  = std::move(grp_voxels);
                                    dg.frame   = renderer.door_anim_frame_count() - 1;
                                    dg.closing = true;
                                    if (active_world != &server.world()) {
                                        dg.ctx_world  = active_world;
                                        dg.ctx_atmos  = active_atmos;
                                        dg.ctx_mesher = active_mesher;
                                    }
                                    animating_doors.push_back(std::move(dg));'''

src = replace_once(src, OLD6e, NEW6e, 'door_close_ctx')

# ═══════════════════════════════════════════════════════════════════════════════
# 7. Queue vehicle world items for rendering (append after main items)
# ═══════════════════════════════════════════════════════════════════════════════
OLD7 = '''\
            renderer.queue_world_items(server.entities(), hovered_item_entity,
                                       cam_pos, cam_yaw, cam_pitch);'''

NEW7 = '''\
            renderer.queue_world_items(server.entities(), hovered_item_entity,
                                       cam_pos, cam_yaw, cam_pitch);
            // Append vehicle-local items at their world-space positions
            if (test_vehicle)
                renderer.queue_world_items(test_vehicle->entities(), NULL_ENTITY,
                                           cam_pos, cam_yaw, cam_pitch,
                                           test_vehicle->world_pos_f(), /*clear_first=*/false);'''

src = replace_once(src, OLD7, NEW7, 'queue_vehicle_items')

# ════════════════════════════════════════════════════════════════════════════════
# Also update the vehicle tick to tick world_items (lighting dirty chunk re-enqueue)
# ════════════════════════════════════════════════════════════════════════════════
OLD8 = '''\
            if (test_vehicle) test_vehicle->tick_world_pos(eff_dt);'''

NEW8 = '''\
            if (test_vehicle) {
                test_vehicle->tick_world_pos(eff_dt);
                // Re-enqueue any chunks dirtied by lighting updates or item physics
                World& vw2 = test_vehicle->world();
                auto dirty = vw2.dirty_chunks();
                if (!dirty.empty())
                    vehicle_mesher.enqueue_batch(dirty, vw2);
            }'''

src = replace_once(src, OLD8, NEW8, 'vehicle_tick_lighting_dirty')

# ─── write out ────────────────────────────────────────────────────────────────
if src == original:
    print('ERROR: no changes made!', file=sys.stderr)
    sys.exit(1)

with open(MAIN, 'w', encoding='utf-8') as f:
    f.write(src)

print(f'OK: applied {len(edits)} edits: {", ".join(edits)}')
