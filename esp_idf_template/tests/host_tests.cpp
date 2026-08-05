#include <array>
#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "app_manager.hpp"

namespace {

class FakeModule final : public AppModule {
public:
    FakeModule(std::string_view name,
               std::vector<std::string>& calls,
               bool initialize_succeeds = true,
               bool cleanup_succeeds = true)
        : name_(name),
          calls_(calls),
          initialize_succeeds_(initialize_succeeds),
          cleanup_succeeds_(cleanup_succeeds)
    {
    }

    std::string_view name() const override { return name_; }

    void process() override
    {
        calls_.emplace_back("process:" + std::string(name_));
    }

private:
    bool on_initialize() override
    {
        calls_.emplace_back("start:" + std::string(name_));
        return initialize_succeeds_;
    }

    bool on_deinitialize() override
    {
        calls_.emplace_back("stop:" + std::string(name_));
        return cleanup_succeeds_;
    }

    std::string_view name_;
    std::vector<std::string>& calls_;
    bool initialize_succeeds_;
    bool cleanup_succeeds_;
};

class RetryCleanupModule final : public AppModule {
public:
    RetryCleanupModule(std::string_view name, std::vector<std::string>& calls)
        : name_(name), calls_(calls)
    {
    }

    std::string_view name() const override { return name_; }

private:
    bool on_initialize() override
    {
        calls_.emplace_back("start:" + std::string(name_));
        return true;
    }

    bool on_deinitialize() override
    {
        calls_.emplace_back("stop:" + std::string(name_));
        return ++cleanup_attempts_ > 1;
    }

    std::string_view name_;
    std::vector<std::string>& calls_;
    std::size_t cleanup_attempts_ = 0;
};

std::unique_ptr<AppModule> fake(std::string_view name,
                                std::vector<std::string>& calls,
                                bool initialize_succeeds = true,
                                bool cleanup_succeeds = true)
{
    return std::make_unique<FakeModule>(
        name, calls, initialize_succeeds, cleanup_succeeds);
}

void test_registration_validation()
{
    app::AppManager manager;
    std::vector<std::string> calls;

    assert(manager.register_module(nullptr).status == app::RegistrationStatus::null_module);
    assert(manager.register_module(fake("module-0", calls)));
    assert(manager.register_module(fake("module-0", calls)).status ==
           app::RegistrationStatus::duplicate_name);

    constexpr std::array names{
        "module-1", "module-2", "module-3", "module-4",
        "module-5", "module-6", "module-7",
    };
    for (const char* name : names) {
        assert(manager.register_module(fake(name, calls)));
    }
    assert(manager.module_count() == app::AppManager::kMaxModules);
    assert(manager.register_module(fake("overflow", calls)).status ==
           app::RegistrationStatus::registry_full);
}

void test_initialization_rollback_order()
{
    app::AppManager manager;
    std::vector<std::string> calls;
    assert(manager.register_module(fake("a", calls)));
    assert(manager.register_module(fake("b", calls)));
    assert(manager.register_module(fake("failure", calls, false)));

    const app::LifecycleResult result = manager.initialize_all();
    assert(result.status == app::LifecycleStatus::module_failed);
    assert(result.module_name == "failure");

    const std::vector<std::string> expected{
        "start:a", "start:b", "start:failure", "stop:failure", "stop:b", "stop:a",
    };
    assert(calls == expected);
    assert(!manager.is_running());
    assert(!manager.has_cleanup_failure());
}

void test_failed_cleanup_preserves_dependencies()
{
    app::AppManager manager;
    std::vector<std::string> calls;
    assert(manager.register_module(fake("dependency", calls)));
    assert(manager.register_module(fake("failure", calls, false, false)));

    const app::LifecycleResult result = manager.initialize_all();
    assert(result.status == app::LifecycleStatus::rollback_failed);
    assert(result.module_name == "failure");
    assert(manager.has_cleanup_failure());

    const std::vector<std::string> expected{
        "start:dependency", "start:failure", "stop:failure",
    };
    assert(calls == expected);
}

void test_processing_and_reverse_stop()
{
    app::AppManager manager;
    std::vector<std::string> calls;
    assert(manager.register_module(fake("a", calls)));
    assert(manager.register_module(fake("b", calls)));
    assert(manager.initialize_all());
    assert(manager.register_module(fake("too-late", calls)).status ==
           app::RegistrationStatus::invalid_state);

    manager.process_all();
    assert(manager.deinitialize_all());
    manager.process_all();

    const std::vector<std::string> expected{
        "start:a", "start:b", "process:a", "process:b", "stop:b", "stop:a",
    };
    assert(calls == expected);
}

void test_cleanup_can_be_retried()
{
    app::AppManager manager;
    std::vector<std::string> calls;
    assert(manager.register_module(fake("dependency", calls)));
    assert(manager.register_module(
        std::make_unique<RetryCleanupModule>("retry", calls)));
    assert(manager.initialize_all());

    assert(manager.deinitialize_all().status == app::LifecycleStatus::module_failed);
    assert(manager.has_cleanup_failure());
    assert(manager.deinitialize_all());
    assert(!manager.has_cleanup_failure());

    const std::vector<std::string> expected{
        "start:dependency", "start:retry", "stop:retry", "stop:retry", "stop:dependency",
    };
    assert(calls == expected);
}

void test_deinitialize_stops_at_failure()
{
    app::AppManager manager;
    std::vector<std::string> calls;
    assert(manager.register_module(fake("dependency", calls)));
    assert(manager.register_module(fake("failure", calls, true, false)));
    assert(manager.register_module(fake("consumer", calls)));
    assert(manager.initialize_all());

    const app::LifecycleResult result = manager.deinitialize_all();
    assert(result.status == app::LifecycleStatus::module_failed);
    assert(result.module_name == "failure");

    const std::vector<std::string> expected{
        "start:dependency", "start:failure", "start:consumer",
        "stop:consumer", "stop:failure",
    };
    assert(calls == expected);
}

} // namespace

int main()
{
    test_registration_validation();
    test_initialization_rollback_order();
    test_failed_cleanup_preserves_dependencies();
    test_processing_and_reverse_stop();
    test_cleanup_can_be_retried();
    test_deinitialize_stops_at_failure();
    std::cout << "all app manager tests passed\n";
    return 0;
}
