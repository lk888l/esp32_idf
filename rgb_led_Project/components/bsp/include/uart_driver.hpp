#pragma once

/// cpp standard library headers
#include <cstdint>
#include <cstddef>
#include <functional>

#include "BasicObject.hpp"

/**
 * @brief UART driver abstract interface.
 *
 * Inherits BasicObject for FreeRTOS task-notification signal emission.
 * Concrete drivers provide bindReactor() to wire member-function signals
 * into a TaskReactor.  signal_RxComplete / signal_TxComplete act as the
 * slots dispatched by the reactor when the corresponding notification bit
 * is set.
 */
class IUartDriver : public BasicObject
{
public:
    virtual ~IUartDriver() = default;

    /// -------- lifecycle --------

    /** @brief Install IDF UART driver, configure baudrate / data bits / pins, allocate DMA buffers. */
    virtual bool init()   = 0;

    /** @brief Activate driver — set running flag, enable RX event delivery. @pre init() succeeded. */
    virtual bool start()  = 0;

    /** @brief Deactivate and tear down the IDF driver instance.  init() may be called again afterwards. */
    virtual bool stop()   = 0;

    /// -------- TX (non-blocking) --------

    /**
     * @brief Enqueue data for transmission.  Returns immediately — data is
     *        copied into the IDF DMA ring buffer and sent by hardware in the
     *        background.
     * @return Actual bytes enqueued (may be less than @p len if buffer is full;
     *         0 when driver is not running).
     */
    virtual size_t write(const uint8_t* data, size_t len) = 0;

    /** @brief Block until all queued TX data has been transmitted (timeout ~1 s). */
    virtual void   flush() = 0;

    /** @brief Query number of bytes still pending in the TX DMA ring buffer. */
    virtual size_t tx_pending() const = 0;

    /// -------- RX (polling) --------

    /**
     * @brief Drain IDF DMA RX buffer into the internal software ring buffer,
     *        then return the number of bytes available for immediate reading.
     */
    virtual size_t available() const = 0;

    /**
     * @brief Read up to @p max_len bytes.
     * @param timeout_ms  0 = non-blocking (reads from software ring buffer);
     *                    >0 = blocking read from hardware buffer with that timeout.
     * @return Actual bytes copied into @p buf.
     */
    virtual size_t read(uint8_t* buf, size_t max_len, uint32_t timeout_ms = 0) = 0;

    /**
     * @brief Zero-copy read directly from the IDF DMA RX ring buffer.
     *
     * Bypasses the software ring buffer entirely.  Appends '\0' after the
     * last data byte — @p buf must hold at least max_len+1 bytes.
     * @return Data bytes written (excluding null terminator); 0 if no data.
     */
    virtual size_t readAvailable(char* buf, size_t max_len) = 0;

    /// -------- Observer slots (called by TaskReactor) --------

    /**
     * @brief Slot dispatched when UART RX data is ready.
     *
     * Drains all available data and invokes @p slot one or more times
     * (up to 256 bytes per invocation).
     */
    virtual void signal_RxComplete(std::function<void(const uint8_t*, size_t)> slot) = 0;

    /** @brief Slot dispatched when pending TX has completed (best-effort). */
    virtual void signal_TxComplete(std::function<void()> slot) = 0;
};
