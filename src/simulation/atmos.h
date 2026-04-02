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
    float o2      = 0.f;
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
    ATMOS_LOW_O2    = 1 << 0,
    ATMOS_LOW_PRESS = 1 << 1,
    ATMOS_HIGH_CO2  = 1 << 2,
    ATMOS_TOXIC     = 1 << 3,
    ATMOS_FIRE      = 1 << 4,
    ATMOS_DECOMP    = 1 << 5,
    ATMOS_HIGH_TEMP = 1 << 6,
};

// ── Per-cell atmosphere zone ──────────────────────────────────────────────────
// In the TG-style per-cell model every enclosed passable voxel is its own zone.
// cell_count is always 1.  adjacent_zones lists the zone IDs of orthogonal
// passable neighbours (used for the debug adj-count display).
using AtmosZoneID = uint32_t;
constexpr AtmosZoneID ATMOS_ZONE_NULL  = 0;
constexpr AtmosZoneID ATMOS_ZONE_SPACE = 1;

struct AtmosZone {
    AtmosZoneID id         = ATMOS_ZONE_NULL;
    GasMixture  gas{};
    bool        is_space   = false;
    int         cell_count = 1;          // always 1; kept for API compat
    uint8_t     status     = ATMOS_OK;
    float       pressure_loss_rate = 0.f;
    glm::vec3   vent_direction{};
    std::vector<AtmosZoneID> adjacent_zones;
    bool        has_hotspot  = false;
    float       hotspot_temp = 0.f;
    glm::ivec3  cell_pos{};              // world position of this cell (cell_count==1)
};

// ── Direct cell-to-cell edge ──────────────────────────────────────────────────
struct CellEdge {
    AtmosZoneID za;
    AtmosZoneID zb;
};

// ── Door link — a face bounded by a door voxel or open space ─────────────────
struct DoorLink {
    AtmosZoneID             zone_a;
    AtmosZoneID             zone_b;
    std::vector<glm::ivec3> door_voxels;
    glm::vec3               midpoint{};
    glm::vec3               eject_dir{};
};

// ── Atmospheric simulator ─────────────────────────────────────────────────────
// TG-station13-inspired per-cell model:
//   * Every enclosed passable voxel stores its own GasMixture.
//   * Gas flows between adjacent cells each tick at CONDUCTANCE_CELL.
//   * Cells adjacent to open space drain via process_space_drain().
//   * Door voxels create DoorLinks that seep at CONDUCTANCE_CLOSED when shut
//     or flow at CONDUCTANCE_CELL when open.
//   * No full BFS on voxel change — only the affected cell+neighbours update.
class AtmosSimulator {
public:
    AtmosSimulator(World& world, EntityManager* entities = nullptr);

    void rebuild_zones();
    void tick(double dt);

    void on_voxel_changed(glm::ivec3 pos);
    void on_door_changed (glm::ivec3 pos);

    AtmosZone*       zone(AtmosZoneID id);
    const AtmosZone* zone(AtmosZoneID id) const;
    AtmosZoneID      zone_at(glm::ivec3 pos) const;
    GasMixture       mix_at (glm::ivec3 pos) const;
    int              total_rooms() const;

    void try_ignite (AtmosZoneID id);
    void inject_gas (AtmosZoneID id, GasMixture delta);

    void set_model_objects(ModelObjectManager* mgr) { m_model_objects = mgr; }

    const std::unordered_map<glm::ivec3, AtmosZoneID>& all_cells() const { return m_cell_zone; }
    const std::vector<DoorLink>& door_links() const { return m_door_links; }

private:
    void flow_cells          (AtmosZone& a, AtmosZone& b, double dt, float conductance);
    void process_space_drain (AtmosZone& zone, const DoorLink& lnk, float conductance, double dt);
    void move_gas_component  (float& pa, float& pb, float Va, float Vb, float conductance, double dt);
    void mix_temperature     (AtmosZone& dst, float added_moles, float src_temp);
    void diffuse_temperature (AtmosZone& a, AtmosZone& b, double dt);
    void process_hotspot     (AtmosZone& zone, double dt);
    void apply_entity_effects(double dt);
    void update_status       (AtmosZone& zone);
    AtmosZone* zone_at_id    (AtmosZoneID id);

    bool check_is_space    (glm::ivec3 pos) const;
    void remove_cell       (glm::ivec3 pos);
    void rebuild_cell_edges(glm::ivec3 pos);

    bool voxel_is_passable   (glm::ivec3 pos) const;
    bool voxel_is_closed_door(glm::ivec3 pos) const;

    World&              m_world;
    EntityManager*      m_entities;
    ModelObjectManager* m_model_objects = nullptr;

    std::unordered_map<AtmosZoneID, AtmosZone>  m_zones;
    std::unordered_map<glm::ivec3, AtmosZoneID> m_cell_zone;
    std::vector<CellEdge>  m_open_edges;
    std::vector<DoorLink>  m_door_links;

    AtmosZoneID m_next_zone_id = 2;
    int         m_num_regions  = 0;

    static constexpr int   SPACE_THRESHOLD          = 8192;
    static constexpr float PRESSURE_THRESHOLD       = 0.25f;
    static constexpr float CONDUCTANCE_CELL         = 3.0f;
    static constexpr float CONDUCTANCE_CLOSED       = 0.003f;
    static constexpr float CONDUCTANCE_SPACE        = 2.0f;
    static constexpr float CONDUCTANCE_SPACE_SEALED = 0.00f;
    // Wind thresholds — kPa pressure differential between adjacent cells (TG-style)
    static constexpr float WIND_EFFECT_THRESHOLD    = 2.0f;   // min diff to start pushing entities
    static constexpr float WIND_SPEED_FACTOR        = 0.08f;  // coefficient: factor * diff^EXP = m/s²
    static constexpr float WIND_POWER_EXP           = 1.7f;   // exponent — makes large diffs exponentially stronger
    static constexpr float WIND_GROUND_RESIST       = 0.20f;  // on-ground friction multiplier
    static constexpr float WIND_VEL_CAP             = 20.f;   // max wind-induced speed (m/s)
    static constexpr float WIND_JITTER_THRESHOLD    = 2.0f;   // kPa diff → Jitter
    static constexpr float WIND_DIZZY_THRESHOLD     = 15.0f;  // kPa diff → Dizzy
    static constexpr float WIND_KNOCKDOWN_THRESHOLD = 30.0f;  // kPa diff → Knockdown / airborne
    static constexpr float WIND_KNOCKDOWN_DUR_BASE  = 1.0f;
    static constexpr float WIND_KNOCKDOWN_DUR_MAX   = 4.0f;
    static constexpr float O2_CONSUMPTION_RATE      = 0.018f;
    static constexpr float CO2_PRODUCTION_RATE      = 0.014f;
    static constexpr float IGNITION_TEMPERATURE     = 360.f;
    static constexpr float PLASMA_FIRE_O2_MIN       = 16.f;

    // Temperature diffusion (independent of pressure)
    static constexpr float TEMP_DIFFUSION_RATE      = 0.08f;

    // Fire spreading thresholds
    static constexpr float FIRE_SPREAD_TEMP_THRESH  = 410.f;  // hotspot temp to spread
    static constexpr float FIRE_SPREAD_PLASMA_MIN   = 8.f;    // min plasma in neighbor

    // Tritium fire
    static constexpr float TRITIUM_FIRE_O2_MIN      = 2.f;
    static constexpr float TRITIUM_BURN_HEAT        = 80.f;   // heat per unit burned

    // Atmospheric damage rates (per second)
    static constexpr float LOW_O2_THRESHOLD         = 16.f;   // kPa
    static constexpr float LOW_PRESSURE_THRESHOLD   = 50.f;   // kPa total
    static constexpr float HIGH_CO2_THRESHOLD       = 5.f;    // kPa
    static constexpr float HIGH_TEMP_THRESHOLD      = 360.f;  // K
    static constexpr float OXY_DAMAGE_RATE          = 0.5f;   // damage/s at full severity
    static constexpr float DECOMP_DAMAGE_RATE       = 0.8f;   // damage/s at full severity
    static constexpr float CO2_TOX_RATE             = 0.25f;
    static constexpr float PLASMA_TOX_RATE          = 1.5f;
    static constexpr float TRITIUM_TOX_RATE         = 0.8f;
    static constexpr float BZ_TOX_RATE              = 0.5f;
    static constexpr float N2O_TOX_RATE             = 0.1f;
    static constexpr float HEAT_BURN_RATE           = 0.002f; // damage/s per excess K

    // N2O thresholds for status effects
    static constexpr float N2O_DROWSY_THRESHOLD     = 5.f;    // kPa
    static constexpr float N2O_CONFUSION_THRESHOLD  = 25.f;   // kPa
    static constexpr float SPACE_OXY_DAMAGE_RATE    = 4.0f;  // open vacuum is instant death

    // Oxy healing — breathing fresh air clears oxy damage gradually
    static constexpr float OXY_HEAL_RATE             = 0.5f;
    static constexpr float OXY_HEAL_O2_MIN           = 16.f;
    static constexpr float OXY_HEAL_PRESSURE_MIN     = 50.f;

    // High-pressure barotrauma (> 550 kPa total environment pressure)
    static constexpr float BARO_PRESSURE_THRESHOLD   = 550.f;
    static constexpr float BARO_BASE_RATE            = 0.5f;  // brute/s at threshold
    static constexpr float BARO_EXCESS_SCALE         = 0.3f;  // brute/s per 100 kPa over
    static constexpr float BARO_MAX_RATE             = 3.0f;  // cap
    // Low pressure (1–10 kPa, not full vacuum): decompression sickness
    static constexpr float DECOMP_BARO_MAX_P         = 10.f;  // kPa — threshold
    static constexpr float DECOMP_BARO_RATE          = 0.25f; // brute/s at 1 kPa
};