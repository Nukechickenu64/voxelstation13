#pragma once
#include "core/world.h"
#include "data/voxel_registry.h"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Map I/O — lightweight JSON serialisation for the voxel world.
//
// File format (maps/current.json):
//   {
//     "version": 1,
//     "voxels": [
//       {"x": 0, "y": 0, "z": 0, "type": "floor"},
//       {"x": 1, "y": 0, "z": 0, "type": "wall", "orientation": 1},
//       …
//     ]
//   }
// ─────────────────────────────────────────────────────────────────────────────

// Save every non-air voxel in the world to a JSON file.
// Creates any missing parent directories.  Returns true on success.
bool map_save(const World& world, const VoxelRegistry& reg, const std::string& path);

// Clear the world then populate it from a JSON map file.
// After loading the caller is responsible for rebuilding chunk meshes and atmos.
// Returns true on success.
bool map_load(World& world, const VoxelRegistry& reg, const std::string& path);
