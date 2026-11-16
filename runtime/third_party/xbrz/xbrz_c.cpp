// C entry for the runtime (gpu_sdl3vk/ge_gpu.c texture upscaling): xBRZ on RGBA8 pixels as the
// GE decoder writes them (bytes R, G, B, A). xBRZ wants ARGB words (0xAARRGGBB), so red and blue
// are swapped on the way in and out. factor 2..6; dst holds (w * factor) x (h * factor) pixels.
#include "xbrz.h"
#include <vector>

extern "C" void xbrz_scale_rgba(int factor, const uint32_t *src, uint32_t *dst, int w, int h) {
    std::vector<uint32_t> in((size_t)w * h);
    for (size_t i = 0; i < in.size(); i++) {
        const uint32_t c = src[i];
        in[i] = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
    }
    xbrz::scale((size_t)factor, in.data(), dst, w, h, xbrz::ColorFormat::ARGB);
    const size_t n = (size_t)w * h * factor * factor;
    for (size_t i = 0; i < n; i++) {
        const uint32_t c = dst[i];
        dst[i] = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
    }
}
