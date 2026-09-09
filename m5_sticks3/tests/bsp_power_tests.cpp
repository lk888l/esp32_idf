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
int fail_read_register = -1;
int fail_write_register = -1;
bool fail_write_after_effect = false;
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
 registers.fill(0);
 registers[0x06] = static_cast<uint8_t>(0x17 | (boosted ? boost : 0));
 registers[0x16] = 0xFF; // Every pin begins with a non-default function.
 voltage(0x22, 3900); voltage(0x24, 0); voltage(0x26, external_mv);
 CHECK(board.initialize() == ESP_OK);
}
} // namespace

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t*, i2c_master_bus_handle_t* out) {
 *out = registers.data(); return ESP_OK;
}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t*, i2c_master_dev_handle_t* out) {
 *out = registers.data(); return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t) { return ESP_OK; }
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t) {
 if (fail_bus_delete) { fail_bus_delete = false; return ESP_FAIL; }
 return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t, const uint8_t* in,
                                    size_t in_size, uint8_t* out, size_t out_size, int) {
 CHECK(in_size == 1);
 ++transaction_count;
 if (in[0] == fail_read_register) { fail_read_register = -1; return ESP_FAIL; }
 CHECK(in[0] + out_size <= registers.size());
 for (size_t i = 0; i < out_size; ++i) out[i] = registers[in[0] + i];
 std::this_thread::yield(); // Amplify read/modify/write interleavings.
 return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t, const uint8_t* in, size_t size, int) {
 CHECK(size == 2);
 ++transaction_count;
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
 reset();
 CHECK((registers[0x16] & 0xC0) == 0); // GPIO3's actual TWO-bit field.
 CHECK((registers[0x16] & 0x3F) == 0x3F);
 CHECK((registers[0x11] & 8) == 0);
 CHECK(board.enable_display_power() == ESP_OK);
 CHECK(registers[0x16] == 0x0F);
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

 reset(5000); // Externally powered on Grove/HAT; never enable boost.
 CHECK(board.acquire_ir_power() == ESP_OK);
 CHECK(!(registers[0x06] & boost));
 bsp::PowerStatus power{};
 CHECK(board.read_power_status(power) == ESP_OK);
 CHECK(power.externally_powered());
 CHECK(power.battery_mv == 3900);
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
 std::puts("BSP power, pin isolation, failure recovery and concurrent arbitration passed");
}
