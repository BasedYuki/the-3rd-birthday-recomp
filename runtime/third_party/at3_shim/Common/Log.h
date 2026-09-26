/* Stand-in for PPSSPP's Common/Log.h as used by at3_standalone/compat.cpp: errors and
 * warnings go to stderr (rate-limited), debug/info are dropped. */
#pragma once
#include <cstdio>
namespace Log { enum Type { ME }; }
#define AT3_SHIM_LOG(...) do { static int n_ = 0; if (n_ < 20) { n_++; fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define ERROR_LOG(t, ...) AT3_SHIM_LOG(__VA_ARGS__)
#define WARN_LOG(t, ...)  AT3_SHIM_LOG(__VA_ARGS__)
#define INFO_LOG(t, ...)  do { } while (0)
#define DEBUG_LOG(t, ...) do { } while (0)
