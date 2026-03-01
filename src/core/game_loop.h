#pragma once
#include <cstdint>
#include <functional>

// Fixed-timestep game loop with variable-rate rendering.
//
//  update_fn  — called at a fixed dt (default 1/60 s), carries game logic tick
//  render_fn  — called once per displayed frame, receives alpha for interpolation
class GameLoop {
public:
    using UpdateFn = std::function<void(double dt)>;
    using RenderFn = std::function<void(double alpha)>;

    explicit GameLoop(double fixed_dt = 1.0 / 60.0);

    void run(UpdateFn update_fn, RenderFn render_fn);
    void stop();

    double fixed_dt()      const { return m_fixed_dt; }
    uint64_t tick_count()  const { return m_tick_count; }
    double fps()           const { return m_fps; }

private:
    // Number of frames used in the sliding-window FPS average.
    static constexpr int k_fps_window = 60;

    double   m_fixed_dt;
    bool     m_running   = false;
    uint64_t m_tick_count= 0;
    double   m_fps       = 0.0;

    // Rolling-window frame times (ring buffer).
    double   m_frame_times[60] = {};
    int      m_ft_head  = 0;
    int      m_ft_count = 0;
};
