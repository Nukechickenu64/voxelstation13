#include "core/game_loop.h"
#include <SDL3/SDL.h>
#include <algorithm>

GameLoop::GameLoop(double fixed_dt)
    : m_fixed_dt(fixed_dt)
{}

void GameLoop::run(UpdateFn update_fn, RenderFn render_fn)
{
    m_running = true;

    // SDL3 high-resolution timer
    const double freq       = static_cast<double>(SDL_GetPerformanceFrequency());
    uint64_t     prev_count = SDL_GetPerformanceCounter();
    double       accumulator= 0.0;
    double       fps_accum  = 0.0;
    int          fps_frames = 0;

    while (m_running) {
        const uint64_t now = SDL_GetPerformanceCounter();
        double frame_time  = static_cast<double>(now - prev_count) / freq;
        prev_count         = now;

        // Cap to avoid spiral of death on very slow frames
        frame_time = std::min(frame_time, 0.25);

        accumulator += frame_time;

        // Fixed-rate update steps
        while (accumulator >= m_fixed_dt) {
            update_fn(m_fixed_dt);
            accumulator -= m_fixed_dt;
            ++m_tick_count;
        }

        // Variable-rate render with interpolation alpha
        const double alpha = accumulator / m_fixed_dt;
        render_fn(alpha);

        // FPS counter
        fps_accum += frame_time;
        ++fps_frames;
        if (fps_accum >= 1.0) {
            m_fps     = fps_frames / fps_accum;
            fps_accum = 0.0;
            fps_frames= 0;
        }
    }
}

void GameLoop::stop()
{
    m_running = false;
}
