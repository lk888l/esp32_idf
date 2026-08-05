#include "app/app_manager.hpp"

namespace app {

RegistrationResult Manager::register_module(std::unique_ptr<Module> module)
{
    if (state_ != State::configuring) {
        return {RegistrationStatus::invalid_state, module ? module->name() : std::string_view{}};
    }
    if (!module) {
        return {RegistrationStatus::null_module, {}};
    }
    if (module_count_ >= modules_.size()) {
        return {RegistrationStatus::registry_full, module->name()};
    }
    const std::string_view name = module->name();
    for (std::size_t index = 0; index < module_count_; ++index) {
        if (modules_[index]->name() == name) {
            return {RegistrationStatus::duplicate_name, name};
        }
    }
    modules_[module_count_++] = std::move(module);
    return {RegistrationStatus::ok, name};
}

LifecycleResult Manager::initialize_all()
{
    if (state_ == State::running) {
        return {};
    }
    if (state_ != State::configuring) {
        return {LifecycleStatus::invalid_state, {}};
    }
    std::size_t initialized = 0;
    for (; initialized < module_count_; ++initialized) {
        if (!modules_[initialized]->initialize()) {
            const std::string_view failed = modules_[initialized]->name();
            while (initialized > 0) {
                if (!modules_[--initialized]->deinitialize()) {
                    state_ = State::faulted;
                    return {LifecycleStatus::rollback_failed, modules_[initialized]->name()};
                }
            }
            return {LifecycleStatus::module_failed, failed};
        }
    }
    state_ = State::running;
    return {};
}

LifecycleResult Manager::deinitialize_all()
{
    if (state_ == State::configuring) {
        return {};
    }
    for (std::size_t remaining = module_count_; remaining > 0; --remaining) {
        if (!modules_[remaining - 1]->deinitialize()) {
            state_ = State::faulted;
            return {LifecycleStatus::module_failed, modules_[remaining - 1]->name()};
        }
    }
    state_ = State::configuring;
    return {};
}

void Manager::process_all()
{
    if (state_ != State::running) {
        return;
    }
    for (std::size_t index = 0; index < module_count_; ++index) {
        modules_[index]->process();
    }
}

} // namespace app

