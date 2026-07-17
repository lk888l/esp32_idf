#pragma once

#include <utility>

#include "esp_log.h"

namespace logger {

class Logger final {
public:
    explicit constexpr Logger(const char* tag) noexcept
        : tag_(tag)
    {
    }

    template <typename... Args>
    void error(const char* format, Args&&... args) const
    {
        esp_log_write(ESP_LOG_ERROR, tag_, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warn(const char* format, Args&&... args) const
    {
        esp_log_write(ESP_LOG_WARN, tag_, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(const char* format, Args&&... args) const
    {
        esp_log_write(ESP_LOG_INFO, tag_, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void debug(const char* format, Args&&... args) const
    {
        esp_log_write(ESP_LOG_DEBUG, tag_, format, std::forward<Args>(args)...);
    }

private:
    const char* tag_;
};

} // namespace logger
