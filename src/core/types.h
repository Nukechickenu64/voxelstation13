#pragma once
#include <cstdint>
#include <array>
#include <functional>
#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

// ── Face direction ────────────────────────────────────────────────────────────
enum class FaceDir : uint8_t {
    PosX = 0,
    NegX,
    PosY,
    NegY,
    PosZ,
    NegZ,
    COUNT
};

constexpr glm::ivec3 face_normal(FaceDir d) {
    constexpr glm::ivec3 normals[6] = {
        { 1,  0,  0}, {-1,  0,  0},
        { 0,  1,  0}, { 0, -1,  0},
        { 0,  0,  1}, { 0,  0, -1},
    };
    return normals[static_cast<int>(d)];
}

// ── Voxel ─────────────────────────────────────────────────────────────────────
struct Voxel {
    uint16_t type_id     = 0;   // 0 = air/vacuum
    uint8_t  orientation = 0;   // 0-3, 90° rotation steps around Y
    uint8_t  light_level = 0;   // 0-15
    uint16_t flags       = 0;   // see VoxelFlag (uint16 for extended flag space)
    uint8_t  reserved[2]{};
};

enum VoxelFlag : uint16_t {
    VFLAG_SOLID        = 1 << 0,
    VFLAG_OPAQUE       = 1 << 1,
    VFLAG_PASSABLE     = 1 << 2,
    VFLAG_CLIMBABLE    = 1 << 3,
    VFLAG_LIGHT_SRC    = 1 << 4,
    VFLAG_FLAT_PLANE   = 1 << 5,  // thin plane at Y=0 of cell (no physics)
    VFLAG_FLAT_TOP     = 1 << 6,  // thin plane at Y=1 of cell (solid floor)
    VFLAG_VERT_PLANE_Z = 1 << 7,  // double-sided vertical plane facing Z, centred in cell
    VFLAG_GAS_PASSABLE = 1 << 8,  // gas flows through this voxel even when solid (e.g. open door)
};

// ── Face coordinate ───────────────────────────────────────────────────────────
struct VoxelFaceCoord {
    glm::ivec3 pos;
    FaceDir    dir;

    bool operator==(const VoxelFaceCoord& o) const {
        return pos == o.pos && dir == o.dir;
    }
};

struct VoxelFaceCoordHash {
    std::size_t operator()(const VoxelFaceCoord& c) const noexcept {
        // simple hash combine
        std::size_t h = std::hash<int>{}(c.pos.x);
        h ^= std::hash<int>{}(c.pos.y) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(c.pos.z) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(c.dir)) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

// ── Face layer stack (mirrors SS13 icon layering) ─────────────────────────────
constexpr int FACE_LAYER_COUNT = 5;
enum FaceLayer : int {
    LAYER_BASE    = 0,  // primary surface texture
    LAYER_OVERLAY1= 1,  // wire/pipe markings
    LAYER_OVERLAY2= 2,  // damage decals, stains
    LAYER_OBJECT  = 3,  // items resting on face
    LAYER_EFFECT  = 4,  // temporary VFX
};

struct LayerStack {
    std::array<uint16_t, FACE_LAYER_COUNT> atlas_index{};  // 0 = empty
};

// ── Face tags ─────────────────────────────────────────────────────────────────
enum FaceTag : uint32_t {
    TAG_DENSE       = 1 << 0,
    TAG_OPAQUE      = 1 << 1,
    TAG_CLIMBABLE   = 1 << 2,
    TAG_ELECTRIFIED = 1 << 3,
    TAG_SLIPPERY    = 1 << 4,
    TAG_ON_FIRE     = 1 << 5,
    TAG_AIRLOCK     = 1 << 6,
    TAG_WINDOW      = 1 << 7,
    TAG_CATWALK     = 1 << 8,
};

// ── VoxelFace — derived on access, persisted only for simulation state ─────────
struct VoxelFace {
    VoxelFaceCoord  coord;
    LayerStack      layers;
    uint32_t        atmos_id    = 0;
    uint8_t         pipe_flags  = 0;
    uint8_t         wire_flags  = 0;
    uint8_t         decal_id    = 0;
    uint32_t        tags        = 0;
    bool            is_visible  = false;
};

// ── Ray-cast result ───────────────────────────────────────────────────────────
struct RayHit {
    glm::ivec3 voxel{};
    FaceDir    face    = FaceDir::PosX;
    glm::vec3  hit_pos{};
    float      distance= 0.f;
    bool       valid   = false;
};

// ── Entity ID ─────────────────────────────────────────────────────────────────
using EntityID = uint32_t;
constexpr EntityID NULL_ENTITY = 0;
