#include "uart_dma_driver.hpp"
#include "driver/uart.h"
#include "esp_log.h"
#include <algorithm>

static const char* DRV_TAG = "UART_DMA";

// ============================================================================
//  Lifecycle
// ============================================================================

Esp32UartDmaDriver::Esp32UartDmaDriver(const Config& cfg)
    : cfg_(cfg)
{
}

Esp32UartDmaDriver::~Esp32UartDmaDriver()
{
    stop();
}

bool Esp32UartDmaDriver::init()
{
    // --- 1. Install driver, allocate DMA buffers, create event queue ---
    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate  = cfg_.baudrate;
    uart_cfg.data_bits  = UART_DATA_8_BITS;
    uart_cfg.parity     = UART_PARITY_DISABLE;
    uart_cfg.stop_bits  = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(cfg_.uart_num,
                                        cfg_.rx_buf_size,
                                        cfg_.tx_buf_size,
                                        cfg_.event_queue_size,
                                        &uart_event_queue_,
                                        0);
    if (err != ESP_OK) {
        ESP_LOGE(DRV_TAG, "uart_driver_install(%d) failed: %s",
                 cfg_.uart_num, esp_err_to_name(err));
        return false;
    }

    // --- 2. Apply baud rate / data format ---
    err = uart_param_config(cfg_.uart_num, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(DRV_TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        uart_driver_delete(cfg_.uart_num);
        uart_event_queue_ = nullptr;
        return false;
    }

    // --- 3. Assign TX / RX GPIO pins ---
    err = uart_set_pin(cfg_.uart_num,
                       cfg_.tx_pin,
                       cfg_.rx_pin,
                       UART_PIN_NO_CHANGE,   // RTS unchanged
                       UART_PIN_NO_CHANGE);  // CTS unchanged
    if (err != ESP_OK) {
        ESP_LOGE(DRV_TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        uart_driver_delete(cfg_.uart_num);
        uart_event_queue_ = nullptr;
        return false;
    }

    return true;
}

bool Esp32UartDmaDriver::start()
{
    running_ = true;
    return true;
}

bool Esp32UartDmaDriver::stop()
{
    running_ = false;
    uart_driver_delete(cfg_.uart_num);
    uart_event_queue_ = nullptr;
    return true;
}

// ============================================================================
//  TX — IDF DMA path (non-blocking)
// ============================================================================

size_t Esp32UartDmaDriver::write(const uint8_t* data, size_t len)
{
    if (!running_) return 0;

    // uart_write_bytes copies data into the IDF DMA TX ring buffer and
    // returns immediately — actual transmission happens in the background.
    int n = uart_write_bytes(cfg_.uart_num, data, static_cast<int>(len));
    return (n > 0) ? static_cast<size_t>(n) : 0;
}

void Esp32UartDmaDriver::flush()
{
    // Block until the hardware TX FIFO and DMA buffer are both empty.
    uart_wait_tx_done(cfg_.uart_num, pdMS_TO_TICKS(1000));
}

size_t Esp32UartDmaDriver::tx_pending() const
{
    size_t pending = 0;
    uart_get_buffered_data_len(cfg_.uart_num, &pending);
    return pending;
}

// ============================================================================
//  RX — polling helpers
// ============================================================================

size_t Esp32UartDmaDriver::available() const
{
    auto* self = const_cast<Esp32UartDmaDriver*>(this);

    // Drain the IDF DMA RX ring buffer into our software ring buffer.
    size_t idf_avail = 0;
    uart_get_buffered_data_len(cfg_.uart_num, &idf_avail);

    if (idf_avail > 0) {
        uint8_t buf[64];
        size_t total = 0;
        while (total < idf_avail) {
            size_t chunk = std::min(idf_avail - total, sizeof(buf));
            int n = uart_read_bytes(cfg_.uart_num, buf, chunk, 0);
            if (n <= 0) break;
            for (int i = 0; i < n; ++i) {
                self->rx_buf_.push(buf[i]);
            }
            total += static_cast<size_t>(n);
        }
    }

    return rx_buf_.size();
}

size_t Esp32UartDmaDriver::read(uint8_t* buf, size_t max_len, uint32_t timeout_ms)
{
    // Blocking path — read directly from the hardware buffer.
    if (timeout_ms > 0) {
        int n = uart_read_bytes(cfg_.uart_num, buf, max_len,
                                pdMS_TO_TICKS(timeout_ms));
        return (n > 0) ? static_cast<size_t>(n) : 0;
    }

    // Non-blocking path — pop bytes from the software ring buffer.
    size_t count = 0;
    while (count < max_len) {
        uint8_t byte;
        if (!rx_buf_.pop(byte)) break;
        buf[count++] = byte;
    }
    return count;
}

// ============================================================================
//  RX — zero-copy string read (bypasses software ring buffer)
// ============================================================================

size_t Esp32UartDmaDriver::readAvailable(char* buf, size_t max_len)
{
    // Read directly from the IDF DMA ring buffer — no intermediate copy.
    int n = uart_read_bytes(cfg_.uart_num,
                            reinterpret_cast<uint8_t*>(buf),
                            max_len, 0);
    if (n > 0) {
        buf[n] = '\0';
        return static_cast<size_t>(n);
    }
    return 0;
}

// ============================================================================
//  RX — drain as null-terminated string (zero-copy convenience)
// ============================================================================

bool Esp32UartDmaDriver::drainLines(LineCallback on_line)
{
    // Read whatever the hardware has available right now into line_buf_.
    size_t n = readAvailable(line_buf_, LINE_BUF_SIZE - 1);
    if (n == 0) return false;

    if (on_line) {
        on_line(line_buf_);   // already '\0'-terminated by readAvailable()
    }
    return true;
}

// ============================================================================
//  Observer slots
// ============================================================================

void Esp32UartDmaDriver::signal_RxComplete(
    std::function<void(const uint8_t*, size_t)> slot)
{
    // First, drain hardware buffer into the software ring buffer.
    available();

    // Then pop all available data in 256-byte chunks and invoke the slot.
    uint8_t buf[256];
    for (;;) {
        size_t n = read(buf, sizeof(buf), 0);
        if (n == 0) break;
        slot(buf, n);
    }
}

void Esp32UartDmaDriver::signal_TxComplete(std::function<void()> slot)
{
    // Best-effort: invoke immediately.  A full implementation would wait
    // for the UART_TX_DONE event from the IDF event queue.
    if (slot) slot();
}
