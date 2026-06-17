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
 * TX  – uart_write_bytes() → IDF DMA ring buffer → hardware.
 *        Non-blocking.  ZERO extra busy-wait loops.
 *
 * RX  – IDF ISR fills internal RX ring buffer (DMA), posts
 *        UART_DATA to the event queue.  The user task calls
 *        xQueueReceive() on the exposed queue handle, then
 *        reads data via read() or available().
 *
 *        ┌──────────┐  DMA  ┌──────────┐ UART_DATA ┌──────────────┐
 *        │ UART HW  │──────▶│ IDF ISR  │──────────▶│ event queue  │──▶ user task
 *        └──────────┘       └──────────┘           └──────────────┘    xQueueReceive
 *                                                                         │
 *                                                              uart.read() / available()
 *
 * No extra tasks, no emit() needed — the event queue IS the
 * notification mechanism.  ZERO additional FreeRTOS primitives.
 *
 * If a reactor is bound via bindReactor(), signal_RxComplete()
 * can still be used as a polling slot (drains rx_buf_ → callback).
 */
class Esp32UartDmaDriver : public IUartDriver
{
public:
    struct Config
    {
        uart_port_t uart_num         = UART_NUM_1;
        int         tx_pin           = -1;         ///< GPIO_NUM_NC for TX-only
        int         rx_pin           = -1;
        int         baudrate         = 115200;
        size_t      tx_buf_size      = 4096;       ///< IDF DMA TX ring buffer
        size_t      rx_buf_size      = 4096;       ///< IDF DMA RX ring buffer
        int         event_queue_size = 16;          ///< UART event queue depth (0 = no events)
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

    /// ────── Convenience: line-oriented RX ────────────────
    using LineCallback = std::function<void(const char* line)>;

    /**
     * @brief Drain available UART data as a null-terminated string.
     *
     *        Reads directly from the IDF DMA ring buffer into an internal
     *        buffer (zero intermediate copies), appends '\0', and invokes
     *        @p on_line once with the complete payload.
     *
     *        No line-scanning, no accumulation across events — each call
     *        delivers exactly what the hardware has available right now.
     *
     * @return true if data was received and on_line was called.
     */
    bool drainLines(LineCallback on_line);
    void signal_RxComplete(std::function<void(const uint8_t*, size_t)> slot) override;
    void signal_TxComplete(std::function<void()> slot) override;

    /// Expose the IDF UART event queue so the user can xQueueReceive() directly.
    QueueHandle_t event_queue() const { return uart_event_queue_; }

private:
    Config                    cfg_;
    std::atomic<bool>         running_{false};

    /// Lock-free ring buffer for RX data (polling / reactor path)
    static constexpr size_t   RX_BUF_SIZE = 512;
    mutable atomic_array<uint8_t, RX_BUF_SIZE> rx_buf_;

    /// Scratch buffer for drainLines()
    static constexpr size_t   LINE_BUF_SIZE = 256;
    char                      line_buf_[LINE_BUF_SIZE]{};

    /// Native UART event queue (IDF ISR → UART_DATA events)
    QueueHandle_t             uart_event_queue_ = nullptr;
};
