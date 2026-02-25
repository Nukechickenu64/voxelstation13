#pragma once
#include "core/types.h"
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>
#include <unordered_map>

// Full descriptor for a voxel type, resolved after prototype inheritance.
struct VoxelTypeDef {
    uint16_t             type_id  = 0;
    std::string          id;
    std::string          parent_id;
    std::string          name;
    std::string          icon;
    uint16_t             default_flags = 0;  // VoxelFlag bitmask
    uint32_t             default_tags  = 0;  // FaceTag bitmask
    int                  health        = 100;
    std::string          material;
    std::string          on_hit;       // verb handler name
    std::string          on_walk;      // verb handler name
    uint8_t              emit_light    = 0;  // 0-15
    // Per-face texture overrides (keys match the "icon" format, e.g. "tiles/grass").
    // Empty string means "use icon for that face".
    std::string          tex_top;     // PosY face
    std::string          tex_bottom;  // NegY face (falls back to tex_top if empty)
    std::string          tex_sides;   // PosX/NegX/PosZ/NegZ faces
    // Layer indices into texture atlas per face direction (populated by Renderer)
    std::array<uint16_t, static_cast<int>(FaceDir::COUNT)> atlas_indices{};
};

// Loads all voxel type JSON files, resolves prototype chains, and
// provides runtime lookup by ID or name.
class VoxelRegistry {
public:
    VoxelRegistry() = default;

    bool load_directory(const std::string& path);
    bool load_file(const std::string& path);

    const VoxelTypeDef* get(uint16_t type_id) const;
    const VoxelTypeDef* get(const std::string& id) const;
    bool                has(const std::string& id) const;

    uint16_t id_of(const std::string& name_id) const;

    const std::unordered_map<uint16_t, VoxelTypeDef>& all() const { return m_by_id; }
    std::unordered_map<uint16_t, VoxelTypeDef>&       all_mutable()   { return m_by_id; }

    // Write atlas_indices for a type into both lookup maps.
    void set_atlas_indices(uint16_t type_id,
                           const std::array<uint16_t, static_cast<int>(FaceDir::COUNT)>& indices);

private:
    void resolve_inheritance(VoxelTypeDef& def);

    std::unordered_map<std::string,  VoxelTypeDef> m_by_name;
    std::unordered_map<uint16_t,     VoxelTypeDef> m_by_id;
    uint16_t m_next_id = 1;
};
