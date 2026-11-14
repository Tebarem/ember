/*
 * SheenBidi unity build (R2c). SheenBidi.c includes every SheenBidi source file when
 * SB_CONFIG_UNITY is defined. Compiled with warnings off (module.cmake). Its allocations go
 * through the default allocator, which text_bidi.c replaces with an SDL-backed one at start-up.
 */
#define SB_CONFIG_UNITY 1
#include "SheenBidi.c"
