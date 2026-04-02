#pragma once
#include "core/types.h"
#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>

// ── MobComponent ──────────────────────────────────────────────────────────────
// Attached to any entity rendered as a billboard mob (NPC, player dummy, etc.).
//
// Doom-style sprite rotation: 4 directions — front, back, left, right.
// The renderer selects the sprite based on the horizontal angle from the mob's
// facing direction (stored in TransformComponent::yaw) to the camera.
//
//   Front  — camera is in the direction the mob faces (sees its face)
//   Back   — camera is behind the mob
//   Left   — camera is to the mob's left
//   Right  — camera is to the mob's right
//
// Textures are loaded from:
//   textures/mobs/<species>/<variant>/(front|back|left|right).png
// or any file whose name contains the direction word (e.g. "hffront.png").

struct MobComponent {
    std::string species = "human";   // sub-directory under textures/mobs/
    std::string variant = "female";  // sub-sub-directory
};

// ── HealthComponent ───────────────────────────────────────────────────────────
// TG SS13-faithful damage tracking for mobs.
//
// Five independent damage buckets:
//   brute — physical trauma (punching, explosions)
//   burn  — heat / electrical / laser damage
//   tox   — toxin accumulation (plasma gas, drugs, poison)
//   oxy   — oxygen deprivation / suffocation
//   clone — radiation / mutagenic damage (persists through cloning)
//
// health = health_max – (brute + burn + tox + oxy + clone)
// This can go NEGATIVE (like TG: health ranges from +maxHealth to −maxHealth).
//
// TG thresholds (from /code/modules/mob/living/living.dm):
//   HEALTH_THRESHOLD_CRIT     = 50  → softcrit when health ≤ 50 (out of 100)
//   HEALTH_THRESHOLD_死       = 0   → dead when health ≤ 0
//
// Stamina is tracked separately from HP:
//   stam_damage accumulates from stun-batons, flashes, etc.
//   At stam_damage ≥ stamina_max the mob is Knocked Out (stam_ko).
//   Stamina regenerates at ~3/s when not fully depleted (server tick).
//   TG proc: /mob/living/proc/apply_stamina_loss(), stamRegenRate.

struct HealthComponent {
    // ── TG-parity crit thresholds ─────────────────────────────────────────────
    // HEALTH_THRESHOLD_CRIT in TG = 50  (softcrit below this value)
    // HEALTH_THRESHOLD_死  in TG = 0   (dead at or below 0)
    static constexpr float CRIT_THRESHOLD       = 50.f;
    static constexpr float STAMINA_KO_THRESHOLD = 100.f;

    float health_max  = 100.f;

    // ── Damage buckets ────────────────────────────────────────────────────────
    float brute = 0.f;   // physical / melee / explosion
    float burn  = 0.f;   // heat / electrical / laser
    float tox   = 0.f;   // toxin / plasma gas / poison
    float oxy   = 0.f;   // oxygen deprivation / asphyxia
    float clone = 0.f;   // radiation / mutagenic (TG: clone_damage)

    // ── Stamina (separate KO track) ───────────────────────────────────────────
    float stamina_max = 100.f;
    float stam_damage = 0.f;   // 0 = rested; stamina_max = KO'd

    // ── State flags ───────────────────────────────────────────────────────────
    bool  dead    = false;   // current() <= 0
    bool  crit    = false;   // 0 < current() <= CRIT_THRESHOLD (softcrit)
    bool  stam_ko = false;   // stam_damage >= stamina_max
    bool  godmode = false;   // absorbs all incoming damage

    // ── Computed health ───────────────────────────────────────────────────────
    // May be negative when total_damage() > health_max (TG allows this).
    float current() const {
        return health_max - (brute + burn + tox + oxy + clone);
    }

    float total_damage() const { return brute + burn + tox + oxy + clone; }

    // Stamina as 0..1 fraction (1 = fully rested).
    float stamina_pct() const {
        return 1.f - std::clamp(stam_damage / stamina_max, 0.f, 1.f);
    }

    // ── Apply damage ──────────────────────────────────────────────────────────
    // Each bucket is clamped to [0, health_max].
    // Negative amount is forwarded to heal_type() (TG negative-damage heal path).
    void apply(const std::string& type, float amount) {
        if (godmode && amount > 0.f) return;
        if (amount < 0.f) { heal_type(type, -amount); return; }
        auto clamp_add = [&](float& bucket) {
            bucket = std::min(bucket + amount, health_max);
        };
        if      (type == "brute") clamp_add(brute);
        else if (type == "burn")  clamp_add(burn);
        else if (type == "tox")   clamp_add(tox);
        else if (type == "oxy")   clamp_add(oxy);
        else if (type == "clone") clamp_add(clone);
        update_flags();
    }

    // ── Heal a specific damage type ───────────────────────────────────────────
    // TG analogues: heal_bodypart_damage(), adjustBruteLoss(), adjustFireLoss() …
    void heal_type(const std::string& type, float amount) {
        if (amount <= 0.f) return;
        if      (type == "brute") brute = std::max(0.f, brute - amount);
        else if (type == "burn")  burn  = std::max(0.f, burn  - amount);
        else if (type == "tox")   tox   = std::max(0.f, tox   - amount);
        else if (type == "oxy")   oxy   = std::max(0.f, oxy   - amount);
        else if (type == "clone") clone = std::max(0.f, clone - amount);
        update_flags();
    }

    // ── Heal all damage types equally (universal heal / medkits) ─────────────
    void heal(float amount) {
        if (amount <= 0.f) return;
        brute = std::max(0.f, brute - amount);
        burn  = std::max(0.f, burn  - amount);
        tox   = std::max(0.f, tox   - amount);
        oxy   = std::max(0.f, oxy   - amount);
        clone = std::max(0.f, clone - amount);
        update_flags();
    }

    // ── Stamina damage / regen ────────────────────────────────────────────────
    // TG: /mob/living/proc/apply_stamina_loss()
    void apply_stam(float amount) {
        if (godmode) return;
        stam_damage = std::min(stam_damage + amount, stamina_max);
        stam_ko = (stam_damage >= stamina_max);
    }

    // TG: stamRegenRate ≈ 3/s when not fully depleted
    void regen_stam(float amount) {
        stam_damage = std::max(0.f, stam_damage - amount);
        if (stam_damage < stamina_max) stam_ko = false;
    }

    // ── Explicit flag resync ──────────────────────────────────────────────────
    // Call after directly writing damage fields (e.g. dev tools, network sync).
    // Normal game code should go through apply() / heal() / heal_type() which
    // call update_flags() automatically.
    void recalculate() { update_flags(); }

private:
    // Recompute dead/crit flags after any damage or healing.
    // Mirrors TG's /mob/living/proc/updatehealth() → update_stat().
    void update_flags() {
        float hp = current();
        dead = (hp <= 0.f);
        crit = (!dead && hp <= CRIT_THRESHOLD);
        // Revive from dead if damage was healed back above zero
        // (explicit revival via defibrillator/CPR also calls this path)
    }
};

// ── MobPlayerTag ─────────────────────────────────────────────────────────────
// Marker component: entity is controlled by a local player.
// Used by the server damage tick to apply atmospheric harm.
struct MobPlayerTag {
    std::string species = "human";
};

// ── Human overlay assembly ────────────────────────────────────────────────────
// A single composited layer in a HumanAppearance stack.
//
// kind = Bodypart (default):
//   Composites 8 directional body-part sprites.
//   sprite_dir – sub-directory under legacysets/extracted/mob/human/
//                e.g. "bodyparts", "bodyparts_greyscale"
//   prefix     – filename prefix, e.g. "default_human", "human"
//   gender     – optional infix: "", "_f", "_m"
//   The loader key is:  {sprite_dir}/{prefix}_{part}{gender}_{dir_suffix}
//
// kind = Clothing:
//   A single full-body worn-clothing overlay (suit, helmet, gloves, …).
//   sprite_dir – path relative to extracted/mob/, e.g. "clothing/suits/spacesuit"
//   prefix     – sprite name without dir suffix, e.g. "space"
//   The loader key is:  {sprite_dir}/{prefix}{dir_suffix}
//
// kind = Inhand:
//   A single in-hand item sprite drawn over the body.
//   sprite_dir – path relative to extracted/mob/, e.g. "inhands/tools_lefthand"
//   prefix     – sprite name without dir suffix, e.g. "wrench"
//   The loader key is:  {sprite_dir}/{prefix}{dir_suffix}
//
// tint – RGBA colour multiply for all kinds; {255,255,255,255} = identity.
struct SpriteColor { uint8_t r=255,g=255,b=255,a=255; };

enum class HumanOverlayKind { Bodypart, Clothing, Inhand };

struct HumanOverlay {
    HumanOverlayKind kind       = HumanOverlayKind::Bodypart;
    std::string      sprite_dir = "bodyparts"; // meaning depends on kind (see above)
    std::string      prefix;                   // sprite name (no direction suffix)
    std::string      gender;                   // Bodypart only: "", "_f", "_m"
    SpriteColor      tint;                     // RGBA multiply tint
};

// Component: mark an entity for overlay-based human sprite assembly.
// Add this alongside (or instead of) MobComponent for human/humanoid mobs.
// The renderer composites all layers bottom-to-top each time dirty==true,
// then caches the result as a GPU texture layer until the next change.
struct HumanAppearance {
    std::vector<HumanOverlay> layers;  // composited bottom-to-top
    bool dirty = true;                 // set true whenever layers change
};

// ── DensityComponent ──────────────────────────────────────────────────────────
// Mirrors TG SS13's /atom/var/density.
// When dense == true the entity occupies physical space and blocks movement:
//   • Other CharacterControllers cannot pass through the entity's AABB.
//   • Attempting to do so triggers bump_attack() / COMSIG_ATOM_BUMPED.
// Set dense = false for ghosts, incorporeal mobs, etc.
struct DensityComponent {
    bool dense = true;
};

// ── NameComponent ─────────────────────────────────────────────────────────────
// Human-readable name shown in examine / context menus and examine output.
// Mirrors /atom/var/name in TG.
struct NameComponent {
    std::string name;
    std::string desc;  // examine description (mirrors /atom/var/desc)
};

// ── MobTypeTag ────────────────────────────────────────────────────────────────
// Marker: entity is a mob (living or not).  Used for quick istype() checks
// without looking up the full type-path hierarchy.
struct MobTypeTag {
    std::string type_path = "/mob/living/carbon/human";
};

// ── CorpseComponent ───────────────────────────────────────────────────────────
// Automatically added when a mob's HealthComponent.dead becomes true.
// Tracks time-since-death for examine text, revivability, and cause of death.
// Mirrors TG's /mob/living/proc/death / revival logic.
struct CorpseComponent {
    float       time_since_death = 0.f;    // seconds since death; incremented each tick
    bool        can_be_revived   = true;   // false after decay_time seconds
    float       decay_time       = 300.f;  // 5 minutes before revival impossible
    std::string cause_of_death   = "unknown causes";
};

// ── DragComponent ─────────────────────────────────────────────────────────────
// Marks an entity that is being physically dragged (pulled) by another entity.
// Mirrors TG's pull/drag mechanic — used primarily for corpse dragging.
// The server tick moves the dragged entity to follow directly behind the dragger.
struct DragComponent {
    EntityID dragger = NULL_ENTITY;
};

// ── NpcAiState ────────────────────────────────────────────────────────────────
enum class NpcAiState : uint8_t {
    Idle   = 0,   // stationary; idle_timer counts down then→Wander
    Wander = 1,   // moving toward wander_target
    Flee   = 2,   // fleeing away from a threat entity
    Chase  = 3,   // pursuing a target entity (hostile or curious)
};

// ── NpcAiComponent ────────────────────────────────────────────────────────────
// Autonomous wander/flee/chase behaviour for non-player mob entities.
// Ticked by tick_npc_ai() in npc_ai.cpp, called from Server::tick().
struct NpcAiComponent {
    NpcAiState state         = NpcAiState::Idle;
    glm::vec3  wander_target {};              // current wander destination
    float      idle_timer    = 2.f;           // seconds before next wander impulse
    float      wander_timer  = 0.f;           // deadline for reaching wander_target
    float      wander_radius = 4.f;           // max wander distance from spawn_pos
    glm::vec3  spawn_pos     {};              // world position at time of creation
    bool       wanders       = true;          // false = pins NPC at spawn_pos

    // ── Flee / Chase config ───────────────────────────────────────────────────
    // flee_dist > 0  → mob will flee from any MobPlayerTag entity within this
    //                  radius.  Overrides wander/idle when triggered.
    // aggro_dist > 0 → mob will chase MobPlayerTag entities within this radius.
    //                  Flee takes priority over Chase.
    // flee_speed_mult → speed factor applied while in Flee state (>1 = faster).
    // threat_eid     → entity currently being fled from / chased.
    // flee_timer     → minimum seconds to keep fleeing even after threat leaves
    //                  range, so the mob doesn't rubber-band instantly.
    float      flee_dist       = 0.f;         // sense radius for fleeing (0=disabled)
    float      aggro_dist      = 0.f;         // sense radius for chasing (0=disabled)
    float      flee_speed_mult = 1.5f;        // speed multiplier while fleeing
    float      flee_timer      = 0.f;         // remaining forced-flee seconds
    EntityID   threat_eid      = NULL_ENTITY; // current threat / chase target
};
