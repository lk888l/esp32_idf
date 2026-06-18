#pragma once

/// cpp standard library headers
#include <atomic>
#include <functional>

#include "uart_driver.hpp"
#include "atomic_array.hpp"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/**
 * @brief UART driver backed by IDF DMA + native event queue.
 *
 * TX — uart_write_bytes() to IDF DMA ring buffer → hardware.
 *      Non-blocking, zero extra busy-wait loops.
 *
 * RX — IDF ISR fills the internal DMA RX ring buffer and posts UART_DATA
 *      events to the native FreeRTOS event queue.  The user task calls
 *      xQueueReceive() on the exposed queue handle, then reads data via
 *      drainLines() / readAvailable().
 *
 *      ┌──────────┐  DMA  ┌──────────┐ UART_DATA ┌──────────────┐
 *      │ UART HW  │──────▶│ IDF ISR  │──────────▶│ event queue  │──▶ user task
 *      └──────────┘       └──────────┘           └──────────────┘    xQueueReceive
 *                                                                        │
 *                                                           drainLines() / read()
 *
 * No extra FreeRTOS primitives — the IDF event queue IS the notification
 * mechanism.  When bound via bindReactor(), signal_RxComplete() can also
 * be used as a reactor slot (drains rx_buf_ → callback).
 */
class Esp32UartDmaDriver : public IUartDriver
{
public:
    /** Hardware configuration.  All fields have sensible defaults. */
    struct Config
    {
        uart_port_t uart_num         = UART_NUM_1;     ///< UART peripheral number
        int         tx_pin           = -1;             ///< TX GPIO (-1 = unused)
        int         rx_pin           = -1;             ///< RX GPIO (-1 = unused)
        int         baudrate         = 115200;         ///< Baud rate
        size_t      tx_buf_size      = 4096;           ///< IDF DMA TX ring buffer (bytes)
        size_t      rx_buf_size      = 4096;           ///< IDF DMA RX ring buffer (bytes)
        int         event_queue_size = 16;             ///< UART event queue depth (0 = no events)
    };

    explicit Esp32UartDmaDriver(const Config& cfg);
    ~Esp32UartDmaDriver();

    /// -------- IUartDriver overrides --------
    bool   init()   override;
    bool   start()  override;
    bool   stop()   override;

    size_t write(const uint8_t* data, size_t len) override;
    void   flush() override;
    size_t tx_pending() const override;

    size_t available() const override;
    size_t read(uint8_t* buf, size_t max_len, uint32_t timeout_ms = 0) override;
    size_t readAvailable(char* buf, size_t max_len) override;

    // ────────────────────────────────────────────────────────────
    //  Convenience — line-oriented RX
    // ────────────────────────────────────────────────────────────

    /** Callback signature for drainLines(): receives a null-terminated payload. */
    using LineCallback = std::function<void(const char* line)>;

    /**
     * @brief Drain all available UART data as a null-terminated string.
     *
     * Reads directly from the IDF DMA ring buffer via readAvailable()
     * (zero intermediate copies), then invokes @p on_line once with the
     * complete payload.  Call this when a UART_DATA event arrives.
     *
     * @note This delivers the **entire available payload** as one string —
     *       it does NOT split on newlines or accumulate across events.
     *
     * @return true if data was received and on_line was called.
     */
    bool drainLines(LineCallback on_line);

    void signal_RxComplete(std::function<void(const uint8_t*, size_t)> slot) override;
    void signal_TxComplete(std::function<void()> slot) override;

    /**
     * @brief Expose the native IDF UART event queue handle.
     *
     * The caller can block directly on this queue with xQueueReceive()
     * to wait for UART_DATA (and other) events.
     */
    QueueHandle_t event_queue() const { return uart_event_queue_; }

private:
    Config                    cfg_;
    std::atomic<bool>         running_{false};

    /// Lock-free SPSC ring buffer for polling / reactor RX path
    static constexpr size_t   RX_BUF_SIZE = 512;
    mutable atomic_array<uint8_t, RX_BUF_SIZE> rx_buf_;

    /// Scratch buffer for drainLines() — avoids heap allocation on the hot path
    static constexpr size_t   LINE_BUF_SIZE = 256;
    char                      line_buf_[LINE_BUF_SIZE]{};

    /// Native IDF UART event queue (ISR → UART_DATA events)
    QueueHandle_t             uart_event_queue_ = nullptr;
};
