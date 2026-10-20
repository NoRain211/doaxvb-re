#ifndef DOAXBV_RECOMP_D3D_TUNING_H
#define DOAXBV_RECOMP_D3D_TUNING_H

#include <atomic>

/* Live settings changed by hotkeys on the game thread (d3d_vblank.cpp) and read
   by the render worker. RTGI seeds the values from its environment variables. */
struct RecompTuning {
    std::atomic<float> rtgi_strength{1.0f};
    std::atomic<bool> rtgi_split{false};
    std::atomic<bool> rtgi_off{false};
    std::atomic<bool> paused{false};
    std::atomic<unsigned> gbuffer_cycles{0}; // F11 presses: game, normal, depth view
};
inline RecompTuning recomp_tuning;

#endif
