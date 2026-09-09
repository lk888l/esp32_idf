#pragma once
#include <cstddef>
#include <cstdint>
#include "gpio.h"
using i2c_port_t = int;
constexpr int I2C_NUM_1=1, I2C_CLK_SRC_DEFAULT=0, I2C_ADDR_BIT_LEN_7=0;
using i2c_master_bus_handle_t = void*;
using i2c_master_dev_handle_t = void*;
struct i2c_master_bus_config_t {
 i2c_port_t i2c_port; gpio_num_t sda_io_num, scl_io_num; int clk_source;
 uint8_t glitch_ignore_cnt; int intr_priority; size_t trans_queue_depth;
 struct { bool enable_internal_pullup, allow_pd; } flags;
};
struct i2c_device_config_t {
 int dev_addr_length; uint16_t device_address; uint32_t scl_speed_hz, scl_wait_us;
 struct { bool disable_ack_check; } flags;
};
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t*, i2c_master_bus_handle_t*);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t*, i2c_master_dev_handle_t*);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t);
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t, const uint8_t*, size_t, uint8_t*, size_t, int);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t, const uint8_t*, size_t, int);
