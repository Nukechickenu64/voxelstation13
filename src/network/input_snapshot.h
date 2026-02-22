#pragma once
#include <glm/glm.hpp>

// Snapshot of one frame's input, sent from client to server each tick.
struct InputSnapshot {
    glm::vec3 wish_dir{};   // movement direction (world-space XZ)
    float     yaw   = 0.f;
    float     pitch = 0.f;
    bool      jump  = false;
    bool      crouch= false;
    bool      sprint= false;
    bool      primary_interact  = false;
    bool      secondary_interact= false;
    uint64_t  tick_id = 0;      // client-side tick counter for rewind
};
