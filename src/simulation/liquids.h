#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  Liquid / puddle physics — inspired by Monkestation2.0 SSliquids.
//
//  Design overview
//  ───────────────
//  • A "liquid cell" is an air voxel that has reagent pooled on its floor.
//    A cell can hold liquid only when the voxel directly below it is solid
//    (the solid voxel acts as a container floor).
//
//  • All spatially connected liquid cells belong to a single LiquidGroup that
//    owns a shared reagent pool.  Volume is divided equally across members;
//    this "volume per cell" value drives visual height and spreading rules.
//
//  • Each tick (2 Hz, LIQUID_TICK_DT):
//      1. Fall  — if the voxel below a cell is passable, reagents drop there.
//      2. Spread — edge cells try to expand into neighbouring floor cells
//                 once volume per cell ≥ LIQUID_SPREAD_THRESHOLD.
//      3. Merge — when an edge cell touches another group, both groups unite.
//      4. Evaporate — small volumes (< 1 u/cell) vanish over time.
//      5. Expose — entities standing in a wet cell receive a reagent touch.
//
//  TG/Monkestation analogue
//  ─────────────────────────
//    datum/liquid_group     → LiquidGroup
//    SSliquids              → LiquidSimulator::tick()
//    turf.liquids           → LiquidSimulator::group_id_at(pos)
//    liquid_group.reagents  → LiquidGroup::reagents
//    spread_liquid()        → LiquidSimulator::try_spread_cell()
//    merge_groups()         → LiquidSimulator::merge_groups()
// ─────────────────────────────────────────────────────────────────────────────

#include "core/world.h"
#include "core/entity_manager.h"
#include "simulation/reagents.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <string_view>

// ── Numeric constants (mirrors code/__DEFINES/liquids.dm) ────────────────────

/// Minimum reagent units per cell required before liquid expands laterally.
constexpr float LIQUID_SPREAD_THRESHOLD  = 10.0f;

/// Base evaporation rate (units/second per cell).  Very small puddles
/// (< 1 u/cell after LIQUID_STAGNANT_TICKS ticks) are removed outright.
constexpr float LIQUID_EVAP_RATE_BASE    =  0.05f;

/// Soft cap on reagent volume in a single group before further addition
/// overflows to adjacent cells (enforced lazily via spreading, not hard-clip).
constexpr float LIQUID_MAX_VOLUME_PER_CELL = 200.0f;

/// Puddles with less than this volume per cell evaporate in one tick.
constexpr float LIQUID_EVAP_THRESHOLD    =  1.0f;

/// Fixed sub-step duration (0.5 s → 2 Hz, same as Monkestation SSliquids).
constexpr double LIQUID_TICK_DT          =  0.5;

/// Max cells one BFS scan will traverse when checking group connectivity
/// after a cell is removed (prevents O(map) stalls on large lakes).
constexpr int LIQUID_SPLIT_BFS_LIMIT     = 512;

// ── Visual state thresholds ───────────────────────────────────────────────────
enum class LiquidState : uint8_t {
    Puddle    = 0,  //  < 10  u/cell — small stain
    Ankles    = 1,  //  10–49 u/cell — ankle-deep
    Waist     = 2,  //  50–99 u/cell — waist-deep
    Shoulders = 3,  // 100–149 u/cell — shoulder-deep
    FullTile  = 4,  // 150+   u/cell — full tile
};

// ── Group ID ──────────────────────────────────────────────────────────────────
using LiquidGroupID = uint32_t;
constexpr LiquidGroupID LIQUID_GROUP_NULL = 0;

// ── LiquidGroup ───────────────────────────────────────────────────────────────
/// All spatially contiguous liquid cells that share a single reagent pool.
struct LiquidGroup {
    LiquidGroupID id = LIQUID_GROUP_NULL;

    /// Every cell (world voxel position) occupied by this group.
    std::unordered_set<glm::ivec3> members;

    /// All reagents pooled across every member cell (volume = total for group).
    ReagentContainer reagents;

    /// Per-boundary-cell spread candidates:
    ///   edge_cache[cell] = list of cardinal step vectors still worth trying.
    /// A direction is removed once liquid flows that way (or it's proven blocked).
    std::unordered_map<glm::ivec3, std::vector<glm::ivec3>> edge_cache;

    /// True if this group is scheduled for deletion at end-of-tick.
    bool marked_for_delete = false;

    // ── Derived helpers ───────────────────────────────────────────────────────
    float volume_per_cell() const {
        if (members.empty()) return 0.f;
        return reagents.total_volume() / static_cast<float>(members.size());
    }

    LiquidState visual_state() const {
        const float v = volume_per_cell();
        if (v < 10.f)  return LiquidState::Puddle;
        if (v < 50.f)  return LiquidState::Ankles;
        if (v < 100.f) return LiquidState::Waist;
        if (v < 150.f) return LiquidState::Shoulders;
        return LiquidState::FullTile;
    }
};

// ── LiquidSimulator ───────────────────────────────────────────────────────────
class LiquidSimulator {
public:
    LiquidSimulator(World& world, EntityManager* entities = nullptr);

    // ── Main tick — call from Server::tick() ─────────────────────────────────
    /// Accumulates elapsed time and fires sub-steps at LIQUID_TICK_DT intervals.
    void tick(double dt);

    // ── World-event hooks ─────────────────────────────────────────────────────
    /// Call when a voxel changes (wall broken, door opened, etc.) so that cells
    /// that can no longer hold liquid are evicted from their groups.
    void on_voxel_changed(glm::ivec3 pos);

    // ── Liquid injection API ──────────────────────────────────────────────────
    /// Add `amount` units of reagent `id` to the cell at `pos`.
    /// Creates a new group or joins the existing one.
    void add_liquid(glm::ivec3 pos, std::string_view reagent_id, float amount);

    /// Transfer up to `amount` total units (proportional split) from `src`
    /// into the liquid pool at `pos`.
    void add_liquid_from_container(glm::ivec3 pos,
                                   ReagentContainer& src, float amount);

    // ── Query API ─────────────────────────────────────────────────────────────
    float          volume_at      (glm::ivec3 pos) const;
    LiquidGroupID  group_id_at    (glm::ivec3 pos) const;
    LiquidGroup*   group_at       (glm::ivec3 pos);
    const LiquidGroup* group_at   (glm::ivec3 pos) const;

    /// Iterate every known liquid cell (for rendering / HUD overlays).
    const std::unordered_map<glm::ivec3, LiquidGroupID>& all_cells() const {
        return m_cell_group;
    }

private:
    // ── Internal helpers ──────────────────────────────────────────────────────

    /// True if the voxel at `pos` is passable (air / gas-passable).
    bool voxel_passable(glm::ivec3 pos) const;

    /// True if `pos` has a solid floor directly beneath it
    /// (voxel at pos + DOWN is solid, or pos.y == 0).
    bool has_floor(glm::ivec3 pos) const;

    /// True if `pos` can hold liquid (passable cell with a solid floor).
    bool cell_can_hold_liquid(glm::ivec3 pos) const;

    // ── Group lifecycle ───────────────────────────────────────────────────────
    LiquidGroup& create_group();
    void         add_cell_to_group (LiquidGroup& grp, glm::ivec3 pos);
    void         remove_cell_from_group (glm::ivec3 pos);

    /// Merge group `src_id` into `dst_id`; src is deleted.
    void merge_groups(LiquidGroupID dst_id, LiquidGroupID src_id);

    /// After one or more cells are evicted from a group, check if the
    /// remaining members are still contiguous.  If not, split into new groups.
    void try_split(LiquidGroup& grp, const std::vector<glm::ivec3>& removed);

    // ── Per-tick processing ───────────────────────────────────────────────────
    void process_tick(double dt);

    void process_fall        (LiquidGroup& grp);
    void process_spread      (LiquidGroup& grp);
    void process_evaporation (LiquidGroup& grp, double dt);
    void expose_entities     (LiquidGroup& grp, double dt);

    void add_edges_for_cell  (LiquidGroup& grp, glm::ivec3 cell);
    bool try_spread_cell     (LiquidGroup& grp, glm::ivec3 from,
                              glm::ivec3 direction);

    void cleanup_empty_groups();

    // ── Data ──────────────────────────────────────────────────────────────────
    World&          m_world;
    EntityManager*  m_entities;

    std::unordered_map<LiquidGroupID, std::unique_ptr<LiquidGroup>> m_groups;
    std::unordered_map<glm::ivec3, LiquidGroupID>                   m_cell_group;
    LiquidGroupID m_next_group_id = 1;
    double        m_accumulator   = 0.0;

    // Cardinal horizontal directions for lateral spreading.
    static constexpr glm::ivec3 k_horiz[4] = {
        { 1, 0,  0}, {-1, 0,  0},
        { 0, 0,  1}, { 0, 0, -1},
    };
    // Down direction (Y is up in this engine).
    static constexpr glm::ivec3 k_down = {0, -1, 0};
};
