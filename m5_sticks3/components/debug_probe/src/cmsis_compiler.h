#pragma once
#define __STATIC_INLINE static inline
#define __STATIC_FORCEINLINE static inline __attribute__((always_inline))
#define __WEAK __attribute__((weak))
#define __ASM __asm
#define __NOP() __asm__ volatile ("nop")
