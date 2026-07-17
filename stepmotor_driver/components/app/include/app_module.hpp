#pragma once

#include <string_view>

#include "noncopyable.hpp"

namespace app {

class AppModule : private base::NonCopyable {
public:
    virtual ~AppModule() = default;

    virtual bool initialize() = 0;
    virtual bool deinitialize() = 0;
    virtual bool isInitialized() const noexcept = 0;
    virtual std::string_view name() const noexcept = 0;
    virtual void process() {}

protected:
    AppModule() = default;
};

} // namespace app
