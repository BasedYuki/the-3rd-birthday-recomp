/* Stand-in for PPSSPP's Common/Log.h: messages go to stderr. */
#pragma once
#include <stdio.h>
namespace Log { enum { Loader = 0 }; }
#define INFO_LOG(cat, ...)  ((void)(cat), fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#define DEBUG_LOG(cat, ...) ((void)(cat))
#define WARN_LOG(cat, ...)  INFO_LOG(cat, __VA_ARGS__)
#define ERROR_LOG(cat, ...) INFO_LOG(cat, __VA_ARGS__)
