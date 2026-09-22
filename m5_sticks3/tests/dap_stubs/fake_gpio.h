#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t fake_timestamp(void);
void fake_clock(unsigned value);
void fake_output(unsigned value);
void fake_enable(unsigned value);
uint32_t fake_input(void);
#ifdef __cplusplus
}
#endif
