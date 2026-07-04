#pragma once

/// cpp standard library headers
#include <cstdint>
#include <cstddef>

#include "BasicObject.hpp"

/**
 * @brief LED strip driver abstract interface.
 *
 * Inherits BasicObject for FreeRTOS task-notification signal emission.
 * Concrete drivers implement the low-level protocol (e.g. WS2812B over RMT).
 *
 * The pixel data is passed as raw bytes in GRB order (the standard for
 * WS2812B/W2812B), 3 bytes per pixel.  The driver does not interpret the
 * colour information — it only handles the physical transmission.
 */
class ILedStripDriver : public BasicObject
{
public:
    virtual ~ILedStripDriver() = default;

    /// -------- lifecycle --------

    /** @brief Install the hardware peripheral and allocate resources. */
    virtual bool init()  = 0;

    /** @brief Enable the peripheral so that refresh() can send data. @pre init() succeeded. */
    virtual bool start() = 0;

    /** @brief Disable and tear down the peripheral.  init() may be called again afterwards. */
    virtual bool stop()  = 0;

    /// -------- data transmission --------

    /**
     * @brief Transmit pixel data to the LED strip.
     *
     * Encodes @p num_pixels * 3 bytes of GRB data into the wire protocol
     * and sends it out.  Blocks until transmission completes (including the
     * >50 µs reset pulse at the end).
     *
     * @param pixel_data  Pointer to GRB pixel buffer (3 bytes per pixel).
     * @param num_pixels  Number of pixels to transmit.
     * @return true on success.
     */
    virtual bool refresh(const uint8_t* pixel_data, size_t num_pixels) = 0;

    /**
     * @brief Send an all-zero frame to turn off every pixel.
     *
     * Convenience wrapper — allocates a zero-filled buffer of the required
     * size and calls refresh().
     */
    virtual bool clear(size_t num_pixels) = 0;

    /**
     * @brief Send a reset pulse (>50 µs low) without transmitting any pixel data.
     *
     * Useful for protocol-level synchronisation with the strip.
     */
    virtual bool reset() = 0;
};
