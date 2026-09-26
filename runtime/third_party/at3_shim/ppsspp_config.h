/* Minimal stand-in for PPSSPP's ppsspp_config.h: just the arch macros at3_standalone uses. */
#pragma once
#define PPSSPP_ARCH(x) (PPSSPP_ARCH_##x)
#if defined(__x86_64__) || defined(_M_X64)
#define PPSSPP_ARCH_AMD64 1
#define PPSSPP_ARCH_X86 0
#define PPSSPP_ARCH_SSE2 1
#define PPSSPP_ARCH_ARM_NEON 0
#define PPSSPP_ARCH_ARM64 0
#elif defined(__aarch64__) || defined(_M_ARM64)
#define PPSSPP_ARCH_AMD64 0
#define PPSSPP_ARCH_X86 0
#define PPSSPP_ARCH_SSE2 0
#define PPSSPP_ARCH_ARM_NEON 1
#define PPSSPP_ARCH_ARM64 1
#else
#define PPSSPP_ARCH_AMD64 0
#define PPSSPP_ARCH_X86 0
#define PPSSPP_ARCH_SSE2 0
#define PPSSPP_ARCH_ARM_NEON 0
#define PPSSPP_ARCH_ARM64 0
#endif
