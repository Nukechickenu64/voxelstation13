#include "audio/audio_manager.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cmath>
#include <SDL3/SDL.h>

using json = nlohmann::json;

AudioManager::AudioManager()  = default;
AudioManager::~AudioManager() { shutdown(); }

bool AudioManager::init()
{
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        SDL_Log("AudioManager: SDL_Init(AUDIO) failed: %s", SDL_GetError());
        return false;
    }

    SDL_AudioSpec spec{};
    spec.format   = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq     = 48000;

    m_device_id = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!m_device_id) {
        SDL_Log("AudioManager: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

void AudioManager::shutdown()
{
    for (auto& [h, s] : m_active)
        if (s.stream) SDL_DestroyAudioStream(s.stream);
    m_active.clear();
    if (m_device_id) SDL_CloseAudioDevice(m_device_id);
    m_device_id = 0;
}

void AudioManager::set_listener(glm::vec3 pos, glm::vec3 forward, glm::vec3 /*up*/)
{
    m_listener_pos = pos;
    m_listener_fwd = forward;
}

SoundHandle AudioManager::play(const std::string& event_id, glm::vec3 world_pos)
{
    auto it = m_events.find(event_id);
    if (it == m_events.end()) return INVALID_SOUND;

    SoundHandle h = m_next_handle++;
    ActiveSound s;
    s.handle    = h;
    s.world_pos = world_pos;
    s.spatial   = true;
    s.base_vol  = it->second.volume;
    s.event_def = &it->second;
    // Load audio file and create SDL_AudioStream bound to the output device.
    {
        std::string wav_path = "sounds/" + it->second.file;
        SDL_IOStream* io = SDL_IOFromFile(wav_path.c_str(), "rb");
        if (io) {
            SDL_AudioSpec wav_spec{};
            Uint8* wav_buf  = nullptr;
            Uint32 wav_len  = 0;
            if (SDL_LoadWAV_IO(io, true, &wav_spec, &wav_buf, &wav_len)) {
                SDL_AudioSpec dst{ SDL_AUDIO_F32, 2, 48000 };
                s.stream = SDL_CreateAudioStream(&wav_spec, &dst);
                if (s.stream) {
                    SDL_SetAudioStreamGain(s.stream, s.base_vol);
                    SDL_PutAudioStreamData(s.stream, wav_buf, (int)wav_len);
                    SDL_BindAudioStream(m_device_id, s.stream);
                } else {
                    SDL_Log("AudioManager: SDL_CreateAudioStream failed: %s", SDL_GetError());
                }
                SDL_free(wav_buf);
            } else {
                SDL_Log("AudioManager: SDL_LoadWAV_IO failed for '%s': %s",
                        wav_path.c_str(), SDL_GetError());
            }
        } else {
            SDL_Log("AudioManager: file not found: %s", wav_path.c_str());
        }
    }
    m_active[h] = std::move(s);
    return h;
}

SoundHandle AudioManager::play_ui(const std::string& event_id)
{
    auto it = m_events.find(event_id);
    if (it == m_events.end()) return INVALID_SOUND;
    SoundHandle h = m_next_handle++;
    ActiveSound s;
    s.handle    = h;
    s.spatial   = false;
    s.base_vol  = it->second.volume;
    s.event_def = &it->second;
    m_active[h] = std::move(s);
    return h;
}

void AudioManager::stop(SoundHandle handle)
{
    auto it = m_active.find(handle);
    if (it == m_active.end()) return;
    if (it->second.stream) SDL_DestroyAudioStream(it->second.stream);
    m_active.erase(it);
}

void AudioManager::set_volume(SoundHandle handle, float vol)
{
    auto it = m_active.find(handle);
    if (it == m_active.end()) return;
    if (it->second.stream)
        SDL_SetAudioStreamGain(it->second.stream, vol);
}

void AudioManager::update(float /*dt*/)
{
    // Recompute pan/volume for each spatial sound based on listener position
    for (auto& [h, s] : m_active) {
        if (!s.spatial || !s.event_def) continue;

        float dist = glm::length(s.world_pos - m_listener_pos);
        float atten = 1.f - (dist / s.event_def->max_dist);
        if (atten <= 0.f) { atten = 0.f; }

        // Attenuate harder in low-pressure (vacuum) — linear ramp to 0 at <5 kPa
        float pressure_factor = std::min(m_local_pressure / 20.f, 1.f);
        float final_vol = s.base_vol * atten * pressure_factor * m_master_volume * m_sfx_volume;
        if (s.stream)
            SDL_SetAudioStreamGain(s.stream, final_vol);
    }
}

void AudioManager::set_local_pressure(float kpa)
{
    m_local_pressure = kpa;
}

bool AudioManager::load_events(const std::string& json_path)
{
    std::ifstream f(json_path);
    if (!f.is_open()) return false;
    json j; f >> j;
    for (auto& ev : j) {
        SoundEvent se;
        se.id       = ev.at("id").get<std::string>();
        se.file     = ev.at("file").get<std::string>();
        se.volume   = ev.value("volume",   1.f);
        se.pitch    = ev.value("pitch",    1.f);
        se.max_dist = ev.value("max_dist", 20.f);
        se.spatial  = ev.value("spatial",  true);
        se.looping  = ev.value("looping",  false);
        m_events[se.id] = std::move(se);
    }
    return true;
}
