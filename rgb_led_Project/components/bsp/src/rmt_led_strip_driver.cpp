#include "rmt_led_strip_driver.hpp"
#include "esp_log.h"
#include <cstring>

static const char* DRV_TAG = "RMT_LED";

// ============================================================================
//  Construction / Destruction
// ============================================================================

Esp32RmtLedStripDriver::Esp32RmtLedStripDriver(const Config& cfg)
    : cfg_(cfg)
{
}

Esp32RmtLedStripDriver::~Esp32RmtLedStripDriver()
{
    stop();
}

// ============================================================================
//  Lifecycle
// ============================================================================

bool Esp32RmtLedStripDriver::init()
{
    // --- 1. Build the per-byte encode table ---
    buildEncodeTable();

    // --- 2. Allocate the symbol scratch buffer ---
    // Each pixel = 3 bytes; each byte = 8 RMT symbols; +1 for reset
    size_t symbol_count = cfg_.max_leds * 3 * 8 + 1;
    symbol_buf_.resize(symbol_count);

    // --- 3. Create the RMT TX channel ---
    rmt_tx_channel_config_t tx_chan_cfg = {};
    tx_chan_cfg.gpio_num       = static_cast<gpio_num_t>(cfg_.gpio_num);
    tx_chan_cfg.clk_src        = RMT_CLK_SRC_DEFAULT;
    tx_chan_cfg.resolution_hz  = cfg_.resolution_hz;
    // Allocate one RMT memory block (64 symbols) — the copy encoder
    // streams larger buffers in chunks, so one block is sufficient.
    tx_chan_cfg.mem_block_symbols = 64;
    // Increase the number of transactions that can be queued
    tx_chan_cfg.trans_queue_depth = 4;

    esp_err_t err = rmt_new_tx_channel(&tx_chan_cfg, &tx_channel_);
    if (err != ESP_OK) {
        ESP_LOGE(DRV_TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        return false;
    }

    // --- 4. Create the copy encoder ---
    rmt_copy_encoder_config_t copy_cfg = {};
    err = rmt_new_copy_encoder(&copy_cfg, &copy_encoder_);
    if (err != ESP_OK) {
        ESP_LOGE(DRV_TAG, "rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
        rmt_del_channel(tx_channel_);
        tx_channel_ = nullptr;
        return false;
    }

    // --- 5. Configure the transmit defaults ---
    tx_config_.loop_count = 0;   // single-shot

    return true;
}

bool Esp32RmtLedStripDriver::start()
{
    if (!tx_channel_) return false;

    esp_err_t err = rmt_enable(tx_channel_);
    if (err != ESP_OK) {
        ESP_LOGE(DRV_TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        return false;
    }
    running_ = true;
    return true;
}

bool Esp32RmtLedStripDriver::stop()
{
    running_ = false;

    if (tx_channel_) {
        rmt_disable(tx_channel_);
    }
    if (copy_encoder_) {
        rmt_del_encoder(copy_encoder_);
        copy_encoder_ = nullptr;
    }
    if (tx_channel_) {
        rmt_del_channel(tx_channel_);
        tx_channel_ = nullptr;
    }
    return true;
}

// ============================================================================
//  Encode table — pre-compute RMT symbols for each byte value
// ============================================================================

void Esp32RmtLedStripDriver::buildEncodeTable()
{
    // RMT symbol for bit 0: short high → long low
    const rmt_symbol_word_t bit0 = {
        .duration0 = static_cast<uint16_t>(cfg_.t0h_ticks),
        .level0    = 1,
        .duration1 = static_cast<uint16_t>(cfg_.t0l_ticks),
        .level1    = 0,
    };

    // RMT symbol for bit 1: long high → short low
    const rmt_symbol_word_t bit1 = {
        .duration0 = static_cast<uint16_t>(cfg_.t1h_ticks),
        .level0    = 1,
        .duration1 = static_cast<uint16_t>(cfg_.t1l_ticks),
        .level1    = 0,
    };

    // Fill the table: encode_table_[byte_val][bit_index] → rmt_symbol_word_t
    // Bits are transmitted MSB first (WS2812B protocol requirement).
    for (unsigned byte_val = 0; byte_val < 256; ++byte_val) {
        for (int bit = 0; bit < 8; ++bit) {
            // MSB first: bit 7 → index 0, bit 0 → index 7
            bool is_one = (byte_val >> (7 - bit)) & 1;
            encode_table_[byte_val][bit] = is_one ? bit1 : bit0;
        }
    }
}

// ============================================================================
//  Data transmission
// ============================================================================

bool Esp32RmtLedStripDriver::refresh(const uint8_t* pixel_data, size_t num_pixels)
{
    if (!running_ || !tx_channel_ || !copy_encoder_) return false;
    if (num_pixels == 0 || num_pixels > cfg_.max_leds) return false;

    const size_t bytes       = num_pixels * 3;          // GRB bytes
    const size_t symbols     = bytes * 8;               // 8 RMT symbols per byte
    const size_t total_syms  = symbols + 1;             // +1 for reset pulse

    // --- Encode pixel data into the scratch buffer ---
    for (size_t i = 0; i < bytes; ++i) {
        uint8_t byte_val = pixel_data[i];
        std::memcpy(&symbol_buf_[i * 8], encode_table_[byte_val], sizeof(encode_table_[byte_val]));
    }

    // --- Append the reset symbol (long low period) ---
    symbol_buf_[symbols] = {
        .duration0 = static_cast<uint16_t>(cfg_.reset_ticks),
        .level0    = 0,
        .duration1 = 0,
        .level1    = 0,
    };

    // --- Transmit ---
    esp_err_t err = rmt_transmit(tx_channel_,
                                  copy_encoder_,
                                  symbol_buf_.data(),
                                  total_syms * sizeof(rmt_symbol_word_t),
                                  &tx_config_);
    if (err != ESP_OK) {
        ESP_LOGE(DRV_TAG, "rmt_transmit failed: %s", esp_err_to_name(err));
        return false;
    }

    // --- Block until the frame + reset pulse have been sent ---
    err = rmt_tx_wait_all_done(tx_channel_, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGW(DRV_TAG, "rmt_tx_wait_all_done timeout/error: %s", esp_err_to_name(err));
    }

    return true;
}

bool Esp32RmtLedStripDriver::clear(size_t num_pixels)
{
    if (num_pixels > cfg_.max_leds) return false;

    // Build a zero-filled buffer and transmit it.
    std::vector<uint8_t> zeros(num_pixels * 3, 0);
    return refresh(zeros.data(), num_pixels);
}

bool Esp32RmtLedStripDriver::reset()
{
    if (!running_ || !tx_channel_ || !copy_encoder_) return false;

    // A single long-low symbol acts as the reset pulse.
    const rmt_symbol_word_t reset_sym = {
        .duration0 = static_cast<uint16_t>(cfg_.reset_ticks),
        .level0    = 0,
        .duration1 = 0,
        .level1    = 0,
    };

    esp_err_t err = rmt_transmit(tx_channel_,
                                  copy_encoder_,
                                  &reset_sym,
                                  sizeof(reset_sym),
                                  &tx_config_);
    if (err != ESP_OK) return false;

    rmt_tx_wait_all_done(tx_channel_, pdMS_TO_TICKS(100));
    return true;
}
