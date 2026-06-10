// logger.hpp - simple C++ wrapper around esp_log
#pragma once

#include <string>

namespace espidf_template {

class Logger {
public:
    explicit Logger(const std::string &tag);
    void info(const std::string &msg) const;
    void warn(const std::string &msg) const;
private:
    std::string tag_;
};

} // namespace espidf_template
