#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "app_module.hpp"

namespace app {

class AppManager {
public:
    static AppManager& get_instance();

    bool register_module(std::unique_ptr<AppModule> module);

    bool initialize_all();
    bool deinitialize_all();
    void process_all();

    std::size_t get_module_count() const;

    AppModule* get_module(std::size_t index);
    const AppModule* get_module(std::size_t index) const;

    bool all_initialized() const;
    bool is_started() const { return started_; }

private:
    AppManager() = default;

    std::vector<std::unique_ptr<AppModule>> modules_;
    bool started_ = false;

    AppManager(const AppManager&) = delete;
    AppManager& operator=(const AppManager&) = delete;
};

} // namespace app
