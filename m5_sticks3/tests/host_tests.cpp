#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "app_manager.hpp"
#include "button_debouncer.hpp"
#include "button_event_bus.hpp"
#include "mini_games.hpp"

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

    void process() override { calls_.emplace_back("process:" + std::string(name_)); }

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
    size_t cleanup_attempts_ = 0;
};

std::unique_ptr<AppModule> fake(std::string_view name,
                                std::vector<std::string>& calls,
                                bool initialize_succeeds = true,
                                bool cleanup_succeeds = true)
{
    return std::make_unique<FakeModule>(name, calls, initialize_succeeds, cleanup_succeeds);
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

void test_button_debounce_and_tick_wrap()
{
    app::ButtonDebouncer button(app::ButtonId::key1, 30);
    assert(!button.update(true, 1));
    assert(!button.update(false, 5));
    assert(!button.update(true, 10));
    assert(!button.update(true, 39));
    const auto pressed = button.update(true, 40);
    assert(pressed && pressed->id == app::ButtonId::key1);
    assert(pressed->action == app::ButtonAction::pressed);

    assert(!button.update(false, 50));
    assert(!button.update(false, 79));
    const auto released = button.update(false, 80);
    assert(released && released->action == app::ButtonAction::released);

    app::ButtonDebouncer wrapping(app::ButtonId::key2, 30);
    constexpr TickType_t near_wrap = UINT32_MAX - 10;
    assert(!wrapping.update(true, near_wrap));
    const auto wrapped = wrapping.update(true, 25);
    assert(wrapped && wrapped->action == app::ButtonAction::pressed);
}

struct EventRecorder {
    std::array<app::ButtonEvent, 64> events{};
    size_t count = 0;
};

void record_event(void* context, const app::ButtonEvent& event)
{
    auto& recorder = *static_cast<EventRecorder*>(context);
    assert(recorder.count < recorder.events.size());
    recorder.events[recorder.count++] = event;
}

void test_event_bus_subscription_and_fifo()
{
    app::ButtonEventBus bus;
    EventRecorder first;
    EventRecorder second;
    const auto first_subscription = bus.subscribe(record_event, &first);
    const auto second_subscription = bus.subscribe(record_event, &second);
    assert(first_subscription.valid());
    assert(second_subscription.valid());

    const app::ButtonEvent pressed{
        .id = app::ButtonId::key1,
        .action = app::ButtonAction::pressed,
        .timestamp = 10,
    };
    assert(bus.publish(pressed));
    assert(bus.dispatch_pending() == 1);
    assert(first.count == 1 && second.count == 1);

    assert(bus.unsubscribe(first_subscription));
    assert(!bus.unsubscribe(first_subscription));
    const app::ButtonEvent released{
        .id = app::ButtonId::key1,
        .action = app::ButtonAction::released,
        .timestamp = 20,
    };
    assert(bus.publish(released));
    assert(bus.dispatch_pending() == 1);
    assert(first.count == 1 && second.count == 2);
    assert(second.events[0].timestamp == 10);
    assert(second.events[1].timestamp == 20);
}

void test_event_bus_capacity_and_overflow()
{
    app::ButtonEventBus bus;
    EventRecorder recorder;
    std::array<app::ButtonEventBus::Subscription,
               app::ButtonEventBus::kSubscriberCapacity> subscriptions{};
    for (auto& subscription : subscriptions) {
        subscription = bus.subscribe(record_event, &recorder);
        assert(subscription.valid());
    }
    assert(!bus.subscribe(record_event, &recorder).valid());

    for (size_t index = 0; index < app::ButtonEventBus::kQueueCapacity; ++index) {
        assert(bus.publish({
            .id = app::ButtonId::key2,
            .action = app::ButtonAction::released,
            .timestamp = static_cast<TickType_t>(index),
        }));
    }
    assert(!bus.publish({
        .id = app::ButtonId::key2,
        .action = app::ButtonAction::released,
        .timestamp = 99,
    }));
    assert(bus.dropped_events() == 1);
    assert(bus.dispatch_pending() == app::ButtonEventBus::kQueueCapacity);

    assert(recorder.count == app::ButtonEventBus::kQueueCapacity * subscriptions.size());
    for (size_t index = 0; index < app::ButtonEventBus::kQueueCapacity; ++index) {
        assert(recorder.events[index * subscriptions.size()].timestamp == index);
    }
}

void test_game_metadata()
{
    assert(std::string_view(mini_games::name(mini_games::GameId::tilt_quest)) ==
           "TILT QUEST");
    assert(mini_games::requires_motion(mini_games::GameId::tilt_quest));
    assert(mini_games::requires_motion(mini_games::GameId::meteor_dodge));
    assert(!mini_games::requires_motion(mini_games::GameId::tap_runner));
}

void test_tilt_quest_physics_and_timeout()
{
    mini_games::TiltQuest game;
    game.reset(7);
    assert(game.state() == mini_games::RunState::playing);
    assert(game.score() == 0);
    assert(game.walls().size() == mini_games::TiltQuest::kWallCount);

    game.update(10.0f, 0.0f, 0.0f);
    assert(game.seconds_remaining() > 44.9f);
    for (size_t step = 0; step < 400; ++step) {
        game.update(0.02f, -1.0f, 1.0f);
        assert(game.ball().x >= mini_games::TiltQuest::kBallRadius);
        assert(game.ball().x <=
               mini_games::kFieldWidth - mini_games::TiltQuest::kBallRadius);
        assert(game.ball().y >= mini_games::TiltQuest::kBallRadius);
        assert(game.ball().y <=
               mini_games::kFieldHeight - mini_games::TiltQuest::kBallRadius);
    }
    for (size_t step = 0;
         step < 2400 && game.state() == mini_games::RunState::playing; ++step) {
        game.update(0.02f, 0.0f, 0.0f);
    }
    assert(game.state() == mini_games::RunState::game_over);
    assert(game.seconds_remaining() == 0.0f);
}

void test_meteor_shield_and_steering_bounds()
{
    mini_games::MeteorDodge game;
    game.reset(42);
    assert(game.state() == mini_games::RunState::playing);
    assert(game.shield_ready());
    assert(game.activate_shield());
    assert(!game.activate_shield());
    for (size_t step = 0; step < 16; ++step) {
        game.update(0.05f, 1.0f);
        assert(game.player_x() >= mini_games::MeteorDodge::kPlayerRadius);
        assert(game.player_x() <=
               mini_games::kFieldWidth - mini_games::MeteorDodge::kPlayerRadius);
    }
    assert(!game.shield_active());
    assert(!game.shield_ready());
}

void test_tap_runner_jump_cycle()
{
    mini_games::TapRunner game;
    game.reset(99);
    const float ground_y =
        mini_games::TapRunner::kGroundY - mini_games::TapRunner::kPlayerSize;
    assert(game.on_ground());
    assert(game.player_y() == ground_y);
    assert(game.jump());
    assert(!game.jump());
    game.update(0.05f);
    assert(game.player_y() < ground_y);
    for (size_t step = 0; step < 24; ++step) {
        game.update(0.05f);
    }
    assert(game.state() == mini_games::RunState::playing);
    assert(game.on_ground());
    assert(game.player_y() == ground_y);
    assert(game.jump());
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
    test_button_debounce_and_tick_wrap();
    test_event_bus_subscription_and_fifo();
    test_event_bus_capacity_and_overflow();
    test_game_metadata();
    test_tilt_quest_physics_and_timeout();
    test_meteor_shield_and_steering_bounds();
    test_tap_runner_jump_cycle();
    std::cout << "All host tests passed\n";
    return 0;
}
