#pragma once
#include "core/world.h"
#include "data/voxel_registry.h"
#include <string>

// Forward declarations for the extended full-world save/load.
class EntityManager;
class WorldItemSystem;
class ItemRegistry;
class MobSpeciesRegistry;
class ModelObjectManager;

// ─────────────────────────────────────────────────────────────────────────────
// Map I/O — lightweight JSON serialisation for the voxel world.
//
// File format version 1 (maps/current.json):
//   { "version": 1, "voxels": [ {"x":0,"y":0,"z":0,"type":"floor"}, … ] }
//
// File format version 2 adds three optional arrays:
//   "world_items" : [ {"x":f,"y":f,"z":f,"item":"id","count":n,"integrity":f} ]
//   "mobs"        : [ {"x":f,"y":f,"z":f,"yaw":f,"species":"s","variant":"v"} ]
//   "model_objects": [ {"name":"s","cx":i,"cy":i,"cz":i,"yaw":f,"scale":f,
//                       "blocks_mobs":b,"blocks_gas":b} ]
// ─────────────────────────────────────────────────────────────────────────────

// ── Voxel-only (v1) ──────────────────────────────────────────────────────────

// Save every non-air voxel in the world to a JSON file.
bool map_save(const World& world, const VoxelRegistry& reg, const std::string& path);

// Clear the world then populate it from a JSON map file.
bool map_load(World& world, const VoxelRegistry& reg, const std::string& path);

// ── Full-world (v2) ───────────────────────────────────────────────────────────

// Save voxels + world items + mobs + static model objects (version 2).
bool map_save_full(World& world,
                   const VoxelRegistry&    voxel_reg,
                   EntityManager&          entities,
                   const ItemRegistry&     item_reg,
                   const ModelObjectManager& model_objs,
                   const std::string&      path);

// Clear world + entities + model objects, then load everything from a map file.
// Gracefully falls back to v1 (voxels only) when arrays are absent.
bool map_load_full(World&                  world,
                   const VoxelRegistry&    voxel_reg,
                   EntityManager&          entities,
                   WorldItemSystem&        world_items,
                   const ItemRegistry&     item_reg,
                   const MobSpeciesRegistry& mob_reg,
                   ModelObjectManager&     model_objs,
                   const std::string&      path);
