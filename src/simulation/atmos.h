#pragma once
#include "core/world.h"
#include "core/entity_manager.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

// Forward declaration — avoids circular includes with model_objects.h
class ModelObjectManager;

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
    float moles() const { return total_pressure(); }
};

// ── Atmosphere status bitmask ─────────────────────────────────────────────────
enum AtmosStatus : uint8_t {
    ATMOS_OK        = 0,
    ATMOS_LOW_O2    = 1 << 0,  // O2 < 16 kPa
    ATMOS_LOW_PRESS = 1 << 1,  // total < 50 kPa
    ATMOS_HIGH_CO2  = 1 << 2,  // CO2 > 5 kPa
    ATMOS_TOXIC     = 1 << 3,  // plasma/BZ/N2O above trace levels
    ATMOS_FIRE      = 1 << 4,  // active hotspot
    ATMOS_DECOMP    = 1 << 5,  // losing pressure to space
    ATMOS_HIGH_TEMP = 1 << 6,  // temperature > 360 K (dangerously hot)
};

// ── Atmosphere zone ───────────────────────────────────────────────────────────
using AtmosZoneID = uint32_t;
constexpr AtmosZoneID ATMOS_ZONE_NULL  = 0;
constexpr AtmosZoneID ATMOS_ZONE_SPACE = 1;

struct AtmosZone {
    AtmosZoneID id         = ATMOS_ZONE_NULL;
    GasMixture  gas{};
    bool        is_space   = false;
    int         cell_count = 0;
    uint8_t     status     = ATMOS_OK;
    float       pressure_loss_rate = 0.f;   // kPa/s lost to space this tick
    glm::vec3   vent_direction{};           // toward nearest space vent
    std::vector<AtmosZoneID> adjacent_zones;
    bool  has_hotspot  = false;
    float hotspot_temp = 0.f;
};

// ── Door link — which physical door voxels bridge two zones ───────────────────
struct DoorLink {
    AtmosZoneID             zone_a;
    AtmosZoneID             zone_b;
    std::vector<glm::ivec3> door_voxels;
    glm::vec3               midpoint{};   // average world pos (for wind direction)
};

// ── Atmospheric simulator ─────────────────────────────────────────────────────
class AtmosSimulator {
public:
    AtmosSimulator(World& world, EntityManager* entities = nullptr);

    void rebuild_zones();
    void tick(double dt);

    void on_voxel_changed(glm::ivec3 pos);
    void on_door_changed (glm::ivec3 pos);

    AtmosZone*  zone(AtmosZoneID id);
    AtmosZoneID zone_at(glm::ivec3 pos) const;
    GasMixture  mix_at (glm::ivec3 pos) const;
    int         total_rooms() const;

    void try_ignite (AtmosZoneID id);
    void inject_gas (AtmosZoneID id, GasMixture delta);

    // Register a ModelObjectManager so that gas-blocking model objects are
    // treated as solid walls by the atmos zone builder.
    // The pointer must remain valid for the lifetime of this AtmosSimulator.
    void set_model_objects(ModelObjectManager* mgr) { m_model_objects = mgr; }

    // ── Overlay / visualisation accessors ─────────────────────────────────
    // Returns every tracked air cell and the zone it belongs to.
    const std::unordered_map<glm::ivec3, AtmosZoneID>& all_cells() const { return m_cell_zone; }
    // Returns all door-link records (zone boundaries with midpoints).
    const std::vector<DoorLink>& door_links() const { return m_door_links; }
    // Const zone lookup.
    const AtmosZone* zone(AtmosZoneID id) const;

private:
    void process_door_links   (double dt);
    void process_space_drain  (AtmosZone& zone, const DoorLink& lnk,
                               float conductance, double dt);
    void equalise_zones       (AtmosZone& a, AtmosZone& b, double dt,
                               float conductance, glm::vec3 midpoint);
    void partial_rebuild      (std::unordered_set<AtmosZoneID> zone_ids);
    void move_gas_component   (float& pa, float& pb, float Va, float Vb,
                               float conductance, double dt);
    void mix_temperature      (AtmosZone& dst, float added_moles, float src_temp);
    void process_hotspot      (AtmosZone& zone, double dt);
    void apply_entity_effects (double dt);
    void update_status        (AtmosZone& zone);
    AtmosZone* zone_at_id(AtmosZoneID id);

    bool voxel_is_passable   (glm::ivec3 pos) const;
    bool voxel_is_closed_door(glm::ivec3 pos) const;

    World&          m_world;
    EntityManager*  m_entities;
    ModelObjectManager* m_model_objects = nullptr;
    std::unordered_map<AtmosZoneID, AtmosZone>  m_zones;
    std::unordered_map<glm::ivec3, AtmosZoneID> m_cell_zone;
    std::vector<DoorLink>                       m_door_links;
    AtmosZoneID m_next_zone_id = 2;
    bool        m_rebuild_pending = false;

    static constexpr int   SPACE_THRESHOLD          = 8192;
    static constexpr float PRESSURE_THRESHOLD       = 0.25f;
    static constexpr float CONDUCTANCE_OPEN         = 0.6f;
    static constexpr float CONDUCTANCE_CLOSED       = 0.003f;
    static constexpr float CONDUCTANCE_SPACE        = 0.20f;
    static constexpr float CONDUCTANCE_SPACE_SEALED = 0.00f;   // closed door to space = perfectly airtight
    // ── Wind force constants ───────────────────────────────────────────────
    // WIND_THRESHOLD    : minimum pressure_loss_rate (kPa/s) before wind fires.
    //                     ~0.25 kPa/s ≈ 1–2 kPa room differential through an
    //                     open door — enough for a noticeable gentle push.
    // WIND_ACCEL_PER_KPA_S: impulse added per (kPa/s * atmos_dt) in m/s.
    // WIND_GROUND_RESIST: fraction of impulse applied when on_ground; simulates
    //                     the friction advantage of being in contact with the floor.
    // WIND_VEL_CAP       : absolute maximum m/s wind can push an entity.
    static constexpr float WIND_THRESHOLD        = 0.25f;
    static constexpr float WIND_ACCEL_PER_KPA_S  = 1.2f;
    static constexpr float WIND_GROUND_RESIST    = 0.35f;
    static constexpr float WIND_VEL_CAP          = 12.f;
    static constexpr float O2_CONSUMPTION_RATE      = 0.018f;
    static constexpr float CO2_PRODUCTION_RATE      = 0.014f;
    static constexpr float IGNITION_TEMPERATURE     = 360.f;
    static constexpr float PLASMA_FIRE_O2_MIN       = 16.f;
};
