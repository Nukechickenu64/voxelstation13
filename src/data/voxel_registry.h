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
    uint8_t              default_flags = 0;  // VoxelFlag bitmask
    uint32_t             default_tags  = 0;  // FaceTag bitmask
    int                  health        = 100;
    std::string          material;
    std::string          on_hit;       // verb handler name
    std::string          on_walk;      // verb handler name
    uint8_t              emit_light    = 0;  // 0-15
    // Layer indices into texture atlas per face direction
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

private:
    void resolve_inheritance(VoxelTypeDef& def);

    std::unordered_map<std::string,  VoxelTypeDef> m_by_name;
    std::unordered_map<uint16_t,     VoxelTypeDef> m_by_id;
    uint16_t m_next_id = 1;
};
