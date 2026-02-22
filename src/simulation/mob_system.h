#pragma once
#include "core/types.h"
#include <string>

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
