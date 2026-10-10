#include "bsp_board.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(expr) do { if (!(expr)) { \
 std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); \
} } while (0)

namespace {
std::array<uint8_t, 256> registers{};
std::array<uint8_t, 256> bmi_registers{};
std::array<unsigned, 256> bmi_write_count{};
int fail_bmi_read_register = -1;
int fail_bmi_write_register = -1;
bool fail_bmi_write_after_effect = false;
int ignore_bmi_write_register = -1;
bool fail_bmi_add = false;
bool fail_bmi_remove = false;
bool bmi_device_active = false;
int fail_read_register = -1;
std::array<unsigned, 256> read_failures_remaining{};
int fail_write_register = -1;
bool fail_write_after_effect = false;
int ignore_write_register = -1;
std::array<unsigned, 256> write_count{};
bool core_rail_dropped = false;
bool imu_rail_dropped = false;
bool fail_bus_delete = false;
int transaction_count = 0;
constexpr uint8_t boost = 8;
void voltage(uint8_t reg, uint16_t mv) {
 registers[reg] = static_cast<uint8_t>(mv);
 registers[reg + 1] = static_cast<uint8_t>(mv >> 8);
}
void reset(uint16_t external_mv = 0, bool boosted = false) {
 auto& board = bsp::Board::instance();
 CHECK(board.deinitialize() == ESP_OK);
 CHECK(!bmi_device_active);
 registers.fill(0);
 bmi_registers.fill(0);
 bmi_registers[0x00] = 0x24;
 bmi_registers[0x7C] = 0xA0; // Unrelated config bits survive advanced power-save enable.
 bmi_registers[0x7D] = 0x0E; // All acquisition blocks start enabled.
 bmi_write_count.fill(0);
 registers[0x06] = static_cast<uint8_t>(0x17 | (boosted ? boost : 0));
 registers[0x16] = 0xFF; // Every pin begins with a non-default function.
 registers[0x12] = 0x01; // CHG_STAT inactive (active low).
 voltage(0x22, 3900); voltage(0x24, 0); voltage(0x26, external_mv);
 CHECK(board.initialize() == ESP_OK);
}
} // namespace

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t*, i2c_master_bus_handle_t* out) {
 *out = registers.data(); return ESP_OK;
}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t* config,
                                    i2c_master_dev_handle_t* out) {
 if (config->device_address == bsp::kBmi270Address) {
  if (fail_bmi_add) { fail_bmi_add = false; return ESP_FAIL; }
  CHECK(!bmi_device_active);
  bmi_device_active = true;
  *out = bmi_registers.data();
 } else {
  CHECK(config->device_address == bsp::kM5Pm1Address);
  *out = registers.data();
 }
 return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device) {
 if (device == bmi_registers.data()) {
  CHECK(bmi_device_active);
  if (fail_bmi_remove) { fail_bmi_remove = false; return ESP_FAIL; }
  bmi_device_active = false;
 } else CHECK(device == registers.data());
 return ESP_OK;
}
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t) {
 CHECK(!bmi_device_active); // Losing a temporary-device handle prevents bus cleanup.
 if (fail_bus_delete) { fail_bus_delete = false; return ESP_FAIL; }
 return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t* in,
                                    size_t in_size, uint8_t* out, size_t out_size, int) {
 CHECK(in_size == 1);
 ++transaction_count;
 if (device == bmi_registers.data()) {
  CHECK(bmi_device_active && (registers[0x06] & 4)); // VDDIO must stay powered.
  if (in[0] == fail_bmi_read_register) { fail_bmi_read_register = -1; return ESP_FAIL; }
  CHECK(in[0] + out_size <= bmi_registers.size());
  for (size_t i = 0; i < out_size; ++i) out[i] = bmi_registers[in[0] + i];
  return ESP_OK;
 }
 CHECK(device == registers.data());
 if (read_failures_remaining[in[0]]) {
  --read_failures_remaining[in[0]];
  return ESP_FAIL;
 }
 if (in[0] == fail_read_register) { fail_read_register = -1; return ESP_FAIL; }
 CHECK(in[0] + out_size <= registers.size());
 for (size_t i = 0; i < out_size; ++i) out[i] = registers[in[0] + i];
 std::this_thread::yield(); // Amplify read/modify/write interleavings.
 return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t* in, size_t size, int) {
 CHECK(size == 2);
 ++transaction_count;
 if (device == bmi_registers.data()) {
  CHECK(bmi_device_active && (registers[0x06] & 4));
  ++bmi_write_count[in[0]];
  if (in[0] == ignore_bmi_write_register) {
   ignore_bmi_write_register = -1;
   return ESP_OK;
  }
  if (in[0] == fail_bmi_write_register) {
   fail_bmi_write_register = -1;
   if (fail_bmi_write_after_effect) bmi_registers[in[0]] = in[1];
   fail_bmi_write_after_effect = false;
   return ESP_FAIL;
  }
  bmi_registers[in[0]] = in[1];
  return ESP_OK;
 }
 CHECK(device == registers.data());
 ++write_count[in[0]];
 if (in[0] == 0x06 && !(in[1] & 2)) core_rail_dropped = true;
 if (in[0] == 0x06 && !(in[1] & 4)) imu_rail_dropped = true;
 if (in[0] == ignore_write_register) {
  ignore_write_register = -1;
  return ESP_OK; // Accepted transaction, hardware state did not change.
 }
 if (in[0] == fail_write_register) {
  fail_write_register = -1;
  if (fail_write_after_effect) registers[in[0]] = in[1];
  fail_write_after_effect = false;
  return ESP_FAIL;
 }
 registers[in[0]] = in[1];
 return ESP_OK;
}

int main() {
 auto& board = bsp::Board::instance();
 CHECK(!bsp::battery_voltage_valid(0));
 CHECK(bsp::battery_voltage_valid(3900));
 CHECK(bsp::estimate_battery_percent(3200) == 0);
 CHECK(bsp::estimate_battery_percent(3700) == 50);
 CHECK(bsp::estimate_battery_percent(4200) == 100);
 reset();
 CHECK((registers[0x16] & 0xC0) == 0); // GPIO3's actual TWO-bit field.
 CHECK((registers[0x16] & 0x03) == 0); // GPIO0 CHG_STAT normalized to GPIO.
 CHECK((registers[0x16] & 0x3C) == 0x3C);
 CHECK((registers[0x11] & 8) == 0);
 CHECK(board.enable_display_power() == ESP_OK);
 CHECK(registers[0x16] == 0x0C);
 CHECK((registers[0x10] & 0x03) == 0); // No charge/input pins driven.
 CHECK((registers[0x11] & 4) != 0);

 CHECK(board.set_speaker_enabled(true) == ESP_OK);
 CHECK(board.acquire_ir_receiver() == ESP_ERR_INVALID_STATE);
 CHECK(board.set_speaker_enabled(false) == ESP_OK);
 CHECK(board.acquire_ir_receiver() == ESP_OK);
 CHECK(board.acquire_ir_receiver() == ESP_ERR_INVALID_STATE);
 CHECK(board.set_speaker_enabled(true) == ESP_ERR_INVALID_STATE);
 CHECK(board.deinitialize() == ESP_ERR_INVALID_STATE);
 CHECK(board.release_ir_receiver() == ESP_OK);
 CHECK(board.release_ir_receiver() == ESP_ERR_INVALID_STATE);
 CHECK(board.set_speaker_enabled(true) == ESP_OK);
 CHECK(board.set_speaker_enabled(false) == ESP_OK);

 reset();
 CHECK(board.acquire_ir_power() == ESP_OK);
 CHECK(registers[0x06] & boost);
 CHECK(board.acquire_ir_power() == ESP_OK);
 CHECK(board.release_ir_power() == ESP_OK);
 CHECK(registers[0x06] & boost);
 CHECK(board.release_ir_power() == ESP_OK);
 CHECK(!(registers[0x06] & boost));
 CHECK(board.release_ir_power() == ESP_ERR_INVALID_STATE);
 CHECK(registers[0x10] == 8); // Only amplifier direction configured.

 fail_read_register = 0x09;
 reset(); // Sleeping PM1 NACKs its first address, then acknowledges after wake.
 CHECK(board.initialized());
 CHECK((registers[0x09] & 0x0F) == 0);

 // Two failed idle-config reads trigger direct stock-rail recovery. This write
 // turns charging/L1/L2 on and leaves external 5 V boost off, without a preread.
 CHECK(board.deinitialize() == ESP_OK);
 registers[0x06] = 0x1F; // Retained boost is disabled by recovery's stock config.
 registers[0x09] = 0x92;
 read_failures_remaining[0x09] = 2;
 write_count.fill(0);
 CHECK(board.initialize() == ESP_OK);
 CHECK(read_failures_remaining[0x09] == 0);
 CHECK(registers[0x06] == 0x17);
 CHECK(registers[0x09] == 0x90);
 CHECK(write_count[0x06] == 2); // Direct recovery then normal rail normalization.
 CHECK(write_count[0x0C] == 0);

 reset(5000); // Externally powered on Grove/HAT; never enable boost.
 CHECK(board.acquire_ir_power() == ESP_OK);
 CHECK(!(registers[0x06] & boost));
 bsp::PowerStatus power{};
 CHECK(board.read_power_status(power) == ESP_OK);
 CHECK(power.externally_powered());
 CHECK(power.battery_mv == 3900);
 CHECK(!power.charging);
 registers[0x12] = 0x00;
 CHECK(board.read_power_status(power) == ESP_OK);
 CHECK(power.charging);
 CHECK(board.release_ir_power() == ESP_OK);
 CHECK(!(registers[0x06] & boost));

 reset(2500); // Ambiguous external source / brownout.
 CHECK(board.acquire_ir_power() == ESP_ERR_INVALID_STATE);
 CHECK(!(registers[0x06] & boost));

 reset(5000, true); // Boost owned by earlier firmware/request.
 CHECK(board.acquire_ir_power() == ESP_OK);
 CHECK(board.release_ir_power() == ESP_OK);
 CHECK(board.deinitialize() == ESP_OK);
 CHECK(registers[0x06] & boost);

 reset();
 fail_read_register = 0x22;
 CHECK(board.acquire_ir_power() == ESP_FAIL);
 CHECK(!(registers[0x06] & boost));
 CHECK(board.release_ir_power() == ESP_ERR_INVALID_STATE);

 reset();
 fail_write_register = 0x06; fail_write_after_effect = true;
 CHECK(board.acquire_ir_power() == ESP_FAIL);
 CHECK(!(registers[0x06] & boost)); // Ambiguous ACK is rolled back.
 CHECK(board.release_ir_power() == ESP_ERR_INVALID_STATE);

 reset();
 CHECK(board.acquire_ir_power() == ESP_OK);
 fail_write_register = 0x06;
 CHECK(board.release_ir_power() == ESP_FAIL);
 CHECK(board.deinitialize() == ESP_ERR_INVALID_STATE); // Lease retained.
 CHECK(board.release_ir_power() == ESP_OK);
 CHECK(!(registers[0x06] & boost));

 reset();
 CHECK(board.enable_5v_output() == ESP_OK);
 CHECK(board.acquire_ir_power() == ESP_OK);
 CHECK(board.release_ir_power() == ESP_OK);
 CHECK(registers[0x06] & boost); // Explicit request remains until BSP shutdown.
 CHECK(board.deinitialize() == ESP_OK);
 CHECK(!(registers[0x06] & boost));

 reset();
 fail_write_register = 0x11; fail_write_after_effect = true;
 CHECK(board.set_speaker_enabled(true) == ESP_FAIL);
 CHECK(board.acquire_ir_receiver() == ESP_ERR_INVALID_STATE); // Unknown amp is reserved.
 CHECK(board.set_speaker_enabled(false) == ESP_OK);
 CHECK(board.acquire_ir_receiver() == ESP_OK);
 CHECK(board.release_ir_receiver() == ESP_OK);

 // L2 keeps ESP32/IMU supplies and charging, removing switched consumers.
 // The IMU shares an unisolated bus: suspend it while retaining its VDDIO rail.
 for (const bool charging_enabled : {false, true}) {
  reset(0, true);
  CHECK(board.enable_display_power() == ESP_OK);
  CHECK(board.set_speaker_enabled(true) == ESP_OK);
  registers[0x06] = static_cast<uint8_t>(0x9E | charging_enabled);
  registers[0x09] = 0x90; // Preserve upper control bits, including I2C speed.
  registers[0x11] |= 0x12; // Unrelated GPIO1/GPIO4 outputs must survive.
  registers[0x0A] = 30; // Stale PM1 watchdog must not cut L2 later.
  registers[0x3C] = 0x0E; // Stale armed POWER_OFF action must be disarmed.
  write_count.fill(0);
  core_rail_dropped = imu_rail_dropped = false;
  CHECK(board.prepare_l2_sleep() == ESP_OK);
  CHECK(registers[0x06] == static_cast<uint8_t>(0x86 | charging_enabled));
  CHECK(bmi_registers[0x7D] == 0 && bmi_registers[0x7C] == 0xA1);
  CHECK(bmi_write_count[0x7D] == 1 && bmi_write_count[0x7C] == 1);
  CHECK(!bmi_device_active);
  CHECK((registers[0x11] & 0x0C) == 0); // LCD/audio L3B and amplifier off.
  CHECK((registers[0x11] & 0x12) == 0x12);
  CHECK((registers[0x16] & 0xF0) == 0); // GPIO2/3 remain regular GPIO.
  CHECK((registers[0x10] & 0x0C) == 0x0C); // Driven outputs, not floating gates.
  CHECK((registers[0x13] & 0x0C) == 0); // Push-pull.
  CHECK(registers[0x0A] == 0 && registers[0x3C] == 0);
  CHECK(registers[0x09] == 0x92); // PM1 idle sleep, core supply retained.
  CHECK(write_count[0x0C] == 0 && !core_rail_dropped && !imu_rail_dropped);

  // Deep-sleep wake is a fresh ESP boot while PM1 retains its sleep settings.
  CHECK(board.deinitialize() == ESP_OK);
  fail_read_register = 0x09; // First address only wakes sleeping PM1.
  CHECK(board.initialize() == ESP_OK);
  CHECK(registers[0x09] == 0x90);
  CHECK(registers[0x06] == static_cast<uint8_t>(0x96 | charging_enabled));
  CHECK((registers[0x11] & 8) == 0);
  CHECK(board.enable_display_power() == ESP_OK);
  CHECK(registers[0x11] & 4);
 }

 reset();
 CHECK(board.acquire_ir_power() == ESP_OK);
 auto transactions_before = transaction_count;
 CHECK(board.prepare_l2_sleep() == ESP_ERR_INVALID_STATE);
 CHECK(transaction_count == transactions_before); // No rail edits with an active lease.
 CHECK(board.release_ir_power() == ESP_OK);
 CHECK(board.acquire_ir_receiver() == ESP_OK);
 transactions_before = transaction_count;
 CHECK(board.prepare_l2_sleep() == ESP_ERR_INVALID_STATE);
 CHECK(transaction_count == transactions_before);
 CHECK(board.release_ir_receiver() == ESP_OK);
 CHECK(board.deinitialize() == ESP_OK);
 transactions_before = transaction_count;
 CHECK(board.prepare_l2_sleep() == ESP_ERR_INVALID_STATE);
 CHECK(transaction_count == transactions_before);

 // Fail before preparation can mutate PM1 state: the existing display stays on.
 for (const uint8_t reg : {0x06, 0x11, 0x09}) {
  reset();
  CHECK(board.enable_display_power() == ESP_OK);
  const auto saved = registers;
  fail_read_register = reg;
  CHECK(board.prepare_l2_sleep() == ESP_FAIL);
  CHECK(registers == saved);
 }

 // An ACK can be lost after any applied rail write. Rollback retains L2,
 // restores the LCD/charging/boost state, disables idle sleep, and keeps amp muted.
 for (const bool applied : {false, true}) {
  for (const uint8_t reg : {0x11, 0x16, 0x13, 0x10, 0x0A, 0x3C, 0x06, 0x09}) {
   reset(0, true);
   CHECK(board.enable_display_power() == ESP_OK);
   CHECK(board.set_speaker_enabled(true) == ESP_OK);
   registers[0x09] = 0x90;
   registers[0x11] |= 0x12;
   const auto saved_power = registers[0x06];
   const auto saved_outputs = registers[0x11];
   write_count.fill(0);
   core_rail_dropped = false;
   fail_write_register = reg;
   fail_write_after_effect = applied;
   CHECK(board.prepare_l2_sleep() == ESP_FAIL);
   CHECK(registers[0x06] == saved_power);
   CHECK(registers[0x11] == (saved_outputs & ~8));
   CHECK(registers[0x09] == 0x90);
   CHECK(write_count[0x0C] == 0 && !core_rail_dropped);
  }
 }

 // BMI suspend errors must cancel sleep and remove the temporary I2C device.
 for (const uint8_t reg : {0x00, 0x7C, 0x7D}) {
  reset();
  CHECK(board.enable_display_power() == ESP_OK);
  const auto saved_power = registers[0x06];
  fail_bmi_read_register = reg;
  CHECK(board.prepare_l2_sleep() == ESP_FAIL);
  CHECK(registers[0x06] == saved_power && (registers[0x11] & 4));
  CHECK(!bmi_device_active);
 }
 for (const bool applied : {false, true}) {
  for (const uint8_t reg : {0x7D, 0x7C}) {
   reset();
   CHECK(board.enable_display_power() == ESP_OK);
   const auto saved_power = registers[0x06];
   fail_bmi_write_register = reg;
   fail_bmi_write_after_effect = applied;
   CHECK(board.prepare_l2_sleep() == ESP_FAIL);
   CHECK(registers[0x06] == saved_power && (registers[0x11] & 4));
   CHECK(!bmi_device_active);
  }
 }
 for (const uint8_t reg : {0x7D, 0x7C}) {
  reset();
  CHECK(board.enable_display_power() == ESP_OK);
  ignore_bmi_write_register = reg;
  CHECK(board.prepare_l2_sleep() == ESP_ERR_INVALID_STATE);
  CHECK((registers[0x06] & 6) == 6 && (registers[0x11] & 4));
  CHECK(!bmi_device_active);
 }
 reset();
 CHECK(board.enable_display_power() == ESP_OK);
 bmi_registers[0x00] = 0x00;
 CHECK(board.prepare_l2_sleep() != ESP_OK); // Wrong sensor identity must not receive commands.
 CHECK(bmi_write_count[0x7D] == 0 && bmi_write_count[0x7C] == 0);
 CHECK(!bmi_device_active);
 reset();
 fail_bmi_add = true;
 CHECK(board.prepare_l2_sleep() == ESP_FAIL);
 CHECK(!bmi_device_active);
 reset();
 fail_bmi_remove = true;
 CHECK(board.prepare_l2_sleep() == ESP_FAIL);
 CHECK(bmi_device_active); // Failed removal retains the reachable handle.
 CHECK(board.prepare_l2_sleep() == ESP_OK); // Retry first clears the pending device.
 CHECK(!bmi_device_active);
 reset();
 fail_bmi_remove = true;
 CHECK(board.prepare_l2_sleep() == ESP_FAIL);
 CHECK(bmi_device_active);
 CHECK(board.deinitialize() == ESP_OK); // Retained handle also allows full BSP cleanup.
 CHECK(!bmi_device_active);

 // Successful I2C writes alone are insufficient evidence of a safe sleep state.
 for (const uint8_t reg : {0x06, 0x11, 0x0A, 0x3C}) {
  reset();
  CHECK(board.enable_display_power() == ESP_OK);
  CHECK(board.set_speaker_enabled(true) == ESP_OK);
  registers[0x0A] = 30;
  registers[0x3C] = 0x0E;
  const auto saved_power = registers[0x06];
  const auto saved_outputs = registers[0x11];
  ignore_write_register = reg;
  CHECK(board.prepare_l2_sleep() == ESP_ERR_INVALID_STATE);
  CHECK(registers[0x06] == saved_power);
  CHECK(registers[0x11] == (saved_outputs & ~8));
  CHECK(registers[0x09] == 0);
 }

 reset();
 // Whole PMIC transactions must preserve display and other pins under contention.
 std::atomic<int> active_speakers{0};
 std::thread speaker([&] {
  for (int i = 0; i < 500; ++i) {
   const auto result = board.set_speaker_enabled(true);
   if (result == ESP_OK) {
    ++active_speakers;
    CHECK(!board.ir_receiver_active());
    --active_speakers;
    CHECK(board.set_speaker_enabled(false) == ESP_OK);
   } else CHECK(result == ESP_ERR_INVALID_STATE);
  }
 });
 std::thread receiver([&] {
  for (int i = 0; i < 500; ++i) {
   const auto result = board.acquire_ir_receiver();
   if (result == ESP_OK) {
    CHECK(active_speakers.load() == 0);
    CHECK(board.set_speaker_enabled(true) == ESP_ERR_INVALID_STATE);
    CHECK(board.release_ir_receiver() == ESP_OK);
   } else CHECK(result == ESP_ERR_INVALID_STATE);
  }
 });
 speaker.join(); receiver.join();
 CHECK(board.set_speaker_enabled(false) == ESP_OK);
 CHECK(board.enable_display_power() == ESP_OK);
 CHECK((registers[0x11] & 4) != 0);

 fail_bus_delete = true;
 CHECK(board.deinitialize() == ESP_FAIL);
 CHECK(board.deinitialize() == ESP_OK); // Pending handles are retained.
 CHECK(board.initialize() == ESP_OK);
 CHECK(board.deinitialize() == ESP_OK);
 std::puts("BSP power, L2 rail retention, BMI suspend, failure recovery and concurrent arbitration passed");
}
