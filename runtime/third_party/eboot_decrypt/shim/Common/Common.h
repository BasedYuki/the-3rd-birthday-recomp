/* Stand-in for PPSSPP's Common/Common.h (eboot_decrypt only). */
#pragma once
#include "CommonTypes.h"
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
