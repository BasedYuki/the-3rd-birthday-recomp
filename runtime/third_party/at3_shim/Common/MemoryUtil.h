/* Stand-in for PPSSPP's Common/MemoryUtil.h: the aligned allocator at3_standalone/mem.cpp uses. */
#pragma once
#include <cstddef>
#include <cstdlib>
#ifdef _WIN32
#include <malloc.h>
static inline void *AllocateAlignedMemory(size_t size, size_t align) { return _aligned_malloc(size, align); }
static inline void FreeAlignedMemory(void *p) { _aligned_free(p); }
#else
static inline void *AllocateAlignedMemory(size_t size, size_t align) {
    void *p = nullptr; return posix_memalign(&p, align, size) == 0 ? p : nullptr;
}
static inline void FreeAlignedMemory(void *p) { free(p); }
#endif
