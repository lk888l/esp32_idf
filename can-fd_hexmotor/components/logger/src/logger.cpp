#include "logger.hpp"
#include "esp_log.h"

namespace espidf_template {

Logger::Logger(const std::string &tag) : tag_(tag) {}

void Logger::info(const std::string &msg) const {
    ESP_LOGI(tag_.c_str(), "%s", msg.c_str());
}

void Logger::warn(const std::string &msg) const {
    ESP_LOGW(tag_.c_str(), "%s", msg.c_str());
}

} // namespace espidf_template
