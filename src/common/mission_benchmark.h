#pragma once

#if defined(BLACK_MISSION_BENCHMARK)
#include <cstdint>
struct X86Context;
namespace input { struct Pad; }

namespace game::bench {
bool Enabled();
void Init();
void Window(void* hwnd);
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
void Pad(int port, input::Pad& pad);
void Tick(X86Context& ctx);
bool SceneReady();
uint32_t SceneFrame();
uint64_t CameraSignature();
// Readback is admitted only after settled warmup; successful completion gates epochs.
bool ProofCaptureRequested();
// Renderer calls before Present with that upcoming present ID and actual file-save result.
void ProofCaptureCompleted(uint64_t present_id, bool succeeded);
void Stop(const char* reason);

struct Frame {
    uint64_t id;
    uint64_t batches, atlas_pages, atlas_quads, cached_pages, cached_quads, direct_quads;
    uint32_t width, height, samples;
    bool world;
    int32_t present_result;
    double present_ms, frame_ms;
    bool proof_capture;
};
void Present(const Frame& frame);
} // namespace game::bench
#endif
