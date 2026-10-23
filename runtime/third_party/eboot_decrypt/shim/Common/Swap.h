/* Stand-in for PPSSPP's Common/Swap.h: x86 is little-endian, so the _le types are plain. */
#pragma once
#include "CommonTypes.h"
typedef u16 u16_le; typedef u32 u32_le; typedef u64 u64_le;
typedef s16 s16_le; typedef s32 s32_le; typedef s64 s64_le;
