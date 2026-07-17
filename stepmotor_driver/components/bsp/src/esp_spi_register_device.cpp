#include "esp_spi_register_device.hpp"

#include <array>

namespace bsp {

EspSpiRegisterDevice::EspSpiRegisterDevice(const SpiRegisterDeviceConfig& config) noexcept
    : config_(config)
{
}

EspSpiRegisterDevice::~EspSpiRegisterDevice()
{
    deinitialize();
}

bool EspSpiRegisterDevice::initialize() noexcept
{
    if (initialized_) {
        return true;
    }

    spi_bus_config_t bus_config{};
    bus_config.sclk_io_num = config_.sclk_gpio;
    bus_config.miso_io_num = config_.miso_gpio;
    bus_config.mosi_io_num = config_.mosi_gpio;
    bus_config.quadwp_io_num = GPIO_NUM_NC;
    bus_config.quadhd_io_num = GPIO_NUM_NC;
    bus_config.max_transfer_sz = 2;

    if (spi_bus_initialize(config_.host, &bus_config, SPI_DMA_DISABLED) != ESP_OK) {
        return false;
    }
    bus_initialized_ = true;

    spi_device_interface_config_t device_config{};
    device_config.clock_speed_hz = static_cast<int>(config_.clock_hz);
    device_config.mode = config_.mode;
    device_config.spics_io_num = config_.chip_select_gpio;
    device_config.queue_size = 1;
    device_config.cs_ena_pretrans = 1;
    device_config.cs_ena_posttrans = 1;

    if (spi_bus_add_device(config_.host, &device_config, &device_) != ESP_OK) {
        deinitialize();
        return false;
    }

    initialized_ = true;
    return true;
}

void EspSpiRegisterDevice::deinitialize() noexcept
{
    initialized_ = false;
    if (device_ != nullptr) {
        spi_bus_remove_device(device_);
        device_ = nullptr;
    }
    if (bus_initialized_) {
        spi_bus_free(config_.host);
        bus_initialized_ = false;
    }
}

bool EspSpiRegisterDevice::readRegister(uint8_t register_address, uint8_t& value) noexcept
{
    if (!initialized_) {
        return false;
    }

    const std::array<uint8_t, 2> transmit{
        static_cast<uint8_t>(0x80U | (register_address & 0x7FU)),
        0x00U,
    };
    std::array<uint8_t, 2> receive{};

    spi_transaction_t transaction{};
    transaction.length = 16;
    transaction.rxlength = 16;
    transaction.tx_buffer = transmit.data();
    transaction.rx_buffer = receive.data();

    if (spi_device_polling_transmit(device_, &transaction) != ESP_OK) {
        return false;
    }

    value = receive[1];
    return true;
}

} // namespace bsp
