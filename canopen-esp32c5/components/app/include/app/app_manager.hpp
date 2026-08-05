#pragma once

#include "app/app_module.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>

namespace app {

enum class RegistrationStatus { ok, null_module, duplicate_name, registry_full, invalid_state };
struct RegistrationResult {
    RegistrationStatus status = RegistrationStatus::ok;
    std::string_view module_name{};
    explicit operator bool() const { return status == RegistrationStatus::ok; }
};

enum class LifecycleStatus { ok, invalid_state, module_failed, rollback_failed };
struct LifecycleResult {
    LifecycleStatus status = LifecycleStatus::ok;
    std::string_view module_name{};
    explicit operator bool() const { return status == LifecycleStatus::ok; }
};

class Manager {
public:
    static constexpr std::size_t kMaxModules = 8;

    RegistrationResult register_module(std::unique_ptr<Module> module);
    LifecycleResult initialize_all();
    LifecycleResult deinitialize_all();
    void process_all();
    [[nodiscard]] bool running() const { return state_ == State::running; }

private:
    enum class State { configuring, running, faulted };
    std::array<std::unique_ptr<Module>, kMaxModules> modules_{};
    std::size_t module_count_ = 0;
    State state_ = State::configuring;
};

} // namespace app

