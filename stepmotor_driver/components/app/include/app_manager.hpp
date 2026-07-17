#pragma once

#include <array>
#include <cstddef>

#include "app_module.hpp"
#include "noncopyable.hpp"

namespace app {

class AppManager final : private base::NonCopyable {
public:
    static constexpr std::size_t kMaxModules = 4;

    static AppManager& instance() noexcept;

    bool registerModule(AppModule& module) noexcept;
    bool initializeAll();
    bool deinitializeAll();
    void processAll();

    std::size_t moduleCount() const noexcept { return module_count_; }
    bool isStarted() const noexcept { return started_; }

private:
    AppManager() = default;

    std::array<AppModule*, kMaxModules> modules_{};
    std::size_t module_count_ = 0;
    bool started_ = false;
};

} // namespace app
