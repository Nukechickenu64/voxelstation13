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
// SS13-style damage tracking for mobs.  Health = health_max – total_damage().
// Four independent damage buckets mirror the human.json damage_types list.
//
//   brute — physical trauma (punching, explosions)
//   burn  — heat / electrical damage
//   tox   — toxin accumulation (plasma gas, drugs)
//   oxy   — oxygen deprivation
//
// Each bucket accumulates damage points (0-health_max each).
// total_damage() sums all four; effective health = health_max - total_damage().
// The mob is considered dead when effective health <= 0.

struct HealthComponent {
    float health_max = 100.f;

    float brute = 0.f;
    float burn  = 0.f;
    float tox   = 0.f;
    float oxy   = 0.f;

    bool  dead  = false;

    // Current effective health [0, health_max]
    float current() const {
        return std::max(0.f, health_max - (brute + burn + tox + oxy));
    }

    // Apply damage of a named type.  Clamps each bucket to [0, health_max].
    void apply(const std::string& type, float amount) {
        auto clamp = [&](float& bucket) {
            bucket = std::clamp(bucket + amount, 0.f, health_max);
        };
        if      (type == "brute") clamp(brute);
        else if (type == "burn")  clamp(burn);
        else if (type == "tox")   clamp(tox);
        else if (type == "oxy")   clamp(oxy);
        if (current() <= 0.f) dead = true;
    }

    // Heal all damage types by amount.
    void heal(float amount) {
        brute = std::max(0.f, brute - amount);
        burn  = std::max(0.f, burn  - amount);
        tox   = std::max(0.f, tox   - amount);
        oxy   = std::max(0.f, oxy   - amount);
        if (current() > 0.f) dead = false;
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
// sprite_dir  – sub-directory under legacysets/extracted/mob/human/
//               e.g. "bodyparts", "bodyparts_greyscale"
// prefix      – filename prefix, e.g. "default_human", "human", "skeleton"
//               The loader concatenates: {prefix}_{part}{gender}_{dir_suffix}.png
//               for each of the 8 standard body-part names and 4 direction suffixes.
// gender      – optional infix for greyscale chest/head variants: "", "_f", "_m"
// tint        – per-layer RGBA colour multiply applied before alpha-compositing.
//               {255,255,255,255} = no tint (identity).
struct SpriteColor { uint8_t r=255,g=255,b=255,a=255; };

struct HumanOverlay {
    std::string sprite_dir = "bodyparts"; // relative to extracted/mob/human/
    std::string prefix;                   // e.g. "default_human"
    std::string gender;                   // "", "_f", or "_m"
    SpriteColor tint;                     // RGBA multiply tint
};

// Component: mark an entity for overlay-based human sprite assembly.
// Add this alongside (or instead of) MobComponent for human/humanoid mobs.
// The renderer composites all layers bottom-to-top each time dirty==true,
// then caches the result as a GPU texture layer until the next change.
struct HumanAppearance {
    std::vector<HumanOverlay> layers;  // composited bottom-to-top
    bool dirty = true;                 // set true whenever layers change
};
