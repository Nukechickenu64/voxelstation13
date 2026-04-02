#include "simulation/world_items.h"
#include "simulation/physics.h"   // TransformComponent
#include <glm/gtx/hash.hpp>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>   // rand()

// ── helpers ───────────────────────────────────────────────────────────────────

// Returns the centre of a voxel face in world space.
static glm::vec3 face_centre(glm::ivec3 voxel, FaceDir dir)
{
    glm::vec3 c = glm::vec3(voxel) + glm::vec3(0.5f);
    glm::vec3 n = glm::vec3(face_normal(dir)) * 0.5f;
    return c + n;
}

// Returns the world-space position on a face including a 2D tangent-plane offset.
static glm::vec3 face_offset_pos(glm::ivec3 voxel, FaceDir dir, glm::vec2 off)
{
    glm::vec3 base = face_centre(voxel, dir);
    FaceTangents tb = face_tangents(dir);
    return base + tb.u * off.x + tb.v * off.y;
}

// Pseudo-random float in [-1, 1]
static float rand_11()
{
    return (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)) * 2.f - 1.f;
}

// ── WorldItemSystem ───────────────────────────────────────────────────────────

WorldItemSystem::WorldItemSystem(World& world, EntityManager& entities)
    : m_world(world)
    , m_entities(entities)
{}

/*static*/ glm::vec3 WorldItemSystem::item_world_pos(const WorldItemComponent& wic)
{
    if (wic.is_resting)
        return face_offset_pos(wic.rest_voxel, wic.rest_face, wic.face_offset);
    return {};  // caller should read TransformComponent::pos directly
}

/*static*/ glm::vec4 WorldItemSystem::tint_for_item(const std::string& id)
{
    // Simple deterministic palette based on string hash
    static const glm::vec4 palette[] = {
        {1.0f, 0.65f, 0.2f, 0.92f},  // amber
        {0.4f, 0.9f,  0.5f, 0.92f},  // green
        {0.5f, 0.7f,  1.0f, 0.92f},  // sky blue
        {0.9f, 0.4f,  0.5f, 0.92f},  // rose
        {0.8f, 0.6f,  1.0f, 0.92f},  // lavender
        {1.0f, 0.9f,  0.3f, 0.92f},  // yellow
    };
    static constexpr int N = static_cast<int>(sizeof(palette) / sizeof(palette[0]));
    size_t h = std::hash<std::string>{}(id);
    return palette[h % N];
}

EntityID WorldItemSystem::spawn(glm::ivec3 face_voxel, FaceDir face, ItemStack item,
                               glm::vec2 face_offset)
{
    EntityID id = m_entities.create();

    glm::vec3 pos = face_offset_pos(face_voxel, face, face_offset);

    // TransformComponent so the entity participates in render queries
    auto& tr = m_entities.add_component<TransformComponent>(id);
    tr.pos      = pos;
    tr.prev_pos = pos;
    tr.yaw = tr.pitch = 0.f;

    WorldItemComponent wic;
    wic.item        = std::move(item);
    wic.is_resting  = true;
    wic.rest_face   = face;
    wic.rest_voxel  = face_voxel;
    wic.face_offset = face_offset;
    wic.tint        = wic.item.def ? tint_for_item(wic.item.def->id)
                                   : glm::vec4(1.f, 0.85f, 0.4f, 0.9f);
    m_entities.add_component<WorldItemComponent>(id, std::move(wic));
    return id;
}

EntityID WorldItemSystem::spawn_scattered(glm::ivec3 face_voxel, FaceDir face,
                                           ItemStack item, float scatter_radius)
{
    constexpr float MAX_OFF = 0.38f;
    float r = std::min(scatter_radius, MAX_OFF);
    glm::vec2 off { rand_11() * r, rand_11() * r };
    return spawn(face_voxel, face, std::move(item), off);
}

EntityID WorldItemSystem::spawn_floating(glm::vec3 pos, ItemStack item,
                                         glm::vec3 velocity)
{
    EntityID id = m_entities.create();

    auto& tr = m_entities.add_component<TransformComponent>(id);
    tr.pos = pos; tr.prev_pos = pos;
    tr.yaw = tr.pitch = 0.f;

    // Velocity component so PhysicsSystem applies gravity and collisions
    auto& vel = m_entities.add_component<VelocityComponent>(id);
    vel.linear = velocity;

    WorldItemComponent wic;
    wic.item       = std::move(item);
    wic.is_resting = false;
    wic.tint       = wic.item.def ? tint_for_item(wic.item.def->id)
                                  : glm::vec4(1.f, 0.85f, 0.4f, 0.9f);
    m_entities.add_component<WorldItemComponent>(id, std::move(wic));
    return id;
}

std::optional<ItemStack> WorldItemSystem::pick_up(EntityID id)
{
    auto* wic = m_entities.get_component<WorldItemComponent>(id);
    if (!wic) return std::nullopt;
    ItemStack item = std::move(wic->item);
    m_entities.destroy(id);
    return item;
}

void WorldItemSystem::tick(double /*dt*/)
{
    // Speed threshold below which a floating item is considered at rest (m/s)
    constexpr float SETTLE_SPEED_SQ = 0.005f * 0.005f;
    constexpr float ITEM_RADIUS     = 0.15f;

    m_entities.each<WorldItemComponent>([&](EntityID id, WorldItemComponent& wic) {
        if (wic.is_resting) return;

        auto* vel = m_entities.get_component<VelocityComponent>(id);
        auto* tr  = m_entities.get_component<TransformComponent>(id);
        if (!vel || !tr) return;

        // Only settle when moving slowly
        float speed_sq = vel->linear.x * vel->linear.x
                       + vel->linear.y * vel->linear.y
                       + vel->linear.z * vel->linear.z;
        if (speed_sq > SETTLE_SPEED_SQ) return;

        // Scan the voxels immediately below for a solid floor
        using std::floor;
        glm::vec3 below_min = tr->pos + glm::vec3(-ITEM_RADIUS, -0.15f, -ITEM_RADIUS);
        glm::vec3 below_max = tr->pos + glm::vec3( ITEM_RADIUS,  0.0f,   ITEM_RADIUS);
        glm::ivec3 imin{ (int)floor(below_min.x), (int)floor(below_min.y), (int)floor(below_min.z) };
        glm::ivec3 imax{ (int)floor(below_max.x), (int)floor(below_max.y), (int)floor(below_max.z) };

        bool      found = false;
        glm::ivec3 floor_voxel{};
        for (int z = imin.z; z <= imax.z && !found; ++z)
        for (int y = imin.y; y <= imax.y && !found; ++y)
        for (int x = imin.x; x <= imax.x && !found; ++x) {
            if (m_world.get_voxel({x, y, z}).flags & VFLAG_SOLID) {
                found = true;
                floor_voxel = {x, y, z};
            }
        }
        if (!found) return;

        // Snap to the top face of the floor voxel and mark as resting.
        // Preserve the item's current XZ fractional position within the voxel
        // (clamped to ±0.38) so thrown / falling items land wherever they were,
        // not all piled at the turf centre.
        vel->linear   = {};
        wic.is_resting = true;
        wic.rest_face  = FaceDir::PosY;
        wic.rest_voxel = floor_voxel;

        // Compute face-plane offset from the centre of the floor voxel.
        // face_tangents(PosY).u = X-axis, .v = Z-axis
        glm::vec2 off {
            glm::clamp(tr->pos.x - (floor_voxel.x + 0.5f), -0.38f, 0.38f),
            glm::clamp(tr->pos.z - (floor_voxel.z + 0.5f), -0.38f, 0.38f)
        };
        wic.face_offset = off;

        glm::vec3 settled = face_offset_pos(floor_voxel, FaceDir::PosY, off);
        tr->pos      = settled;
        tr->prev_pos = settled;
    });
}

EntityID WorldItemSystem::ray_cast_items(glm::vec3 origin, glm::vec3 dir,
                                          float max_dist, float voxel_hit_dist,
                                          float& out_dist) const
{
    out_dist = 0.f;
    EntityID best    = NULL_ENTITY;
    float    best_d  = max_dist;

    // Use a sphere radius of 0.28 m for each item
    constexpr float R  = 0.28f;
    constexpr float R2 = R * R;

    // Cache entities snapshot to avoid issues with const iterator
    std::vector<std::pair<EntityID, WorldItemComponent*>> items;
    const_cast<EntityManager&>(m_entities)
        .each<WorldItemComponent>([&](EntityID eid, WorldItemComponent& wic) {
            items.push_back({eid, &wic});
        });

    glm::vec3 d = glm::normalize(dir);

    for (auto& [eid, wic] : items) {
        auto* tr = const_cast<EntityManager&>(m_entities)
                       .get_component<TransformComponent>(eid);
        if (!tr) continue;
        glm::vec3 center = tr->pos;

        // Ray-sphere intersection (closest approach)
        glm::vec3 oc = center - origin;
        float t_ca    = glm::dot(oc, d);
        if (t_ca < 0.f) continue;  // behind camera

        float dist2 = glm::dot(oc, oc) - t_ca * t_ca;
        if (dist2 > R2) continue;

        float t_hc = std::sqrt(std::max(0.f, R2 - dist2));
        float t    = t_ca - t_hc;
        if (t > best_d) continue;
        if (t > voxel_hit_dist + R) continue;  // item behind wall

        best_d = t;
        best   = eid;
    }

    out_dist = best_d;
    return best;
}

std::vector<EntityID> WorldItemSystem::ray_cast_items_all(
    glm::vec3 origin, glm::vec3 dir,
    float max_dist, float voxel_hit_dist) const
{
    constexpr float R  = 0.28f;
    constexpr float R2 = R * R;
    glm::vec3 d = glm::normalize(dir);

    std::vector<std::pair<float, EntityID>> hits;
    const_cast<EntityManager&>(m_entities)
        .each<WorldItemComponent>([&](EntityID eid, WorldItemComponent& wic) {
            (void)wic;
            auto* tr = const_cast<EntityManager&>(m_entities)
                           .get_component<TransformComponent>(eid);
            if (!tr) return;
            glm::vec3 oc = tr->pos - origin;
            float t_ca   = glm::dot(oc, d);
            if (t_ca < 0.f) return;
            float dist2  = glm::dot(oc, oc) - t_ca * t_ca;
            if (dist2 > R2) return;
            float t_hc = std::sqrt(std::max(0.f, R2 - dist2));
            float t    = t_ca - t_hc;
            if (t > max_dist) return;
            if (t > voxel_hit_dist + R) return;
            hits.push_back({t, eid});
        });

    std::sort(hits.begin(), hits.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });

    std::vector<EntityID> result;
    result.reserve(hits.size());
    for (auto& h : hits) result.push_back(h.second);
    return result;
}

EntityID WorldItemSystem::screen_hover(glm::vec2 cursor,
                                        const glm::mat4& mvp,
                                        int fb_w, int fb_h,
                                        float threshold_px) const
{
    EntityID best       = NULL_ENTITY;
    float    best_dist2 = threshold_px * threshold_px;

    const_cast<EntityManager&>(m_entities)
        .each<WorldItemComponent>([&](EntityID eid, WorldItemComponent& /*wic*/) {
            auto* tr = const_cast<EntityManager&>(m_entities)
                           .get_component<TransformComponent>(eid);
            if (!tr) return;

            glm::vec4 clip = mvp * glm::vec4(tr->pos, 1.f);
            if (clip.w <= 0.f) return;  // behind camera

            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (ndc.z < -1.f || ndc.z > 1.f) return;

            // NDC → screen (SDL y-axis = down, NDC y-axis = up)
            glm::vec2 screen = {
                (ndc.x * 0.5f + 0.5f) * fb_w,
                (1.f - (ndc.y * 0.5f + 0.5f)) * fb_h
            };

            glm::vec2 delta = cursor - screen;
            float d2 = glm::dot(delta, delta);
            if (d2 < best_dist2) {
                best_dist2 = d2;
                best  = eid;
            }
        });

    return best;
}

std::vector<EntityID> WorldItemSystem::screen_hover_all(
    glm::vec2 cursor, const glm::mat4& mvp,
    int fb_w, int fb_h, float threshold_px) const
{
    const float thresh2 = threshold_px * threshold_px;
    std::vector<std::pair<float, EntityID>> hits;

    const_cast<EntityManager&>(m_entities)
        .each<WorldItemComponent>([&](EntityID eid, WorldItemComponent& /*wic*/) {
            auto* tr = const_cast<EntityManager&>(m_entities)
                           .get_component<TransformComponent>(eid);
            if (!tr) return;
            glm::vec4 clip = mvp * glm::vec4(tr->pos, 1.f);
            if (clip.w <= 0.f) return;
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (ndc.z < -1.f || ndc.z > 1.f) return;
            glm::vec2 screen = {
                (ndc.x * 0.5f + 0.5f) * fb_w,
                (1.f - (ndc.y * 0.5f + 0.5f)) * fb_h
            };
            float d2 = glm::dot(cursor - screen, cursor - screen);
            if (d2 > thresh2) return;
            hits.push_back({d2, eid});
        });

    std::sort(hits.begin(), hits.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });

    std::vector<EntityID> result;
    result.reserve(hits.size());
    for (auto& h : hits) result.push_back(h.second);
    return result;
}

std::vector<WorldItemLabel> WorldItemSystem::build_labels(
    const glm::mat4& vp, int fb_w, int fb_h, EntityID hovered) const
{
    std::vector<WorldItemLabel> out;

    const_cast<EntityManager&>(m_entities)
        .each<WorldItemComponent>([&](EntityID eid, WorldItemComponent& wic) {
            auto* tr = const_cast<EntityManager&>(m_entities)
                           .get_component<TransformComponent>(eid);
            if (!tr) return;

            glm::vec4 clip = vp * glm::vec4(tr->pos, 1.f);
            if (clip.w <= 0.f) return;

            bool in_front = clip.w > 0.f;

            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (ndc.x < -1.2f || ndc.x > 1.2f) return;  // off-screen
            if (ndc.y < -1.2f || ndc.y > 1.2f) return;

            glm::vec2 screen = {
                (ndc.x * 0.5f + 0.5f) * fb_w,
                (1.f - (ndc.y * 0.5f + 0.5f)) * fb_h
            };

            WorldItemLabel lbl;
            lbl.screen_pos = screen + glm::vec2(10.f, -24.f);  // offset above item
            lbl.name       = wic.item.def ? wic.item.def->name : "?";
            lbl.item_id    = wic.item.def ? wic.item.def->id   : "";
            lbl.entity_id  = eid;
            lbl.hovered    = (eid == hovered);
            lbl.in_front   = in_front;
            out.push_back(lbl);
        });

    return out;
}
