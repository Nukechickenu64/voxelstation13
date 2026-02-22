#pragma once
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <SDL3/SDL.h>

struct SoundEvent {
    std::string id;
    std::string file;
    float       volume     = 1.f;
    float       pitch      = 1.f;
    float       max_dist   = 20.f;
    bool        spatial    = true;
    bool        looping    = false;
};

using SoundHandle = uint32_t;
constexpr SoundHandle INVALID_SOUND = 0;

// Spatial audio using SDL3's audio subsystem.
// Sounds attenuate by distance; vacuum zones suppress propagation.
class AudioManager {
public:
    AudioManager();
    ~AudioManager();

    bool init();
    void shutdown();

    // Update listener position/orientation each frame
    void set_listener(glm::vec3 pos, glm::vec3 forward, glm::vec3 up);

    // Play a sound at a world position; returns handle for stop/modify
    SoundHandle play(const std::string& event_id, glm::vec3 world_pos);

    // Play a non-spatial (UI/ambient) sound
    SoundHandle play_ui(const std::string& event_id);

    void stop(SoundHandle handle);
    void set_volume(SoundHandle handle, float vol);

    // Called each frame — updates pan/volume for active spatial sounds
    void update(float dt);

    // Set the atmospheric pressure at listener (affects propagation)
    void set_local_pressure(float kpa);

    bool load_events(const std::string& json_path);

private:
    struct ActiveSound {
        SoundHandle    handle{};
        SDL_AudioStream* stream = nullptr;
        glm::vec3      world_pos{};
        bool           spatial = true;
        float          base_vol= 1.f;
        const SoundEvent* event_def = nullptr;
    };

    glm::vec3 m_listener_pos{};
    glm::vec3 m_listener_fwd{0,0,-1};
    float     m_local_pressure = 101.325f; // standard atm kPa

    std::unordered_map<std::string, SoundEvent> m_events;
    std::unordered_map<SoundHandle, ActiveSound> m_active;
    SoundHandle m_next_handle = 1;

    SDL_AudioDeviceID m_device_id = 0;
};
