#pragma once

/// cpp standard library headers
#include <cstdint>
#include <cstddef>
#include <functional>

#include "BasicObject.hpp"

/**
 * @brief UART driver abstract interface.
 *
 * Follows the same observer / reactor pattern as ICanDriver:
 *   - Inherits BasicObject for signal emission (emit / emitFromISR).
 *   - Concrete driver provides bindReactor() template.
 *   - signal_RxComplete acts as the slot dispatched by TaskReactor.
 */
class IUartDriver : public BasicObject
{
public:
    virtual ~IUartDriver() = default;

    /// -------- lifecycle --------
    virtual bool init()   = 0;
    virtual bool start()  = 0;
    virtual bool stop()   = 0;

    /// -------- TX --------
    /** Non-blocking write. Returns actual bytes queued. */
    virtual size_t write(const uint8_t* data, size_t len) = 0;
    /** Block until all TX data has been pushed out by DMA/ISR. */
    virtual void   flush() = 0;
    /** Return number of bytes still pending in the TX ring buffer. */
    virtual size_t tx_pending() const = 0;

    /// -------- RX (polling) --------
    /** Bytes available in the RX ring buffer. */
    virtual size_t available() const = 0;
    /** Read up to max_len bytes; returns actual bytes copied. */
    virtual size_t read(uint8_t* buf, size_t max_len, uint32_t timeout_ms = 0) = 0;

    /**
     * @brief Read available data directly from hardware RX buffer into a
     *        string buffer.  Appends '\0' after the last byte written.
     *
     *        ZERO intermediate copies — bypasses the software ring buffer
     *        and reads straight from the IDF DMA ring buffer.
     *
     * @param buf      Destination buffer (must hold at least max_len+1 bytes).
     * @param max_len  Maximum number of data bytes to read.
     * @return         Number of data bytes written (excluding null terminator).
     *                 0 if no data available.
     */
    virtual size_t readAvailable(char* buf, size_t max_len) = 0;

    /// -------- Observer slots (called by TaskReactor) --------
    /**
     * @brief Slot dispatched when UART RX data is ready.
     * @param slot  User callback (data ptr, length).
     */
    virtual void signal_RxComplete(std::function<void(const uint8_t*, size_t)> slot) = 0;

    /**
     * @brief Slot dispatched when all pending TX has completed (optional).
     */
    virtual void signal_TxComplete(std::function<void()> slot) = 0;
};
