#pragma once
#include "core/world.h"
#include <unordered_map>
#include <vector>

// ── Gas mixture ───────────────────────────────────────────────────────────────
struct GasMixture {
    float o2      = 0.f;   // kPa
    float n2      = 0.f;
    float co2     = 0.f;
    float plasma  = 0.f;
    float n2o     = 0.f;
    float bz      = 0.f;
    float tritium = 0.f;
    float temperature = 293.15f; // Kelvin

    float total_pressure() const {
        return o2 + n2 + co2 + plasma + n2o + bz + tritium;
    }
    float moles() const { return total_pressure(); } // simplified
};

// ── Atmosphere zone ───────────────────────────────────────────────────────────
using AtmosZoneID = uint32_t;
constexpr AtmosZoneID ATMOS_ZONE_NULL  = 0;
constexpr AtmosZoneID ATMOS_ZONE_SPACE = 1; // infinite vacuum

struct AtmosZone {
    AtmosZoneID   id       = ATMOS_ZONE_NULL;
    GasMixture    gas{};
    bool          is_space = false;
    std::vector<VoxelFaceCoord> member_faces;
    std::vector<AtmosZoneID>    adjacent_zones;
    bool          has_hotspot = false;
    float         hotspot_temp= 0.f;
};

// ── Atmospheric simulator (ZAS-inspired) ──────────────────────────────────────
class AtmosSimulator {
public:
    explicit AtmosSimulator(World& world);

    // Full zone rebuild (BFS flood-fill, call after large map changes)
    void rebuild_zones();

    // Called each simulation tick (default 20 Hz)
    void tick(double dt);

    // Voxel changed at pos — merge/split affected zones
    void on_voxel_changed(glm::ivec3 pos);

    AtmosZone*       zone(AtmosZoneID id);
    AtmosZoneID      zone_at(glm::ivec3 pos) const;
    GasMixture       mix_at(glm::ivec3 pos) const;  // convenience

    // Ignite plasma fire at a zone (if conditions met)
    void try_ignite(AtmosZoneID id);

private:
    void           equalise_pressure(AtmosZone& a, AtmosZone& b, double dt);
    void           process_hotspot(AtmosZone& zone, double dt);
    void           apply_wind_force(AtmosZoneID decompressing_zone);
    AtmosZone*     zone_at_id(AtmosZoneID id);

    World& m_world;
    std::unordered_map<AtmosZoneID, AtmosZone> m_zones;
    AtmosZoneID m_next_zone_id = 2; // 0 = null, 1 = space

    static constexpr float PRESSURE_THRESHOLD   = 0.5f;   // kPa
    static constexpr float IGNITION_TEMPERATURE = 360.f;  // K
    static constexpr float PLASMA_FIRE_O2_MIN   = 16.f;   // kPa
};
