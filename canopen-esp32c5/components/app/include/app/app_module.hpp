#pragma once

#include <string_view>

namespace app {

class Module {
public:
    virtual ~Module() = default;
    virtual bool initialize() = 0;
    virtual bool deinitialize() = 0;
    virtual void process() {}
    [[nodiscard]] virtual bool initialized() const = 0;
    [[nodiscard]] virtual std::string_view name() const = 0;
};

} // namespace app

