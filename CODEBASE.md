# VoxelStation 13 — Codebase Documentation

**Engine version:** 0.1  
**Last updated:** 2026-05-06

This document covers the entire codebase: architecture, every system, data formats, known gaps, and how to extend the game with new content or mechanics.

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Building the Project](#2-building-the-project)
3. [Directory Structure](#3-directory-structure)
4. [Core Layer (`src/core/`)](#4-core-layer-srccore)
5. [Simulation Layer (`src/simulation/`)](#5-simulation-layer-srcsimulation)
6. [Data/Registry Layer (`src/data/`)](#6-dataregistry-layer-srcdata)
7. [Inventory System (`src/inventory/`)](#7-inventory-system-srcinventory)
8. [Rendering (`src/render/`)](#8-rendering-srcrender)
9. [UI Panels (`src/ui/`)](#9-ui-panels-srcui)
10. [Input (`src/input/`)](#10-input-srcinput)
11. [Networking (`src/network/`)](#11-networking-srcnetwork)
12. [Audio (`src/audio/`)](#12-audio-srcaudio)
13. [Data Files (`data/`)](#13-data-files-data)
14. [Verb / Interaction System](#14-verb--interaction-system)
15. [Known Gaps & Unfinished Features](#15-known-gaps--unfinished-features)
16. [How to Add or Change Things](#16-how-to-add-or-change-things)

---

## 1. Project Overview

VoxelStation 13 is a first-person multiplayer survival/roleplay game engine inspired by [Space Station 13](https://spacestation13.com). The core design shift is replacing SS13's 2-D tile grid with a **3-D voxel grid** while keeping the rich per-tile simulation depth (atmosphere, power grids, pipe networks, chemistry) that makes SS13 compelling.

Key design choices:
- **First-person only** — no top-down view.
- **Alt-mode UI** — hold Alt to unlock the cursor for inventory management without pausing the world.
- **Fully data-driven** — voxel types, items, mob species, reactions, and game modes are defined in JSON files; the engine recompiles nothing when you change them.
- **Server-authoritative networking** — clients send input, the server simulates, state is replicated.
- **TG SS13 parity** — systems mirror TG-station13's architecture (ZAS atmos, COMSIG signals, master controller, type-path inheritance, damage buckets).

---

## 2. Building the Project

### Dependencies
| Dependency | Source | Notes |
|------------|--------|-------|
| CMake ≥ 3.25 | System | `C:\Program Files\CMake` |
| Ninja | System (via VS Build Tools) | Generator used in `build_ninja/` |
| SDL3 | System installed | Must be found by `find_package(SDL3)` |
| GLM 1.0.1 | FetchContent (auto) | Header-only math |
| stb | FetchContent (auto) | Header-only image decode |
| Python 3.x | Optional | Needed to regenerate shader headers and textures |

### Configure
```powershell
cmake -S . -B build_ninja -G Ninja
```

### Build (Debug)
```powershell
cmake --build build_ninja
```

### Run client
```powershell
.\build_ninja\vs13.exe
```

### Run server
```powershell
.\build_ninja\vs13_server.exe
```

### Regenerate shaders (after editing shader source in `tools/gen_shaders.py`)
```powershell
python tools/gen_shaders.py
```
This writes SPIR-V bytecode arrays as C++ headers into `src/render/shaders/`.

### VS Code tasks (`.vscode/tasks.json`)
- **Build (Debug)** — runs cmake --build
- **Run** — build then launch client
- **Build & Run** — single command
- **CMake Configure** — reconfigure
- **Gen Shaders** — run `tools/gen_shaders.py`
- **Server Test: Launch Server / Client 1 / Client 2** — multi-process test

---

## 3. Directory Structure

```
voxelstation13/
├── CMakeLists.txt          # build configuration
├── DESIGN.md               # original design document
├── CODEBASE.md             # this file
├── src/
│   ├── main.cpp            # client entry point — wires all systems together
│   ├── server_main.cpp     # dedicated server entry point
│   ├── stb_impl.cpp        # stb single-header implementations
│   ├── core/               # engine primitives (ECS, world, game loop, signals)
│   ├── simulation/         # game logic (physics, atmos, power, reagents, …)
│   ├── data/               # registries + map I/O
│   ├── inventory/          # item definitions, inventory slots, containers
│   ├── render/             # SDL3 GPU renderer, chunk mesher, lighting
│   ├── ui/                 # immediate-mode UI panels
│   ├── input/              # input manager, alt-mode
│   ├── network/            # server/client networking
│   └── audio/              # spatial audio manager
├── data/
│   ├── voxel_types/        # JSON voxel type definitions
│   ├── item_types/         # JSON item definitions
│   ├── mob_species/        # JSON mob species + slot layouts
│   ├── reactions/          # JSON chemistry reaction tables
│   ├── game_modes/         # JSON game mode descriptors
│   └── sounds.json         # sound event table
├── textures/               # PNG sprite sheets
├── maps/
│   └── current.json        # saved map (loaded at startup)
├── models/                 # .bbmodel / .mesh static model files
├── tools/                  # Python helper scripts
└── legacysets/             # extracted BYOND .dmi sprite sheets
```

---

## 4. Core Layer (`src/core/`)

### `types.h`
Foundation types used everywhere in the codebase.

| Type/Enum | Purpose |
|-----------|---------|
| `EntityID` (uint32) | ECS entity handle; `NULL_ENTITY = 0` |
| `Voxel` (8 bytes) | Per-cell data: `type_id`, `orientation`, `light_level`, `flags` |
| `VoxelFlag` (bitmask) | `SOLID`, `OPAQUE`, `PASSABLE`, `CLIMBABLE`, `LIGHT_SRC`, `FLAT_PLANE`, `FLAT_TOP`, `VERT_PLANE_Z`, `GAS_PASSABLE`, `BITMASK_FLAT` |
| `FaceDir` (enum) | Six cardinal directions: `PosX/NegX/PosY/NegY/PosZ/NegZ` |
| `FaceLayer` (enum) | 5 overlay layers: `BASE`, `OVERLAY1`, `OVERLAY2`, `OBJECT`, `EFFECT` |
| `FaceTag` (bitmask) | `DENSE`, `OPAQUE`, `CLIMBABLE`, `ELECTRIFIED`, `SLIPPERY`, `ON_FIRE`, `AIRLOCK`, `WINDOW`, `CATWALK` |
| `VoxelFace` | Derived face data: layers, atmos ID, pipe/wire flags |
| `RayHit` | Result of `World::raycast()` |

---

### `world.h` + `world.cpp`
The voxel grid. The world is divided into **16×16×16 chunks** stored in a sparse `unordered_map<glm::ivec3, Chunk>`.

**Chunk internals:**
- Palette-compressed storage: up to 256 distinct voxel types per chunk in a 16-bit palette index array.
- Light levels stored separately from geometry in `m_light_data[4096]`.
- Dirty flags: `m_dirty` (geometry changed) and `m_light_dirty` (light changed).

**Key World API:**
```cpp
world.get_voxel(pos)          // read a voxel
world.set_voxel(pos, voxel)   // write a voxel, marks chunk dirty
world.raycast(origin, dir, max_dist)  // DDA ray traversal → RayHit
world.get_face(pos, dir)      // derive a VoxelFace on the fly
world.get_or_create_sim_face(coord)   // persistent face for atmos/wiring
world.dirty_chunks()          // chunks needing remesh
```

Faces are **lazy**: only faces that need simulation state (atmos membership, wires) are stored persistently in `m_sim_faces`. Everything else is computed on demand.

---

### `entity_manager.h` + `entity_manager.cpp`
Minimal ECS (Entity-Component System). Components are stored as `type_index → (EntityID → std::any)` maps. No archetypes or contiguous memory — optimised for clarity over cache performance.

```cpp
EntityID id = entities.create();
entities.add_component<TransformComponent>(id, {...});
entities.get_component<TransformComponent>(id);  // returns T* (null if absent)
entities.has_component<TransformComponent>(id);
entities.remove_component<TransformComponent>(id);
entities.each<TransformComponent>([](EntityID id, TransformComponent& t) { ... });
entities.destroy(id);  // fires on_destroy callbacks, frees all components
```

**`first_local_id`**: On the client, entities are created starting at a high ID to avoid collisions with server-replicated entities (which start at 1).

---

### `game_loop.h` + `game_loop.cpp`
Fixed-timestep update loop with variable-rate rendering.

```
fixed_dt = 1/60 s  (configurable)
GameLoop::run(update_fn, render_fn)
  ↓ accumulates real time
  ↓ calls update_fn(dt) zero or more times to catch up
  ↓ calls render_fn(alpha) once per frame (alpha = interpolation blend)
```

`alpha` is passed to the renderer so entities can interpolate between their previous and current simulation positions.

---

### `master_controller.h` + `master_controller.cpp`
Mirrors TG SS13's Master Controller / subsystem architecture. Entities subscribe to named **processing lists**; each tick the list fires all registered callbacks.

**Well-known lists** (tick order):
1. `"living"` — health updates, oxygen ticks, status-effect decay
2. `"mobs"` — mob AI, player-controlled entities
3. `"objects"` — machinery, doors, etc.
4. `"items"` — floating-item physics helpers

```cpp
master_controller().start_processing("mobs", entity_id,
    [](EntityID id, double dt) { /* ... */ });
master_controller().stop_processing("mobs", entity_id);
master_controller().tick(dt);  // fires all lists in order
master_controller().purge(entity_id);  // removes from all lists
```

**Macros:**
```cpp
START_PROCESSING("mobs", id, fn);
STOP_PROCESSING("mobs", id);
```

---

### `signals.h` + `signals.cpp`
Event bus mirroring TG SS13's `COMSIG` / `SEND_SIGNAL` / `REGISTER_SIGNAL` system.

```cpp
SignalHandle h = signals.register_signal(entity, COMSIG_MOB_DEATH,
    [](EntityID src, const SignalArgs& args) -> SignalResult {
        // handle death
        return 0;  // 0 = continue; COMPONENT_SIGNAL_CANCEL = block default
    });
signals.send_signal(entity, COMSIG_MOB_DEATH);
signals.unregister_signal(h);
signals.purge_entity(entity);  // call when entity is destroyed
```

**Predefined signal names** (partial list, see `signals.h`):
- `COMSIG_ATOM_CLICK`, `COMSIG_ATOM_ATTACKED`, `COMSIG_ATOM_ATTACK_HAND`, `COMSIG_ATOM_ATTACK_ITEM`
- `COMSIG_MOB_DEATH`, `COMSIG_MOB_HEALTHCHANGE`
- `COMSIG_ITEM_PICKUP`, `COMSIG_ITEM_DROP`

---

### `object_types.h` + `object_types.cpp`
Type-path prototype system mirroring TG SS13's `/obj/item/tool/wrench` DM type hierarchy.

```
/atom/movable/obj/item/tool/wrench   ← type path
/atom/movable/mob/living/carbon/human
/turf/floor/plating
```

```cpp
istype("/obj/item/tool/wrench", "/obj/item")  // → true
parent_type("/obj/item/tool/wrench")           // → "/obj/item/tool"
type_ancestry("/obj/item/tool/wrench")         // → full chain to /atom
type_short("/obj/item/tool/wrench")            // → "wrench"
```

Used by the item and voxel prototype-inheritance systems.

---

### `verb_dispatch.h` + `verb_dispatch.cpp`
Named handler registry for item/voxel/mob verbs. Handler names are strings stored in JSON item definitions. At runtime, the string maps to a `std::function`.

Covered in detail in [Section 14](#14-verb--interaction-system).

---

## 5. Simulation Layer (`src/simulation/`)

### `physics.h` + `physics.cpp`
Kinematic character controller. No rigid-body physics — entities move by setting `wish_move` and the system resolves voxel collisions.

**Key components:**

| Component | Purpose |
|-----------|---------|
| `TransformComponent` | `pos`, `prev_pos` (for rendering interpolation), `yaw`, `pitch` |
| `VelocityComponent` | `linear` velocity (used for floating items and projectiles) |
| `CharacterControllerComponent` | `move_speed`, `sprint_mult`, `height`, `radius`, `on_ground`, `zero_g`, `noclip`, `wish_move`, `mob_state` |

**Movement states (`MobState`):**
- `Normal` — full speed
- `Resting` — voluntarily lying down, slow crawl
- `Softcrit` — stunned/soft-crit, very slow crawl
- `Hardcrit` — fully incapacitated, cannot move

**Key API:**
```cpp
physics.prepare_character_movement(id, wish_dir, sprint, grab_wall);
physics.move_character(id, wish_dir, sprint, grab_wall, dt);
physics.apply_wind(id, force);          // decompression wind
physics.set_bump_callback(callback);    // fires when entity bumps a dense entity
physics.set_model_objects(mgr);         // register model object collision
physics.add_aux_world(vehicle_world, offset);  // vehicle grid collision
```

**Zero-G:** when `CharacterControllerComponent::zero_g = true`, gravity is disabled. Jetpack thrust is applied via `jetpack_input`.

**Important:** Jump is intentionally absent from the design. Do not add jump mechanics.

---

### `atmos.h` + `atmos.cpp`
Per-cell atmospheric simulator, TG ZAS-inspired.

**Gas mixture (`GasMixture`):** stores partial pressures (kPa) for: O₂, N₂, CO₂, Plasma, N₂O, BZ, Tritium, and temperature (Kelvin).

**AtmosZone:** every enclosed passable voxel is its own zone (`cell_count = 1`). Gas flows between adjacent zones each tick.

**Tick behavior:**
1. Cell-to-cell gas flow at `CONDUCTANCE_CELL` rate
2. Door links seep at `CONDUCTANCE_CLOSED` when shut or `CONDUCTANCE_CELL` when open
3. Space drain: cells adjacent to vacuum drain rapidly
4. Fire hotspots: plasma + O₂ above ignition temperature starts burning
5. Temperature diffusion between adjacent cells
6. Entity effects: oxy deprivation, fire damage, etc.

**Key API:**
```cpp
atmos.rebuild_zones();            // full BFS rebuild (call after map load)
atmos.tick(dt);
atmos.on_voxel_changed(pos);      // incremental update after a voxel change
atmos.on_door_changed(pos);
atmos.zone_at(pos)                // → AtmosZoneID
atmos.mix_at(pos)                 // → GasMixture
atmos.inject_gas(zone_id, delta); // spawn gas into a zone
atmos.try_ignite(zone_id);        // start a fire hotspot
```

**`AtmosStatus` bitmask:** `LOW_O2`, `LOW_PRESS`, `HIGH_CO2`, `TOXIC`, `FIRE`, `DECOMP`, `HIGH_TEMP`

---

### `power.h` + `power.cpp`
Power grid graph simulation.

**Node types:** `Wire`, `Generator`, `APC`, `Machine`, `Light`, `Airlock`

Each node has: `voltage`, `current`, `demand_w` (consumption), `supply_w` (generators), `powered` flag.

```cpp
power.rebuild();          // scan world for wire voxels, build graph
power.tick(dt);
power.on_wire_changed(pos);
power.powered_at(pos);    // → bool — is this voxel position powered?
power.add_node(node);     // manually add a node (machinery setup)
power.remove_node(id);
```

**Status:** Currently the wire graph is built from `wire` voxel types. Generator/APC placement is done via `add_node()`. Load balancing and APC area management are implemented as `balance_load()`.

**Unfinished:** No in-world machinery actually hooks into the power grid at runtime yet — the infrastructure exists but game objects don't consume power.

---

### `pipes.h` + `pipes.cpp`
Pipe network for fluid/reagent transport.

**Node types:** `Pipe`, `Pump`, `Valve`, `Scrubber`, `Injector`

Each node holds a `FluidPacket` (list of `{id, volume}` reagents + temperature).

```cpp
pipes.rebuild();
pipes.tick(dt);
pipes.on_pipe_changed(pos);
pipes.add_node(node);     // add pump or injector
pipes.node_at(pos);       // find node at voxel
```

`flow_step()` moves reagents from a source to a destination based on `flow_rate`.  
`react()` checks if the resulting mixture triggers a chemistry reaction.

**Unfinished:** Reaction table lookups in `react()` are a stub. The chemistry system (`reagents.h`) is defined but reactions aren't wired into the pipe system yet.

---

### `reagents.h` + `reagents.cpp`
Chemistry/reagent system mirroring TG SS13's `/datum/reagents`.

**`ReagentDef`** (static per-type, loaded from JSON):
- `metabolise_rate`: units removed per tick
- `overdose_threshold`: cumulative dose for overdose
- Per-tick health effects: `oxy_damage`, `tox_damage`, `heal_brute`, `heal_burn`, etc.
- Status effects applied per tick: `stun_time`, `knockdown_time`, `slowdown`, etc.

**`ReagentContainer`** (mutable instance):
```cpp
container.add("morphine", 10.f);
container.remove("morphine", 5.f);
container.transfer_to("water", 5.f, other_container);
container.splash_to(20.f, other_container);  // proportional transfer
```

**`ReagentContainerComponent`**: ECS component. Attach to mobs (bloodstream), beakers, syringes, food.

**`metabolise_reagents(entity, entities, registry, signals, dt)`**: Call each tick on mobs. Applies health effects and removes metabolised doses.

**Unfinished:** Reaction logic (`reaction_fire()`) is a skeleton. Chemistry reactions defined in `data/reactions/chemistry.json` are not yet processed.

---

### `status_effects.h` + `status_effects.cpp`
Status effect tracking mirroring TG SS13's `/datum/status_effect` hierarchy.

**Effect types:**
| Type | Effect |
|------|--------|
| `Stun` | Cannot act → Hardcrit MobState |
| `Paralysis` | Catatonic, like Stun but harder to cure |
| `Knockdown` | Lying prone → Softcrit MobState |
| `Slowdown` | Reduces move speed by `strength` (0..1) |
| `Drowsy` | Mild slowdown; prolonged causes Knockdown |
| `Blind` | Renderer applies vision blackout |
| `Deaf` | Suppresses audio |
| `Confusion` | Random direction drift added to `wish_move` |
| `Silence` | Cannot speak/radio |
| `Jitter` | Camera micro-shake |
| `Dizzy` | Screen tilt overlay |

```cpp
comp.apply(StatusEffectType::Stun, 2.f);          // 2 seconds
comp.has(StatusEffectType::Knockdown);
comp.remove(StatusEffectType::Blind);
MobState ms = comp.tick(dt);                       // advances timers, returns severity
float spd = comp.speed_multiplier();               // 0=stopped, 1=full speed
```

Duration is refreshed via `max(existing, new)` — same as TG grouped status effects.

---

### `attack_chain.h` + `attack_chain.cpp`
Combat/click-chain handler mirroring TG SS13's `attack_chain()` proc.

**Flow** (left-click on a mob):
1. `attack_chain(ctx, entities, signals)` is called
2. If attacker has a weapon → `attack_item()` → applies weapon damage + fires `COMSIG_ITEM_ATTACK` + `COMSIG_ATOM_ATTACKED`
3. If empty-handed → `attack_hand()` → disarm attempt or weak punch, fires `COMSIG_ATOM_ATTACK_HAND`
4. Damage applied to target's `HealthComponent`

**Grab cycling (TG-style):**
```
None → Passive → Aggressive → Neck → Kill
```
The `GrabbedComponent` tracks who is grabbing and the current `GrabState`.

`bump_attack()` is called by `PhysicsSystem` when an entity's movement is blocked by a dense entity (TG's `Bump()` proc).

---

### `projectile_system.h` + `projectile_system.cpp`
Fired projectile entities.

**`ProjectileComponent`:** direction, speed, damage, damage_type, owner, lifetime, stam_damage, stun_dur, hit_radius.

```cpp
EntityID proj = projectiles.spawn(muzzle_pos, props);
projectiles.tick(dt, signals);  // advances all in-flight projectiles
```

Each tick: move `direction * speed * dt`, check voxel collision (destroy on hit), check mob sphere intersection (apply damage + stun, destroy).

**Gun tag format** (on `ItemDef::tags`):
- `proj_dmg:20` — damage amount
- `proj_type:bullet` — "bullet", "shot", "laser", "taser", "disabler", "arrow"
- `proj_speed:35` — travel speed m/s
- `proj_stun:2` — stun seconds (energy weapons)
- `proj_stam:40` — stamina damage (taser)
- `mag_size:8` — magazine capacity
- `energy_max:100` — energy cell capacity (energy guns)

---

### `npc_ai.h` + `npc_ai.cpp`
Simple state-machine AI for non-player mobs.

**States:** `Idle` → `Wander` → `Flee` / `Chase`

The `NpcAiComponent` (defined in `mob_system.h`) stores: `spawn_pos`, `aggro_dist`, `flee_dist`, `flee_speed_mult`, `threat_eid`, and timers.

`tick_npc_ai()` writes `cc->wish_move` so `PhysicsSystem::tick()` sees a valid movement vector. Must be called **before** `PhysicsSystem::tick()`.

---

### `mob_system.h`
Component definitions for mobs (no separate .cpp — behavior split across physics and NPC AI).

Key components:

| Component | Purpose |
|-----------|---------|
| `MobComponent` | Rendering: `species`, `variant` for billboard sprite selection |
| `HealthComponent` | TG damage buckets: `brute`, `burn`, `tox`, `oxy`, `clone`; `health_max`, `dead`, `crit` flags |
| `NpcAiComponent` | AI state, patrol points, aggro/flee settings |

**HealthComponent API:**
```cpp
health.apply("brute", 20.f);      // deal 20 brute damage
health.heal_type("burn", 10.f);   // heal 10 burn
health.current()                  // health_max - total_damage (can go negative)
health.total_damage()
health.stamina_pct()              // 1 = rested, 0 = KO'd
```

---

### `world_items.h` + `world_items.cpp`
Items as entities in the voxel world.

**`WorldItemComponent`:** resting (on a voxel face, with scatter offset) or floating (with physics velocity).

```cpp
world_items.spawn(item_stack, pos, face, scatter_offset);  // spawn resting on a face
world_items.spawn_floating(item_stack, pos, velocity);     // spawn with velocity
world_items.pick_up(entity_id)    // returns ItemStack, destroys entity
world_items.tick(dt)              // advances floating physics, rests on floor
world_items.build_labels(entities, cam_pos, vp, fb_w, fb_h)  // screen-space labels for UI
```

Items are rendered as camera-facing billboards (floating) or flat quads on voxel faces (resting).

---

### `model_objects.h` + `model_objects.cpp`
Static 3-D model objects placed in the world (e.g. lockers, SMES units).

**`StaticModelObject`:** `name` (model key), `cell` (base voxel), `yaw`, `scale`, `blocks_mobs`, `blocks_gas`.

```cpp
model_objs.register_extents("locker", local_min, local_max);
int id = model_objs.add({.name="locker", .cell={5,0,3}, .yaw=90.f});
model_objs.remove(id);
model_objs.blocks_mob_at(cell)  // → bool (used by physics)
model_objs.blocks_gas_at(cell)  // → bool (used by atmos)
```

---

### `enclosure.h` + `enclosure.cpp`
Detects whether a region is enclosed (useful for atmos zone classification, airlock logic, containment breach).

Uses 6-connected BFS. Regions larger than `k_open_threshold = 2048` cells are treated as open space.

```cpp
EnclosureDetector enc(world);
enc.is_enclosed(pos);   // true = in a bounded room
enc.invalidate();        // call after any world change
```

---

### `liquids.h` + `liquids.cpp`
Liquid puddle simulation (inspired by Monkestation2.0 SSliquids).

**`LiquidGroup`:** a contiguous set of cells sharing a `ReagentContainer` pool. Volume is divided equally across cells.

**Tick phases (2 Hz, `LIQUID_TICK_DT = 0.5s`):**
1. **Fall** — drop to passable cell below if floor is missing
2. **Spread** — expand laterally when `volume_per_cell ≥ LIQUID_SPREAD_THRESHOLD` (10 units)
3. **Merge** — unite groups that touch each other
4. **Evaporate** — volumes below `LIQUID_EVAP_THRESHOLD` (1 unit/cell) vanish
5. **Expose** — mobs standing in liquid receive reagent contact

**Visual states:** `Puddle`, `Ankles`, `Waist`, `Shoulders`, `FullTile`

```cpp
liquids.add_liquid(pos, "water", 50.f);
LiquidGroupID gid = liquids.group_id_at(pos);
LiquidGroup* g = liquids.group(gid);
liquids.tick(dt);
```

---

### `vehicle.h` / `vehicle_manager.h` + `.cpp`
Self-contained simulation zones that can dock to the main map.

A `VehicleGrid` owns a full simulation stack: `World`, `EntityManager`, `AtmosSimulator`, `PowerGrid`, `PipeNetwork`, `PhysicsSystem`, `WorldItemSystem`, `LiquidSimulator`.

**States:** `InSpace` → `Docking` → `Docked` → `Undocking`

```cpp
VehicleManager mgr;
VehicleID vid = mgr.create("shuttle");
VehicleGrid& v = mgr.get(vid);
v.set_anchor({100, 0, 100});
v.set_state(VehicleState::Docked);
mgr.tick_all(dt);
```

Entities cross between the vehicle and the main map through `DockPort` connections. Atmos gas equalises between the two simulators through the open hatch.

---

## 6. Data/Registry Layer (`src/data/`)

### `voxel_registry.h` + `voxel_registry.cpp`
Loads all `data/voxel_types/*.json` files and resolves the prototype chain.

**`VoxelTypeDef`** fields: `id`, `name`, `icon`, `default_flags` (VoxelFlag bitmask), `default_tags` (FaceTag bitmask), `health`, `material`, `on_hit`, `on_walk`, `pry_item`, `emit_light` + RGB, per-face texture overrides, atlas indices.

```cpp
VoxelRegistry reg;
reg.load_directory("data/voxel_types");
const VoxelTypeDef* def = reg.get("floor");
uint16_t type_id = reg.id_of("reinforced_wall");
```

Prototype inheritance: a type with `"parent": "wall"` inherits all unset fields from `wall` at load time.

---

### `item_registry.h` + `item_registry.cpp`
Loads all `data/item_types/*.json` files. Covered in detail in `item_system_analysis.md` (repo memory).

Key: `ItemDef` has a type-path string (e.g. `/obj/item/tool/wrench`), tags, verbs, weight, volume, container properties. Prototype inheritance via `"parent"` field.

---

### `mob_species_registry.h` + `mob_species_registry.cpp`
Loads `data/mob_species/*.json`. Returns `MobSpeciesDef` with stats and inventory slot layout.

```cpp
MobSpeciesRegistry mob_reg;
mob_reg.load_directory("data/mob_species");
const MobSpeciesDef* def = mob_reg.get("human");
// def->slots = [{ "head", ["hat","helmet","mask"] }, ...]
```

---

### `map_io.h` + `map_io.cpp`
JSON map serialization.

**Format v1:** voxels only.
**Format v2:** voxels + world items + mob spawns + static model objects.

```cpp
map_save_full(world, voxel_reg, entities, item_reg, model_objs, "maps/current.json");
map_load_full(world, voxel_reg, entities, world_items, item_reg, mob_reg, model_objs, "maps/current.json");
```

---

### `data_validator.h` + `data_validator.cpp`
Validates all `data/` JSON files at startup. Logs errors, returns `false` if any critical errors are found.

```cpp
DataValidator validator;
validator.validate_all("data");
for (auto& err : validator.errors()) SDL_Log("%s: %s", err.file.c_str(), err.message.c_str());
```

---

## 7. Inventory System (`src/inventory/`)

See `memories/repo/item_system_analysis.md` for deep coverage. Summary:

### `ItemDef` and `ItemStack`
- `ItemDef*`: static type definition (loaded from JSON)
- `ItemStack`: instance of an item: `def`, `count`, `integrity`, `custom_name`, `contents` (for containers)

### `InventorySlot`
- Categories: `General`, `Equipment`, `BeltTool`, `Container`
- `accepts_tags`: tag filter (empty = any)
- `volume_capacity`: volume limit
- Child slots for open containers

### `Inventory`
- Collection of `InventorySlot`
- `auto_equip(item)`: tries active hand → other hand → pockets → equip slot → any slot
- `take(slot_id)`: remove item from slot
- `open_container(slot_id)`: populate child slots from container contents
- `close_container(slot_id)`: sync child slots back to contents
- `cycle_active_hand()`: toggle left/right active hand
- Two-handed enforcement: blocks the other hand slot when a two-handed item is held

**`make_player_inventory(species, mob_reg)`** (in `inventory.cpp`): builds the slot list from a species definition.

---

## 8. Rendering (`src/render/`)

### `renderer.h` + `renderer.cpp`
Main SDL3 GPU renderer.

**Frame flow:**
```
renderer.begin_frame(alpha)          // acquire swapchain texture
renderer.queue_highlight(ray_hit)    // mark selected face
renderer.queue_world_items(...)      // prepare item billboard quads
renderer.draw_world(world, cam, ...) // upload dirty chunks, draw geometry
renderer.draw_face_highlight(hit)    // draw selection outline
renderer.draw_world_items()          // draw item quads
renderer.draw_viewmodel(type_id)     // draw held item in view-model pass
renderer.end_world_pass()            // end 3-D render pass
// ui_renderer.begin() / draw calls / ui_renderer.end(...)
renderer.end_frame()                 // submit GPU command buffer
```

**Texture loading:**
```cpp
renderer.load_tile_textures(voxel_reg, "textures");   // tile atlas
renderer.load_item_textures(item_reg, "textures");     // item icon atlas
renderer.load_mob_textures("textures/mobs");           // mob sprites
renderer.load_human_bodyparts("legacysets/extracted"); // BYOND bodyparts
renderer.load_door_anim("path/door.gif", type_id);    // door animation
```

**Dynamic lights:**
```cpp
lighting.add_dynamic_light(pos, level, entity_id, color);
lighting.remove_dynamic_light(entity_id);
lighting.rebuild_dynamic();  // call each frame for moving lights
```

---

### `chunk_mesher.h` + `chunk_mesher.cpp`
Generates GPU-ready meshes from dirty chunks on background threads.

- **Greedy meshing**: groups coplanar same-material faces into quads.
- Vertex layout: position (3f), normal (3f), UV + layer info.
- Produces `ChunkMesh` with opaque and transparent (overlay) geometry.
- `generation` stamp prevents stale results from being uploaded.

---

### `lighting.h` + `lighting.cpp`
Block light, sky light, and colored dynamic point lights.

- **Block light**: BFS flood-fill from emissive voxels, 0-15 range, -1 per cell.
- **Sky light**: top-down column propagation from the highest unoccluded voxel.
- **Dynamic lights**: per-entity point lights (flashlights, fires); rebuilt each frame via `rebuild_dynamic()`.
- **Colored light**: `LightMap` stores per-cell RGB tint for non-white lights.
- **AO**: per-vertex ambient occlusion for smooth lighting.
- **`flood_border_sky(level)`**: used on vehicle grids to ensure exterior faces are lit.

---

### `ui_renderer.h` + `ui_renderer.cpp`
Immediate-mode 2-D renderer layered over the world pass. CPU-side draw list uploaded each frame.

```cpp
ui.begin();
ui.rect({10, 10}, {200, 50}, {1,0,0,1});          // colored rectangle
ui.text({15, 20}, "Hello", {1,1,1,1}, 16.f);       // text
ui.icon({10, 10}, {32, 32}, atlas_index, 1.f);     // tile icon
ui.image({10, 10}, {64, 64}, texture);             // texture quad
ui.image_tinted({10, 10}, {64, 64}, tex, tint);   // tinted sprite
ui.end(cmd_buf, swapchain_tex, fb_w, fb_h);
```

Uses MTSDF font rendering (Roboto) for crisp text at all sizes.

---

### `model_loader.h` + `model_loader.cpp`
Loads `.bbmodel` (Blockbench) and `.mesh` (custom) 3-D model files for static objects (lockers, SMES units, etc.).

---

## 9. UI Panels (`src/ui/`)

All UI is immediate-mode — no persistent widget state. Each panel is a class with a `draw(cursor, lmb, ...)` method called every frame.

### `hud.h` + `hud.cpp`
TG SS13-style persistent HUD. Always visible in play mode.

**Sections:**
- Health panel (right side): damage doll, brute/burn/tox/oxy/clone bars
- Bottom bar: hand slots (left/right), intent selector (Help/Disarm/Grab/Harm), body zone targeting doll
- Equipment slots: head, suit, back, belt, uniform, id, pda
- Radio log (last 30 messages, bottom-left)
- Examine label (selected face description)
- Clock

**`HUDState`** is filled by the game loop each frame and passed to `HUD::draw()`.

**Intent enum:** `Help`, `Disarm`, `Grab`, `Harm` — controls attack behavior in `attack_chain()`.

---

### `inventory_panel.h` + `inventory_panel.cpp`
Alt-mode inventory overlay. Shows equipment silhouette + hands/pockets.

- Drag-and-drop between slots (shift = split stack)
- Container sub-panel (grid of child slots)
- `DropToWorld` action when item dragged outside panel
- Volume/weight stats

---

### `map_editor.h` + `map_editor.cpp`
Full-screen top-down voxel map editor (F7 to toggle).

**Tabs:** Voxels, Items, Mobs, Objects

**Voxel tab controls:**
- LMB drag = Brush paint
- RMB drag = Erase
- Shift+LMB = Rectangle fill
- B/F/R = Brush/Fill/Rect tool
- Q/E = rotate orientation
- Alt+LMB = Eyedropper
- Ctrl+C/V = Copy/Paste selection

**Objects tab:** place static model objects; Q/E to rotate.

**Ctrl+Z/Y:** undo/redo (voxels + model objects; mob/item spawns are undo-only).

**Ctrl+S / Ctrl+L:** save/load `maps/current.json`.

---

### `admin_menu.h` + `admin_menu.cpp`
F1 overlay with debug/admin toggles. Results reported via `AdminMenuResult` struct.

**Toggles:** noclip, godmode, build_mode, gas_overlay, debug_overlay, wireframe, fullbright, AO, auto_heal, zero-G override, infinite oxy, freeze_sim, slow_motion.

**One-shot actions:** full heal, kill player, teleport to origin, force atmos rebuild, spawn test items.

---

### `character_creator.h` + `character_creator.cpp`
TG SS13-style character setup screen (accessible from main menu).

**`CharacterProfile`:** name, sex, skin color, hair style + color, facial hair + color, eye color, species.

Composites bodypart sprites from `legacysets/extracted` for a live preview.

---

### `main_menu.h` + `main_menu.cpp`
Full-screen pre-game menu. Buttons: Play, Setup Character, Exit.

### `pause_menu.h` + `pause_menu.cpp`
In-game pause overlay (Esc).

### `debug_overlay.h` + `debug_overlay.cpp`
FPS, tick count, position, chunk info overlay.

### `gas_overlay.h` + `gas_overlay.cpp`
Visualizes atmosphere zone data (pressure, temperature, gas composition) as a color overlay on the world.

### `player_stats_overlay.h` + `player_stats_overlay.cpp`
Shows detailed player stats (all damage buckets, stamina, status effects) during development.

### `creative_menu.h` + `creative_menu.cpp`
Quick item-spawning menu for testing.

### `context_menu.h` + `context_menu.cpp`
Right-click verb context menu (shown over a voxel face or world item).

---

## 10. Input (`src/input/`)

### `input_manager.h` + `input_manager.cpp`
Named action abstraction over SDL scancodes/mouse buttons.

**Actions:**
```
MoveForward/Back/Left/Right, GrabWall, Sprint,
PrimaryInteract, SecondaryInteract,
PickUp, ThrowItem, DropItem, SwitchHand,
ExamineMode, AltMode, ChatOpen, Escape, ToggleCamera, Rest
```

```cpp
input.is_held(Action::Sprint)       // true while key held
input.is_pressed(Action::PickUp)    // true on first frame
input.consume_press(Action::SwitchHand)  // clears flag after read
input.mouse_delta()                 // frame mouse movement
input.scroll_delta() / consume_scroll()
input.capture_cursor(window, true)  // lock cursor for FPS look
```

**No jump action**: intentionally absent per design. Do not add it.

---

### `alt_mode.h` + `alt_mode.cpp`
Manages the Alt-key mode transition:
- Releases OS cursor, freezes camera, fades in inventory overlay.
- Restores camera and re-captures cursor on Alt release.

```cpp
AltMode alt(input, window);
alt.set_camera_angles(yaw, pitch);  // update frozen angles each frame
bool changed = alt.update();
if (alt.active()) {
    glm::vec2 cursor = alt.cursor_pos();
    float alpha = alt.overlay_alpha();  // 0-1 for fade animation
}
```

---

## 11. Networking (`src/network/`)

### `server.h` + `server.cpp`
Authoritative server. Owns all simulation state. Replicates to clients.

**Packet types (server → client):**
- `ChunkData` / `ChunkDelta` — compressed voxel data
- `EntityState` / `EntitySpawn` — entity position/component updates
- `AtmosDelta` — atmosphere zone changes
- `ChatMessage`
- `RoundState` — game phase (lobby/running/end)
- `SpawnInfo` — tells client its entity ID and spawn position
- `AppearanceState` — relayed player appearance

**Packet types (client → server):**
- `InputState` — per-frame input snapshot
- `InteractFace` — player clicked a voxel face
- `InventoryAction` — drag/drop/use inventory
- `ChatSend`
- `Connect` — initial handshake
- `AppearanceUpdate` — client appearance change
- `AdminCmd` — debug/admin command

**Admin commands:** `ToggleNoclip`, `ToggleGodmode`, `ActionFullHeal`, `ActionKillPlayer`, `ActionTeleportOrigin`, `ActionForceAtmos`, `ActionSpawnItems`, `ToggleAutoHeal`, `ToggleZeroGOverride`, `ToggleInfiniteOxy`

**Key API:**
```cpp
server.start(7778);
server.spawn_player("human", {0,1,0});
server.spawn_npc("mouse", {5,1,5});
server.tick(dt);
server.world(), server.entities(), server.atmos(), server.physics(), ...
```

---

### `client.h` + `client.cpp`
Client-side world view. Applies server state deltas, performs client-side prediction for local player movement.

**Loopback mode** (single-player): `Client` delegates directly to an in-process `Server`.
**Remote mode**: sends/receives UDP packets.

```cpp
client.connect("localhost", 7778);
client.send_input(snapshot);
client.tick(dt);
client.interpolate(alpha);
client.local_player();  // → EntityID
client.world(), client.entities()
```

---

### `input_snapshot.h`
Per-frame input packet sent from client to server:
```cpp
struct InputSnapshot {
    glm::vec3 wish_dir;
    float     yaw, pitch;
    bool      sprint, primary_interact, secondary_interact;
    uint64_t  tick_id;  // for lag compensation rewind
};
```

---

## 12. Audio (`src/audio/`)

### `audio_manager.h` + `audio_manager.cpp`
Spatial audio via SDL3 audio streams. Sounds attenuate by distance; vacuum zones suppress propagation.

**`SoundEvent`:** id, file, volume, pitch, max_dist, spatial, looping.

```cpp
audio.init();
audio.load_events("data/sounds.json");
audio.set_listener(pos, forward, up);  // call each frame
SoundHandle h = audio.play("footstep", world_pos);
audio.play_ui("click");               // non-spatial
audio.stop(h);
audio.set_master_volume(0.8f);
audio.set_local_pressure(kpa);        // affects propagation
audio.update(dt);                     // update pan/volume for spatial sounds
```

**`data/sounds.json`:** array of `SoundEvent` objects defining every sound in the game.

---

## 13. Data Files (`data/`)

### `data/voxel_types/base.json`
The single file containing all voxel type definitions. Each entry:
```json
{
  "id":       "floor",
  "parent":   "wall",         // optional — inherit fields from this type
  "name":     "Steel Floor",
  "icon":     "tiles/floor_dark",
  "flags":    ["SOLID", "OPAQUE"],
  "health":   200,
  "material": "steel",
  "pry_item": "steel_tile",   // item spawned when crowbar pries this voxel
  "on_hit":   "deconstruct_wall",  // verb handler name
  "emit_light": 8,            // 0-15, for light sources
  "emit_r": 255, "emit_g": 200, "emit_b": 180,
  "tex_top": "", "tex_bottom": "", "tex_sides": ""  // per-face texture overrides
}
```

**Flags:** `SOLID`, `OPAQUE`, `PASSABLE`, `CLIMBABLE`, `LIGHT_SRC`, `FLAT_PLANE`, `FLAT_TOP`, `VERT_PLANE_Z`, `GAS_PASSABLE`, `BITMASK_FLAT`

### `data/item_types/`
| File | Contents |
|------|---------|
| `tools.json` | Wrench, screwdriver, crowbar, wirecutters, toolbox, stun_baton, fire_extinguisher, id_card |
| `containers.json` | Satchel, duffel bag, briefcase, medical kit, crates, bodybag, mining satchel, ore box, engineering bag |
| `medical.json` | Syringe, beaker, defib, scalpel, blood pack, firstaid kit, pills, gauze, stasis bag |
| `equipment.json` | Hardsuit, helmet variants, jetpack, oxygen tank, backpack |
| `weapons.json` | Telescopic baton, classic baton, fireaxe, claymore, other melee weapons |
| `guns.json` | Sec pistol, M1911, revolver, shotgun, SMG, energy weapons, etc. |
| `ammo.json` | Magazine/ammo types for each gun |

**Item definition format:**
```json
{
  "id":         "wrench",
  "parent":     "some_item_id",     // optional prototype
  "name":       "Wrench",
  "icon":       "items/wrench",
  "tags":       ["tool", "wrench", "small"],
  "weight":     0.4,
  "volume":     1.0,
  "stack_max":  1,
  "two_handed": false,
  "is_container": false,
  "container_volume": 0.0,
  "container_accepts_tags": [],
  "equip_slot": "",                 // e.g. "back", "suit"
  "verbs": [
    { "name": "Wrench", "range": 1.5, "handler": "verb_wrench" },
    { "name": "Examine", "range": 99, "handler": "verb_examine" }
  ]
}
```

### `data/mob_species/`
| File | Species |
|------|---------|
| `human.json` | Playable human (14 equipment slots) |
| `drone.json` | Maintenance drone |
| `mouse.json` | Mouse NPC |

**Species definition format:**
```json
{
  "species":    "human",
  "name":       "Human",
  "health_max": 100,
  "move_speed": 2.25,
  "sprint_mult": 2.0,
  "height": 1.8,
  "radius": 0.4,
  "damage_types": ["brute", "burn", "tox", "oxy"],
  "slots": [
    { "id": "head", "accepts": ["hat", "helmet", "mask"] },
    ...
  ]
}
```

### `data/reactions/chemistry.json`
Chemistry reactions triggered when reagents mix:
```json
{
  "id":    "water_synthesis",
  "reagents": [
    { "id": "hydrogen", "min_volume": 10 },
    { "id": "oxygen",   "min_volume": 5  }
  ],
  "products": [{ "id": "water", "volume_per_tick": 2 }],
  "min_temperature": 293.0,
  "exothermic": false
}
```

### `data/game_modes/extended.json`
```json
{
  "id": "extended",
  "name": "Extended",
  "description": "No antagonists. Pure roleplay and construction.",
  "min_players": 1,
  "antagonist_chance": 0.0,
  "objectives": []
}
```

Only `extended` mode exists. Other modes (Traitor, Nuclear Emergency, etc.) are described in `DESIGN.md` but not implemented.

### `data/sounds.json`
Array of `SoundEvent` objects loaded by `AudioManager::load_events()`.

---

## 14. Verb / Interaction System

Verbs are interactive actions on items or voxel faces. The handler name in JSON maps to a C++ function via `VerbDispatch`.

### Flow
1. Player right-clicks a voxel face or item → `ContextMenu` shows verb list
2. Player selects a verb (or presses E for `PickUp` / F for `ThrowItem`)
3. `VerbDispatch::invoke(handler_name, ctx)` is called
4. The handler reads `VerbContext` (actor, item_def, item_stack, target_voxel, etc.)

### `VerbContext` fields
```cpp
ctx.actor          // EntityID doing the action
ctx.actor_pos      // eye position
ctx.actor_fwd      // view direction
ctx.target_ent     // world entity being acted on (item, mob)
ctx.item_def       // pointer to ItemDef of the held item
ctx.item_stack     // pointer to ItemStack in inventory
ctx.slot_id        // inventory slot that owns the item
ctx.target_voxel   // voxel position (if triggered from face context menu)
ctx.hud_log        // deque to push HUD messages into
ctx.log("message") // convenience method
```

### Built-in handlers (registered in `init_verb_dispatch()`)
| Handler | Effect |
|---------|--------|
| `verb_examine` | Push item name/weight/volume/condition to HUD log |
| `verb_throw` | Throw item in camera direction (8 m/s) |
| `verb_open` | Open/close container item |
| `verb_close` | Close container item |
| `verb_stun` | Apply Stun to nearest mob in range |
| `verb_show_id` | Show ID card info in HUD |
| `verb_open_pda` | Open PDA interface |

### Game-logic handlers (registered in `main.cpp`)
These lambdas capture simulation state by reference:
- `verb_fire` — fire weapon, consuming ammo from item stack
- `verb_melee` — melee attack via `attack_chain()`
- `verb_pry_floor` — pry a floor tile with crowbar
- `verb_reload` — reload gun from held ammo item
- `deconstruct_wall` — destroy wall, spawn materials
- Custom handlers for each item type

### Registering a new verb handler
```cpp
// In main() after systems are initialized:
verb_dispatch.register_handler("verb_my_action",
    [&world, &entities](const VerbContext& ctx) {
        // ctx.item_def, ctx.actor_pos, ctx.target_voxel, etc.
        // Do whatever the verb needs to do
        ctx.log("You did the thing!");
    });
```

Then in your item's JSON:
```json
"verbs": [
  { "name": "My Action", "range": 2.0, "handler": "verb_my_action" }
]
```

---

## 15. Known Gaps & Unfinished Features

### Incomplete Systems

| System | Status |
|--------|--------|
| **Chemistry reactions** | `ReagentDef` + `ReagentContainer` fully defined; `reaction_fire()` is a skeleton — reactions in `data/reactions/chemistry.json` are NOT processed |
| **Power grid gameplay** | Infrastructure exists; no machinery actually consumes power or reacts to APCs |
| **Pipe network reactions** | `pipes.cpp::react()` is a stub |
| **Game modes** | Only `extended` defined; Traitor, Nuclear Emergency, Blob, Cult are design-doc-only |
| **Round flow** | Lobby → Round Start → Round Running → Round End phases defined in DESIGN.md but not implemented |
| **Most verb handlers** | `verb_wear`, `verb_remove`, `verb_inject`, `verb_defib`, `verb_bandage`, `verb_fill_beaker`, etc. are either stubs or missing |
| **Item degradation** | `integrity` tracked per ItemStack; no degradation system decrements it |
| **Drag-and-drop from world** | Items in hand only; cannot drag items from world directly into inventory |
| **Item animations** | No pickup/drop/use animations |
| **Sound effects for items** | `AudioManager` + `sounds.json` infrastructure exists; item interactions do not play sounds |
| **Lock code system** | `secure_briefcase` in data but no lock/unlock mechanic |
| **Custom item name persistence** | `custom_name` field not saved in map v2 |
| **NPC pathfinding** | NPC AI wanders to a random point; no A* or navmesh — NPCs can get stuck |
| **Lua scripting** | DESIGN.md mentions Lua for AI behaviour trees; not implemented |
| **Networked atmos/power/pipe** | These are server-side only; no delta packets sent to clients |
| **Client-side prediction** | Input snapshots have `tick_id` for rewind; prediction code is not implemented |
| **Blob/growing organism game mode** | Architecture supports dynamic voxel placement; game mode logic absent |
| **w-level transitions** | DESIGN.md describes teleporter voxels between vertical dimensinal bands; not implemented |
| **Grabbed mob dragging** | `GrabbedComponent` + `GrabState` enum exist; drag-along physics not implemented |
| **Medical treatment gameplay** | Defibrillator, syringe injection, gauze application are all data-defined but handler stubs |
| **HUD Blind/Dizzy status effects** | `Blind`/`Dizzy` status effects fire signals but renderer doesn't yet apply vision blackout or screen tilt |

### Potential Bugs

1. `Container num_slots = ceil(volume / 3.0)` — may produce too many sparse slots for small containers.
2. Two-handed enforcement only checks hand slots — another equipment slot could hold a two-handed item alongside it.
3. `WorldItemComponent` floating items use `rand()` for scatter, not a seeded PRNG — not thread-safe if physics runs async.
4. `Inventory::put()` volume check doesn't account for nested container parent's shared pool.
5. Container children (sub-slots) cannot themselves be opened — no recursive container support.
6. No validation that item icon files exist before attempting to load GPU textures.
7. `AtmosZone::cell_count` is always 1 but the field is still checked in some places (leftover from an earlier group-zone model).

---

## 16. How to Add or Change Things

### Add a new voxel type

1. Open `data/voxel_types/base.json`.
2. Add a new entry:
   ```json
   {
     "id": "my_voxel",
     "parent": "wall",        // inherit from an existing type (optional)
     "name": "My Voxel",
     "icon": "tiles/my_voxel",
     "flags": ["SOLID", "OPAQUE"],
     "health": 250,
     "material": "steel"
   }
   ```
3. Add the texture PNG at `textures/tiles/my_voxel.png` (32×32 RGBA).
4. Optionally register an `on_hit` verb handler in `main.cpp`.
5. Rebuild — `VoxelRegistry::load_directory()` loads all entries automatically.

**Voxel flags reference:**
- `SOLID` — blocks movement
- `OPAQUE` — blocks light
- `PASSABLE` — marks explicitly passable (overrides SOLID for gas)
- `CLIMBABLE` — entities can vault over
- `LIGHT_SRC` — emits light (use `emit_light`, `emit_r/g/b`)
- `FLAT_PLANE` — thin plane at Y=0 of cell (e.g. floor decal)
- `FLAT_TOP` — thin plane at Y=1 of cell (solid floor like catwalk)
- `VERT_PLANE_Z` — double-sided vertical plane facing Z (doors, windows)
- `GAS_PASSABLE` — gas flows through even when SOLID (open door)
- `BITMASK_FLAT` — uses bitmask connectivity sprites (wires, pipes)

---

### Add a new item

1. Open (or create) the appropriate file in `data/item_types/`.
2. Add an entry. Minimum fields: `id`, `name`, `icon`, `tags`, `weight`, `volume`, `verbs`.
3. Add the texture PNG at `textures/items/my_item.png` (32×32 RGBA).
4. If the item needs a custom verb, register it in `main.cpp` (see Section 14).
5. That's it — the item can now be spawned in the map editor or via `verb_dispatch`.

**Tag conventions:**
- Item category: `tool`, `weapon`, `medical`, `ammo`, `clothing`, `hat`, `helmet`, etc.
- Slot filtering: `small` (fits in pockets), `bag`, `tank`, `suit`, `holster`, etc.
- Weapon stats: `force:20` (melee damage), `stam:40` (stam damage), `sharp`
- Gun stats: `gun`, `ballistic`, `mag_size:8`, `proj_dmg:20`, `proj_type:bullet`, `proj_speed:35`, `ammo_type:pistol_mag`
- Energy gun: `energy_max:100`

---

### Add a new mob species

1. Create `data/mob_species/my_species.json`:
   ```json
   {
     "species": "my_species",
     "name": "My Species",
     "health_max": 80,
     "move_speed": 3.0,
     "sprint_mult": 1.5,
     "height": 1.6,
     "radius": 0.35,
     "damage_types": ["brute", "burn", "tox", "oxy"],
     "slots": [
       { "id": "l_hand", "accepts": ["*"] },
       { "id": "r_hand", "accepts": ["*"] }
     ]
   }
   ```
2. Add mob sprites at `textures/mobs/my_species/default/front.png`, `back.png`, `left.png`, `right.png`.
3. `MobSpeciesRegistry::load_directory()` picks it up automatically.
4. Spawn with `server.spawn_npc("my_species", pos)`.

---

### Add a new simulation system

1. Create `src/simulation/my_system.h` + `my_system.cpp`.
2. Add a component struct (if needed):
   ```cpp
   struct MyComponent { float some_value = 0.f; };
   ```
3. Add a system class:
   ```cpp
   class MySystem {
   public:
       MySystem(World& world, EntityManager& entities);
       void tick(double dt);
   private:
       World& m_world;
       EntityManager& m_entities;
   };
   ```
4. Add the `.cpp` file to `set(SRC_SIM ...)` in `CMakeLists.txt`.
5. Instantiate the system in `server.cpp` (or `main.cpp` for client-only) and call `tick(dt)` each frame.
6. Optionally register entities in the `MasterController`:
   ```cpp
   START_PROCESSING("objects", entity_id,
       [&my_system](EntityID id, double dt) { my_system.tick_entity(id, dt); });
   ```

---

### Add a new UI screen

1. Create `src/ui/my_screen.h` + `src/ui/my_screen.cpp`.
2. Give it a `UIRenderer&` reference and a `draw(cursor, lmb)` method returning a result struct.
3. Add the `.cpp` to `set(SRC_UI ...)` in `CMakeLists.txt`.
4. In `main.cpp`:
   - Declare `bool show_my_screen = false;`
   - Toggle it on some key press
   - While active: `auto r = my_screen.draw(cursor_pos, lmb_pressed);`
   - Handle the result struct.

---

### Add a new game mode

1. Create `data/game_modes/my_mode.json`:
   ```json
   {
     "id": "my_mode",
     "name": "My Mode",
     "description": "Description here.",
     "min_players": 2,
     "antagonist_chance": 0.3,
     "objectives": ["objective_a", "objective_b"]
   }
   ```
2. Load the game mode in `server.cpp` via the `DataValidator` path.
3. Implement round-start logic (role assignment, objective distribution) in `server.cpp` inside the `RoundState` handling section.

Note: The round flow state machine (Lobby → Running → End) is **not yet implemented** in code. This is one of the bigger missing pieces.

---

### Add a new verb handler

```cpp
// In main() after VerbDispatch is initialized and simulation pointers are valid:
verb_dispatch().register_handler("verb_my_handler",
    [&world, &atmos, &entities, &player_inv](const VerbContext& ctx) {
        if (!ctx.item_def) return;

        // Check range
        // ... (use ctx.actor_pos and ctx.target_voxel or ctx.target_ent)

        // Do the action
        ctx.log("You use the " + ctx.item_def->name + "!");
        
        // Example: modify a voxel
        if (ctx.has_target_voxel) {
            world.set_voxel(ctx.target_voxel, Voxel{0}); // set to air
            atmos.on_voxel_changed(ctx.target_voxel);
        }
    });
```

---

### Add a new chemistry reagent

1. Register the definition at startup (in `server.cpp` or a dedicated reagents init function):
   ```cpp
   ReagentRegistry::get().register_def(ReagentDef{
       .id               = "my_drug",
       .name             = "My Drug",
       .metabolise_rate  = 0.5f,
       .overdose_threshold = 30.f,
       .heal_brute       = 2.f,    // heals 2 brute per tick per unit present
       .stun_time        = 0.1f,   // applies 0.1s stun per tick
   });
   ```
2. Optionally add a reaction in `data/reactions/chemistry.json` so it appears when the right reagents mix.
3. **Note:** Reaction processing is currently a stub. Step 2 won't function until `pipes.cpp::react()` and `reagents.cpp::reaction_fire()` are implemented.

---

### Modify player stats

Edit `data/mob_species/human.json`:
- `health_max` — total HP
- `move_speed` — base walk speed (m/s)
- `sprint_mult` — speed multiplier while sprinting
- `height` / `radius` — character controller capsule size
- `slots` — add or remove equipment slots

---

### Working with the map editor

Press **F7** in-game to open the map editor.
- Select a voxel type from the left palette.
- Click/drag on the 2-D grid to paint.
- Use **Page Up/Down** to change the Y-level (vertical slice).
- **Ctrl+S** saves to `maps/current.json` which loads on next startup.
- Switch to the **Objects** tab to place 3-D model objects.
- Switch to the **Items** / **Mobs** tabs to place world items or NPC spawns.

---

*This document was generated from a full source analysis of the codebase as of 2026-05-06.*
