#pragma once

/// cpp standard library headers
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <vector>

#include "led_strip_driver.hpp"
#include "driver/rmt_tx.h"

/**
 * @brief LED strip driver using the ESP32 RMT (Remote Control) peripheral.
 *
 * Encodes pixel data into the WS2812B / W2812B one-wire NZR protocol:
 *   - 0-bit : short high + long low
 *   - 1-bit : long high  + short low
 *   - Reset : >50 µs low period between frames
 *
 * The RMT copy-encoder is used with a pre-computed symbol buffer — one
 * rmt_symbol_word_t per bit (2 per symbol edge-pair).  For N pixels this
 * produces N × 3 × 8 + 1 symbols.
 *
 * Typical usage:
 * @code
 *   Esp32RmtLedStripDriver::Config cfg;
 *   cfg.gpio_num = 8;
 *   cfg.max_leds = 16;
 *   Esp32RmtLedStripDriver driver(cfg);
 *   driver.init();
 *   driver.start();
 *   driver.refresh(pixel_data, 16);
 * @endcode
 */
class Esp32RmtLedStripDriver : public ILedStripDriver
{
public:
    /** Hardware configuration — all fields have sensible defaults for WS2812B. */
    struct Config
    {
        int      gpio_num       = -1;             ///< Data output GPIO (required)
        size_t   max_leds       = 64;             ///< Maximum LEDs in the strip (reserves encode buffer)
        uint32_t resolution_hz  = 10'000'000;     ///< RMT clock resolution (10 MHz → 0.1 µs / tick)

        /// Timing parameters in RMT ticks at the configured resolution.
        /// Defaults match WS2812B at 10 MHz:
        ///   0-bit:  0.4 µs high + 0.9 µs low
        ///   1-bit:  0.8 µs high + 0.5 µs low
        ///   Reset: 80 µs low
        uint32_t t0h_ticks = 4;                   ///< 0-bit high duration (400 ns)
        uint32_t t0l_ticks = 9;                   ///< 0-bit low  duration (900 ns)
        uint32_t t1h_ticks = 8;                   ///< 1-bit high duration (800 ns)
        uint32_t t1l_ticks = 5;                   ///< 1-bit low  duration (500 ns)

        /** Reset pulse low time in ticks (default 800 → 80 µs at 10 MHz). */
        uint32_t reset_ticks = 800;               ///< >50 µs required by protocol
    };

    explicit Esp32RmtLedStripDriver(const Config& cfg);
    ~Esp32RmtLedStripDriver();

    /// -------- ILedStripDriver overrides --------
    bool init()   override;
    bool start()  override;
    bool stop()   override;

    bool refresh(const uint8_t* pixel_data, size_t num_pixels) override;
    bool clear(size_t num_pixels) override;
    bool reset()  override;

private:
    /** Build the per-byte encode table — called once in init(). */
    void buildEncodeTable();

    Config                    cfg_;
    std::atomic<bool>         running_{false};

    /// RMT hardware handles
    rmt_channel_handle_t      tx_channel_ = nullptr;
    rmt_encoder_handle_t      copy_encoder_ = nullptr;

    /// Pre-computed RMT symbols for all 256 byte values (MSB-first).
    /// encode_table_[byte][bit] → one rmt_symbol_word_t (8 entries per byte).
    rmt_symbol_word_t         encode_table_[256][8];

    /// Scratch buffer for encoding — max_leds × 3 bytes × 8 symbols + 1 reset.
    std::vector<rmt_symbol_word_t> symbol_buf_;

    /// RMT transmit configuration
    rmt_transmit_config_t     tx_config_ = {};
};
