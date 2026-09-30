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
uint32_t fake_tdo(void);
uint32_t fake_tdi(void);
void fake_tdi_output(unsigned value);
#ifdef __cplusplus
}
#endif
