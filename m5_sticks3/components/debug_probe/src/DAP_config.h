#pragma once
#include <stdint.h>
#include <string.h>
#include "cmsis_compiler.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "soc/gpio_struct.h"

#define CPU_CLOCK 240000000U
#define IO_PORT_WRITE_CYCLES 0U
#define DAP_SWD 1
#define DAP_JTAG 0
#define DAP_DEFAULT_PORT 1U
#define DAP_DEFAULT_SWJ_CLOCK 1000000U
#define DAP_PACKET_SIZE 64U
#define DAP_PACKET_COUNT 1U
#define SWO_UART 0
#define SWO_MANCHESTER 0
#define SWO_STREAM 0
#define DAP_UART 0
#define DAP_UART_USB_COM_PORT 0
#define TIMESTAMP_CLOCK 1000000U
#define TARGET_FIXED 0

#ifdef __cplusplus
extern "C" {
#endif
uint32_t probe_set_clock(uint32_t hz);
void probe_port_off(void);
void probe_port_swd(void);
uint8_t probe_serial(char* text);
uint8_t probe_reset(void);
int probe_cancelled(void);
void probe_host_status(unsigned kind, unsigned value);
#ifdef __cplusplus
}
#endif

static inline void probe_delay_cycles(uint32_t cycles) {
    const uint32_t start = esp_cpu_get_cycle_count();
    while ((uint32_t)(esp_cpu_get_cycle_count() - start) < cycles) { __asm__ volatile("nop"); }
}
static inline uint8_t DAP_GetVendorString(char* p) { strcpy(p,"M5StickS3"); return 10; }
static inline uint8_t DAP_GetProductString(char* p) { strcpy(p,"StickS3 CMSIS-DAP"); return 18; }
static inline uint8_t DAP_GetTargetDeviceVendorString(char* p) { (void)p; return 0; }
static inline uint8_t DAP_GetTargetDeviceNameString(char* p) { (void)p; return 0; }
static inline uint8_t DAP_GetTargetBoardVendorString(char* p) { (void)p; return 0; }
static inline uint8_t DAP_GetTargetBoardNameString(char* p) { (void)p; return 0; }
static inline uint8_t DAP_GetSerNumString(char* p) { return probe_serial(p); }
static inline uint8_t DAP_GetProductFirmwareVersionString(char* p) { strcpy(p,"1.0.0"); return 6; }
#define PORT_OFF() probe_port_off()
#define DAP_SETUP() probe_port_off()
#define PORT_SWD_SETUP() probe_port_swd()
#define RESET_TARGET() probe_reset()
#define LED_CONNECTED_OUT(v) probe_host_status(0,(v))
#define LED_RUNNING_OUT(v) probe_host_status(1,(v))
#define TIMESTAMP_GET() ((uint32_t)esp_timer_get_time())
#define PIN_SWCLK_TCK_SET() (GPIO.out_w1ts = (1U << 6))
#define PIN_SWCLK_TCK_CLR() (GPIO.out_w1tc = (1U << 6))
#define PIN_SWCLK_TCK_IN() ((GPIO.in >> 6) & 1U)
#define PIN_SWDIO_TMS_SET() (GPIO.out_w1ts = (1U << 7))
#define PIN_SWDIO_TMS_CLR() (GPIO.out_w1tc = (1U << 7))
#define PIN_SWDIO_TMS_IN() ((GPIO.in >> 7) & 1U)
#define PIN_SWDIO_IN() PIN_SWDIO_TMS_IN()
#define PIN_SWDIO_OUT(v) do { if ((v)&1U) PIN_SWDIO_TMS_SET(); else PIN_SWDIO_TMS_CLR(); } while(0)
#define PIN_SWDIO_OUT_ENABLE() (GPIO.enable_w1ts = (1U << 7))
#define PIN_SWDIO_OUT_DISABLE() (GPIO.enable_w1tc = (1U << 7))
#define PIN_nRESET_IN() ((GPIO.in >> 8) & 1U)
// Reset is always open drain: high releases the target's own pull-up.
#define PIN_nRESET_OUT(v) do { if ((v)&1U) GPIO.out_w1ts = (1U << 8); else GPIO.out_w1tc = (1U << 8); } while(0)
#define PIN_TDI_IN() 0U
#define PIN_TDO_IN() 0U
#define PIN_nTRST_IN() 1U
#define PIN_TDI_OUT(v) ((void)(v))
#define PIN_nTRST_OUT(v) ((void)(v))
