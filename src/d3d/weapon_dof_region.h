#pragma once

#include <cstdint>
#include <cmath>

namespace gpu {

// Fractional tile viewports can suppress the last pixel even under conservative
// rasterization. Use only exact whole-tile host viewports; never stretch them.
inline bool WeaponDofViewportSupported(float x, float y, float width, float height) {
    const auto aligned = [](float v) { return std::isfinite(v) && std::fmod(v, 8.0f) == 0; };
    return x >= 0 && y >= 0 && width > 0 && height > 0 &&
           aligned(x) && aligned(y) && aligned(width) && aligned(height);
}

// Each accepted replay accounts for exactly one ensuing native depth-write
// serial increment. Any other mutation (clear, copy, raw command-list access,
// target recreation) makes the region unusable until the next Begin.
struct WeaponDofRegion {
    uint64_t expected_serial = 0;
    bool valid = false;

    void Begin(uint64_t serial, bool available) { expected_serial = serial; valid = available; }
    void Invalidate() { valid = false; }
    bool Complete(uint64_t serial) const { return valid && serial == expected_serial; }
    bool AcceptDraw(uint64_t serial, bool supported) {
        valid = Complete(serial) && supported;
        if (valid) ++expected_serial;
        return valid;
    }
};

} // namespace gpu
