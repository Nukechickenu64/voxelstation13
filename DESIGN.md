# VoxelStation 13 — Game Engine Design Document

**Version:** 0.1  
**Date:** 2026-02-21

---

## Table of Contents

1. [Overview](#overview)
2. [Core Philosophy](#core-philosophy)
3. [World Representation](#world-representation)
   - [Voxel Grid](#voxel-grid)
   - [Face-as-Tile Model](#face-as-tile-model)
   - [Chunk System](#chunk-system)
4. [Rendering](#rendering)
   - [First-Person Camera](#first-person-camera)
   - [Face Rendering Pipeline](#face-rendering-pipeline)
   - [Lighting Model](#lighting-model)
5. [Interaction Model](#interaction-model)
   - [Ray Casting & Face Selection](#ray-casting--face-selection)
   - [Face Interaction Contexts](#face-interaction-contexts)
6. [Input System](#input-system)
   - [Standard Controls](#standard-controls)
   - [Alt-Mode: Cursor Unlock & Inventory Management](#alt-mode-cursor-unlock--inventory-management)
7. [Inventory System](#inventory-system)
   - [Slots & Container Hierarchy](#slots--container-hierarchy)
   - [Item Definitions](#item-definitions)
   - [Drag-and-Drop in Alt-Mode](#drag-and-drop-in-alt-mode)
8. [Tile/Face Data Model](#tileface-data-model)
   - [Face Layers](#face-layers)
   - [Face Metadata & Tags](#face-metadata--tags)
   - [Tile Types and Inheritance](#tile-types-and-inheritance)
9. [Atmosphere & Gas Simulation](#atmosphere--gas-simulation)
10. [Power & Wiring Grid](#power--wiring-grid)
11. [Pipe & Fluid Network](#pipe--fluid-network)
12. [Mob & NPC System](#mob--npc-system)
13. [Game Modes & Round Flow](#game-modes--round-flow)
14. [Networking Architecture](#networking-architecture)
15. [Data-Driven Content Pipeline](#data-driven-content-pipeline)
16. [Audio System](#audio-system)
17. [UI Framework](#ui-framework)
18. [Engine Module Breakdown](#engine-module-breakdown)
19. [File & Directory Layout](#file--directory-layout)
20. [Milestones](#milestones)

---

## Overview

VoxelStation 13 is a first-person multiplayer survival/role-play game engine inspired by the top-down tile-based game Space Station 13 (SS13). The central design shift is moving SS13's **2-D tile grid** into a **3-D voxel grid**, while preserving the rich per-tile metadata, layering, and simulation mechanics that make SS13 compelling. Each exposed face of a voxel behaves conceptually like an SS13 tile: it carries layers (floor, object, wall-decal, overlay), tags, and can participate in atmospheric, electrical, and pipe simulations.

The game is played entirely in first-person. The mouse is exclusively used for looking/aiming in normal mode. Holding **Alt** unlocks the cursor, giving the player a 2-D HUD overlay for inventory management without requiring them to pause or open a separate menu screen.

---

## Core Philosophy

| Principle | Description |
|-----------|-------------|
| **Tile-parity** | Every mechanic available on an SS13 tile (atmos, wiring, layering, density checks) must have a direct voxel-face equivalent. |
| **First-person immersion** | No top-down view. All spatial reasoning happens from the player's perspective in 3-D. |
| **Non-interrupting UI** | The inventory/management UI slides in via Alt-mode without halting gameplay or freezing the world. |
| **Data-driven** | Tile types, items, mobs, recipes, and atmospheric constants are defined in JSON/data files, not hard-coded. |
| **Simulation depth** | Atmospheric gas spread, power grids, and pipe networks all tick on the server at configurable rates. |

---

## World Representation

### Voxel Grid

The world is a 3-D integer grid where each cell is a **Voxel**. A voxel has:

```
struct Voxel {
    uint16_t  type_id;        // index into voxel type registry
    uint8_t   orientation;    // 0-3, 90° rotation steps around Y axis
    uint8_t   flags;          // SOLID, OPAQUE, PASSABLE, CLIMBABLE …
    uint8_t   light_level;    // 0-15, cached from light propagation
    uint8_t   reserved[3];
};
```

An entirely **air/vacuum** voxel uses `type_id == 0` and costs essentially no memory — chunks store only non-zero voxels in a compressed palette encoding.

### Face-as-Tile Model

Each voxel has **6 faces** (±X, ±Y, ±Z). A face is the primary unit of gameplay interaction, mirroring an SS13 tile:

```
struct VoxelFace {
    VoxelFaceCoord  coord;          // voxel position + face direction enum
    LayerStack      layers;         // ordered list of face layers (see below)
    uint32_t        atmos_id;       // owning atmosphere zone ID (0 = none)
    uint8_t         pipe_flags;     // which pipe channels pass through
    uint8_t         wire_flags;     // which wire channels pass through
    uint8_t         decal_id;       // cosmetic decal index (stain, marking…)
    bool            is_visible;     // culled by meshing pass?
};
```

Faces are not stored explicitly for every voxel (that would be 6× the voxel count). Instead they are **derived on access** from the voxel grid: an engine call `get_face(x, y, z, dir)` returns a `VoxelFace` built from the voxel at that cell plus the adjacent voxel, the same way SS13 computes turf properties per tile.

Only faces that require persistent simulation state (atmos zone membership, wiring state) maintain entries in a **sparse face map** keyed by `VoxelFaceCoord`.

### Chunk System

- **Chunk size:** 16 × 16 × 16 voxels (configurable at compile time).
- Chunks are the unit of loading, saving, meshing, and dirty-marking.
- Chunks store voxels in a **palette-compressed array** (up to 256 distinct types per chunk before expanding to 16-bit palette).
- Adjacent chunks communicate face data across boundaries during mesh generation and atmos ticks.
- **Z-levels** from SS13 are modelled as contiguous vertical bands of chunks; inter-z-level transitions are implemented via specific voxel types that act as teleporters.

---

## Rendering

### First-Person Camera

- Standard FPS camera: yaw/pitch controlled by mouse delta when **not** in Alt-mode.
- FOV: default 90°, configurable.
- Bob/sway: optional head-bob tied to movement velocity.
- The held item is rendered in a separate **view-model** pass at a fixed near-plane position, not affected by world transforms.

### Face Rendering Pipeline

1. **Chunk meshing** — runs on worker threads whenever a chunk is dirtied.  
   - Greedy meshing groups co-planar same-material faces into quads.  
   - Each quad carries a `face_uv_index` into the atlas, plus a `layer_bitmask` for overlay compositing.  
2. **Layer compositing** — the fragment shader samples up to 4 texture atlas layers per face (base, overlay1, overlay2, decal) and alpha-blends them in order, reproducing SS13-style icon layering.  
3. **Occlusion culling** — back-face culling + frustum culling per chunk AABB.  
4. **Transparency pass** — translucent faces (glass, windows) rendered in a second depth-sorted pass.

### Lighting Model

- **Block light:** propagates as an integer flood-fill (BFS) up to 15 cells, attenuated by 1 per cell. Each light source voxel has a defined emission level.  
- **Sky light:** top-down column light from the highest unoccluded voxel, simulating station exterior panels or skylights.  
- **Dynamic light:** point lights (fires, consoles, emitters) rendered in a deferred lighting pass with a radius and color.  
- **Smooth lighting:** bi-linear interpolation of per-vertex AO to soften block edges.

---

## Interaction Model

### Ray Casting & Face Selection

Every frame, a ray is cast from the camera origin along the view direction into the voxel grid using a **DDA voxel traversal** (Amanatides & Woo). The traversal reports:

```
struct RayHit {
    glm::ivec3   voxel;          // hit voxel position
    FaceDir      face;           // which face was struck (±X/Y/Z)
    glm::vec3    hit_pos;        // world-space hit point
    float        distance;
};
```

The selected face is highlighted by rendering a thin outline quad over it. Interaction distance is configurable per voxel type (default 2.5 m; machinery can have extended reach).

### Face Interaction Contexts

Pressing **left-click** or **right-click** on a selected face dispatches to the face's **interaction handler**, identical in spirit to SS13's `click` and `alt-click` verbs:

| Action | Behaviour |
|--------|-----------|
| Left-click (empty hand) | Examine / pick up top item layer |
| Left-click (item in hand) | Use item on face |
| Right-click | Context menu (verb list) |
| Alt + left-click (in Alt-mode) | Drag item from world to inventory |
| Scroll wheel | Cycle active hand / hotbar slot |

Verbs are registered per voxel-type and per item-type in JSON data files, allowing content to be added without recompiling.

---

## Input System

### Standard Controls

| Key / Input | Action |
|-------------|--------|
| W A S D | Move |
| Space | Jump |
| Ctrl | Crouch / crawl |
| Shift | Sprint |
| Mouse move | Look (camera yaw/pitch) |
| Left-click | Primary interact |
| Right-click | Secondary interact / context menu |
| Scroll wheel | Cycle active hand slot |
| 1–9 | Hotbar slot select |
| E | Pick up / interact with targeted face |
| F | Throw held item |
| X | Drop held item |
| Tab | Toggle examine mode (shows labels on nearby objects) |
| Alt (hold) | Enter Alt-mode (see below) |
| Esc | Main menu / release cursor |

### Alt-Mode: Cursor Unlock & Inventory Management

Holding **Alt** transitions the engine into **Alt-mode**:

1. **Mouse capture is released** — the OS cursor becomes visible and confined to the window.
2. **Camera freezes** — pitch and yaw stop updating so head movement does not disrupt the UI.
3. **Inventory overlay fades in** — a translucent HUD panel slides in (configurable position: right side by default).
4. The world continues to simulate; other players, mobs, and the environment are unpaused.
5. Releasing **Alt** immediately returns to FPS mode: cursor re-captured, camera unlocked, overlay fades out.

This design avoids a hard "pause" state and preserves the tension of SS13's multitasking gameplay where managing your inventory mid-chaos is part of the skill ceiling.

---

## Inventory System

### Slots & Container Hierarchy

The inventory is a tree of named **slots**. Each slot holds 0 or 1 item stacks. Slots are defined per mob species in data files:

```json
{
  "species": "human",
  "slots": [
    { "id": "head",         "accepts": ["hat", "helmet", "mask"] },
    { "id": "eyes",         "accepts": ["glasses", "goggles"] },
    { "id": "ears",         "accepts": ["headset", "earmuffs"] },
    { "id": "mask",         "accepts": ["mask", "respirator"] },
    { "id": "uniform",      "accepts": ["uniform", "jumpsuit"] },
    { "id": "suit",         "accepts": ["suit", "hardsuit"] },
    { "id": "gloves",       "accepts": ["gloves"] },
    { "id": "boots",        "accepts": ["boots"] },
    { "id": "back",         "accepts": ["bag", "tank"] },
    { "id": "belt",         "accepts": ["belt", "holster"] },
    { "id": "l_hand",       "accepts": ["*"] },
    { "id": "r_hand",       "accepts": ["*"] },
    { "id": "id_card",      "accepts": ["id_card"] },
    { "id": "pda",          "accepts": ["pda"] }
  ]
}
```

Items that are containers (bags, boxes, tool belts) expose their own child slot list when opened, rendered as a nested sub-panel in the overlay.

### Item Definitions

Items are defined in JSON:

```json
{
  "id": "wrench",
  "name": "Wrench",
  "icon": "items/wrench",
  "tags": ["tool", "wrench"],
  "weight": 0.4,
  "stack_max": 1,
  "verbs": [
    { "name": "Wrench",   "range": 1.5, "handler": "verb_wrench"   },
    { "name": "Throw",    "range": 0.0, "handler": "verb_throw"    },
    { "name": "Examine",  "range": 99,  "handler": "verb_examine"  }
  ]
}
```

### Drag-and-Drop in Alt-Mode

In Alt-mode the cursor can:

- **Drag** items between inventory slots (slot-to-slot restack, swap).
- **Drag** items from the world (if the ray-hit face has an object layer) directly into a slot.
- **Drop** items from a slot onto a face in the world view (the world renders beneath the translucent overlay).
- **Right-click** an inventory slot to get a compact verb context menu for that item.
- **Tooltip** appears on hover showing item name, tags, weight, and a short description.

The overlay panel renders using an **immediate-mode UI** sub-system (see UI Framework) so it can be re-laid-out every frame without persistent widget state.

---

## Tile/Face Data Model

### Face Layers

Each face carries an ordered **LayerStack** mirroring SS13's `icon_state` layering:

| Layer index | Name | Purpose |
|-------------|------|---------|
| 0 | `base` | Primary surface texture (wall material, floor tile) |
| 1 | `overlay1` | Wire/pipe pass-through markings |
| 2 | `overlay2` | Damage decals, burns, stains |
| 3 | `object` | Items sitting on/against this face |
| 4 | `effect` | Temporary VFX (wet, sparking, on fire) |

Each layer references an entry in the **texture atlas** by index. An index of 0 means the layer is empty/transparent.

### Face Metadata & Tags

```cpp
enum FaceTag : uint32_t {
    TAG_DENSE       = 1 << 0,   // blocks movement
    TAG_OPAQUE      = 1 << 1,   // blocks light & visibility
    TAG_CLIMBABLE   = 1 << 2,   // can be vaulted over
    TAG_ELECTRIFIED = 1 << 3,   // causes shock on contact
    TAG_SLIPPERY    = 1 << 4,   // banana-peel physics
    TAG_ON_FIRE     = 1 << 5,
    TAG_AIRLOCK     = 1 << 6,
    TAG_WINDOW      = 1 << 7,
    TAG_CATWALK     = 1 << 8,
    // ... extensible via data file
};
```

Tags are used by the physics, atmos, and interaction subsystems to gate behaviours without code coupling.

### Tile Types and Inheritance

Voxel types are defined in JSON with a simple prototype-inheritance chain:

```json
{
  "id": "reinforced_wall",
  "parent": "wall",
  "name": "Reinforced Wall",
  "icon": "tiles/rwall",
  "flags": ["SOLID", "OPAQUE"],
  "health": 600,
  "material": "plasteel",
  "on_hit": "deconstruct_wall"
}
```

The engine resolves the full type descriptor at load time by merging parent fields.

---

## Atmosphere & Gas Simulation

The atmospheric model is a direct 3-D extension of SS13's ZAS (Zone-based Atmospheric Simulator):

- **Zones:** contiguous regions of connected, non-solid faces grouped into a single gas mixture object. Splitting or merging voxels updates zone membership via a BFS flood-fill on a worker thread.
- **Gas mixture:** each zone stores partial pressures (kPa) for: `O2`, `N2`, `CO2`, `Plasma`, `N2O`, `BZ`, `Tritium` (extensible).
- **Pressure equalisation:** each simulation tick, adjacent zones with a pressure differential above a threshold exchange gas proportional to the delta. The tick rate is configurable (default: 20 Hz server-side).
- **Hotspot / fire:** if a zone contains sufficient plasma + O2 and is above ignition temperature, a fire hotspot spawns, consuming gas and producing heat and CO2.
- **Breach detection:** when a wall/window voxel is destroyed, the two bordering zones merge; if one is vacuum (space), a wind force is applied to loose items and mobs within the decompressing zone.
- **Temperature:** zones track temperature (K); heat transfers between adjacent zones and to/from voxels (e.g., cold metal walls absorb heat).

---

## Power & Wiring Grid

- Wires are stored in **face overlay channels** — a face can carry up to 4 wire channels without consuming a full voxel slot, analogous to SS13's cable layers.
- Power is simulated as a graph: **generators** and **APCs** (Area Power Controllers) are nodes; wires are edges.
- Each tick the power graph propagates voltage and current; devices consume power and report their state to their local APC.
- APCs control lighting, machinery, and airlocks in their area. Damaging an APC (or cutting wires) cascades area power outage.

---

## Pipe & Fluid Network

- Pipes occupy **face overlay channels** separate from wires (up to 4 pipe channels per face).
- Pipe networks form directed graphs; pumps and valves are node types with configurable flow rate and direction.
- Fluid packets (water, coolant, reagents) flow through the network each tick.
- The chemistry system allows reagents to mix at junctions, triggering reactions defined in JSON reaction tables.

---

## Mob & NPC System

- A **Mob** is an entity (player or NPC) with an inventory, a stats block, a species, and a list of active status effects.
- Movement uses kinematic character physics: step-up over 1-voxel ledges, slide along walls, crouch under 1-voxel gaps.
- **AI mobs** (rats, maintenance drones, antagonist bots) use a Behaviour Tree evaluated each tick. Behaviour nodes are scriptable in Lua.
- Damage model mirrors SS13's BRUTE / BURN / TOX / OXY split, applied per body part slot.
- **Grabbing:** a mob can grab another mob or item in the world; the grabbed entity is dragged along until released or broken free.

---

## Game Modes & Round Flow

| Phase | Description |
|-------|-------------|
| **Lobby** | Players connect, choose roles, ready up. Server waits for minimum player count or ready-all. |
| **Round start** | Roles assigned, spawn locations generated, antagonist objectives distributed. |
| **Round running** | Continuous simulation. No fixed end condition unless a mode-specific trigger fires. |
| **Round end** | Triggered by vote, objective completion, or admin fiat. Scoreboard + debrief screen. |
| **Post-round** | Short cooldown; players re-enter lobby. Map can optionally reset or persist. |

Example game modes (defined in JSON, not hard-coded):
- **Extended** — no antagonists, pure role-play/build.
- **Traitor** — 1–2 players receive secret objectives; win by completing without being caught.
- **Nuclear Emergency** — antagonist team must detonate a nuke; crew must prevent it.
- **Blob** — one player becomes an expanding voxel-based organism; crew must contain it.

---

## Networking Architecture

- **Authority model:** server-authoritative. Clients send input packets; server simulates and broadcasts state deltas.
- **Transport:** UDP via a reliable-ordered channel abstraction (wrapping an SDL3 networking backend or a library such as enet/GameNetworkingSockets).
- **Entity replication:** entities within a player's PVS (Potentially Visible Set, computed per chunk) are replicated at 20 Hz. Out-of-PVS entities are not sent.
- **Chunk streaming:** clients request chunks as they move; server sends compressed chunk payloads. Dirty chunks are sent as delta patches.
- **Interest management:** the server maintains a per-player subscription list; atmos and power events outside the subscription radius are suppressed.
- **Lag compensation:** hit-scan interactions (ray casts) are resolved server-side with a small rewind buffer (up to 150 ms) to account for network latency.

---

## Data-Driven Content Pipeline

All content is defined outside compiled code:

```
data/
  voxel_types/       *.json   — voxel type definitions
  item_types/        *.json   — item definitions  
  mob_species/       *.json   — mob/species slot layouts and stats
  recipes/           *.json   — crafting and construction recipes
  reactions/         *.json   — chemistry reaction tables
  game_modes/        *.json   — round mode definitions
  maps/              *.json   — pre-built map layouts
  sounds/            *.json   — sound event definitions
textures/
  atlas/             *.png    — texture atlas sheets
  icons/             *.png    — UI icons
  items/             *.png    — item sprites for HUD
```

The engine validates all data files at startup and reports schema errors before entering the main loop.

---

## Audio System

- Spatial audio using SDL3's audio mixer with a distance-attenuation model.
- Sounds are attached to world positions; volume and pan are computed each frame relative to the listener (player camera).
- **Atmosphere propagation:** sounds attenuate faster through low-pressure zones and do not propagate across vacuum.
- **Material occlusion:** ray-cast occlusion check between source and listener; solid voxels apply a low-pass filter approximation.
- Sound events are defined in JSON and triggered programmatically or via data-driven verb handlers.

---

## UI Framework

The HUD is rendered after the 3-D world pass using an **immediate-mode** 2-D layer:

- **Always-on elements:** health bar, suit sensors readout, clock, active hand indicator, hotbar (bottom centre).
- **Alt-mode overlay:** inventory panel, container sub-panels, item tooltips. Rendered as translucent panels that do not block the world view.
- **Context menu:** appears on right-click at cursor position; dismissed on any click outside or Alt release.
- **Chat / radio log:** scrollable text log, lower-left. Input box toggled with `/` or `T`.
- **Examine tooltip:** small floating label over the selected face showing voxel type name and a brief description.

The UI system has no retained widget tree; all layout and draw calls happen in a single `ui_draw()` function called each frame. This keeps UI state trivially serialisable and avoids complex lifecycle bugs.

---

## Engine Module Breakdown

```
src/
  core/
    game_loop.cpp         — fixed-timestep update + variable render loop
    world.cpp             — voxel grid, chunk manager, face accessor
    entity_manager.cpp    — ECS-lite entity/component registry
  render/
    renderer.cpp          — SDL3 GPU init, swap chain, passes
    chunk_mesher.cpp      — greedy mesh generation worker
    lighting.cpp          — block/sky/dynamic light propagation
    ui_renderer.cpp       — immediate-mode 2-D draw calls
  simulation/
    atmos.cpp             — zone BFS, gas mixing, fire
    power.cpp             — wire graph, APC logic
    pipes.cpp             — pipe network, reagent flow
    physics.cpp           — character controller, projectiles
  input/
    input_manager.cpp     — raw SDL3 events → named actions
    alt_mode.cpp          — Alt-mode cursor unlock, ray reproject
  inventory/
    inventory.cpp         — slot tree, stack operations
    item_registry.cpp     — JSON item loader
  network/
    server.cpp            — authoritative simulation, replication
    client.cpp            — prediction, interpolation, chunk streaming
  audio/
    audio_manager.cpp     — spatial audio, occlusion
  scripting/
    lua_vm.cpp            — mob AI behaviour trees (optional Lua)
  data/
    voxel_registry.cpp    — JSON voxel type loader + prototype merge
    data_validator.cpp    — schema validation at startup
  ui/
    hud.cpp               — always-on HUD elements
    inventory_panel.cpp   — Alt-mode inventory overlay
    context_menu.cpp      — right-click verb menu
```

---

## File & Directory Layout

```
voxelstation13/
  DESIGN.md               — this document
  CMakeLists.txt
  includes/               — third-party headers (SDL3, nlohmann/json, …)
  src/                    — engine source (see above)
  data/                   — game content (JSON definitions)
  textures/               — texture atlases and sprites
  sounds/                 — audio assets
  maps/                   — pre-built map files
  scripts/                — Lua AI scripts
  tests/                  — unit and integration tests
  tools/
    map_editor/           — standalone voxel map editor
    atlas_packer/         — texture atlas packing utility
```

---

## Milestones

| Milestone | Goals |
|-----------|-------|
| **M0 — Foundation** | SDL3 window, basic voxel grid, greedy mesher, first-person camera, chunk load/save. |
| **M1 — Interaction** | Ray cast face/item selection, face/item highlight, left/right click interact, item pickup/drop, inventory. |
| **M2 — Alt-Mode UI** | Alt-mode cursor unlock, inventory overlay, item tooltips. |
| **M3 — Simulation** | Atmos zones and gas mixing, power grid, basic fire propagation. |
| **M4 — Networking** | Client/server split, chunk streaming, entity replication, basic lag compensation. |
| **M5 — Content** | Map editor, species definitions, game mode JSON. |
| **M6 — Polish** | Spatial audio with vacuum attenuation, dynamic lighting, smooth lighting AO, chemistry. |
| **M7 — Release Alpha** | Stable dedicated server, public test build, documentation. |

---

*This document is a living specification. Update it as design decisions are revised during development.*
