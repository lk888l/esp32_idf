#include "clock_service.hpp"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>

namespace {
time_t wall_time = 0;
void check(bool value, const char* message)
{
    if (!value) { std::cerr << "Clock: " << message << '\n'; std::exit(1); }
}
void receive(time_t epoch)
{
    wall_time = epoch;
    timeval value{epoch, 0};
    fake::sntp_config.sync_cb(&value);
}
} // namespace
extern "C" time_t __wrap_time(time_t* output)
{
    if (output) *output = wall_time;
    return wall_time;
}

int main()
{
    using namespace clock_service;
    check(request_sync() == ESP_ERR_INVALID_STATE, "cannot request before initialization");
    Service service;
    check(service.initialize() == ESP_OK && service.initialize() == ESP_OK, "initialize is idempotent");
    Service second;
    check(second.initialize() == ESP_ERR_INVALID_STATE, "SNTP has one owner");
    wall_time = 1791475200; // Host wall clock alone is not a successful device sync.
    service.process(false);
    check(!snapshot().valid && !std::strcmp(snapshot().time, "--:--:--"), "unsynced time is a placeholder");
    check(fake::sntp_inits == 0 && request_sync() == ESP_ERR_INVALID_STATE, "AP/offline never starts SNTP");
    service.process(true);
    check(fake::sntp_inits == 1 && fake::sntp_starts == 1, "station IP starts once");
    check(!fake::sntp_config.start && !fake::sntp_config.wait_for_sync &&
          fake::sntp_config.num_of_servers == 2 &&
          !std::strcmp(fake::sntp_config.servers[1], "pool.ntp.org"), "nonblocking primary and backup config");
    for (int i = 0; i < 100; ++i) { check(request_sync() == ESP_OK, "in-flight requests coalesce"); service.process(true); }
    check(fake::sntp_starts == 1, "repeated buttons do not reset server fallback");
    fake::now += 60'000'000;
    service.process(true);
    check(snapshot().state == State::retrying && snapshot().error == ESP_ERR_TIMEOUT &&
          fake::sntp_stops == 0, "timeout keeps native retries running");
    receive(1704124799); // 2024-01-01 15:59:59 UTC -> 23:59:59 China.
    auto value = snapshot();
    check(value.valid && value.synced && value.sync_count == 1 &&
          !std::strcmp(value.time, "23:59:59") && !std::strcmp(value.date, "2024/01/01") &&
          !std::strcmp(value.weekday, "Mon"), "POSIX timezone and date before midnight");
    ++wall_time; fake::now += 1'000'000;
    check(!std::strcmp(snapshot().time, "00:00:00") && !std::strcmp(snapshot().date, "2024/01/02"), "midnight rollover");
    service.process(false);
    check(!fake::sntp_active && snapshot().valid && !snapshot().online &&
          request_sync() == ESP_ERR_INVALID_STATE, "offline keeps valid system time");
    wall_time += 30; fake::now += 30'000'000;
    check(!std::strcmp(snapshot().time, "00:00:30") && snapshot().sync_age_s == 31, "offline tick and monotonic sync age");
    service.process(true);
    check(fake::sntp_inits == 2 && fake::sntp_starts == 2, "reconnect resynchronizes");
    receive(1704124830);
    check(request_sync() == ESP_ERR_TIMEOUT, "manual resync is rate limited");
    fake::now += 10'000'000;
    check(request_sync() == ESP_OK && request_sync() == ESP_OK, "manual requests queue and coalesce");
    timeval immediate{1704124840, 0}; wall_time = immediate.tv_sec;
    fake::sntp_immediate_response = &immediate;
    service.process(true);
    check(snapshot().state == State::synced && snapshot().sync_count == 3, "response during start is not overwritten");
    fake::sntp_immediate_response = nullptr;
    fake::now += 3660'000'000LL;
    service.process(true);
    check(snapshot().state == State::retrying && snapshot().valid, "missed hourly refresh is visible");
    receive(1704128500);
    check(snapshot().state == State::synced && snapshot().error == ESP_OK, "late response recovers");
    service.deinitialize(); service.deinitialize();
    check(snapshot().state == State::stopped && !fake::sntp_active &&
          request_sync() == ESP_ERR_INVALID_STATE, "teardown clears state");
    check(service.initialize() == ESP_OK, "reinitialize after teardown");
    fake::sntp_init_error = ESP_ERR_NO_MEM;
    service.process(true);
    const auto attempts = fake::sntp_inits;
    for (int i = 0; i < 100; ++i) service.process(true);
    check(fake::sntp_inits == attempts && snapshot().error == ESP_ERR_NO_MEM, "init failure has bounded retry");
    fake::sntp_init_error = ESP_OK; fake::sntp_start_error = ESP_FAIL;
    fake::now += 60'000'000; service.process(true);
    check(!fake::sntp_active && snapshot().error == ESP_FAIL, "start failure releases allocation");
    fake::sntp_start_error = ESP_OK; fake::now += 60'000'000; service.process(true);
    check(snapshot().state == State::syncing && fake::sntp_active, "automatic recovery from start failure");

    receive(1791475200);
    fake::now += 40'000'000; wall_time += 40;
    const auto before_sleep = snapshot();
    prepare_for_sleep();
    service.deinitialize();
    wall_time += 120; fake::now = 0; fake::reset_reason = ESP_RST_DEEPSLEEP;
    check(service.initialize() == ESP_OK, "initialize after a deep-sleep wake");
    service.process(false);
    value = snapshot();
    check(value.valid && value.synced && !value.online && value.state == State::waiting_wifi &&
          value.sync_count == before_sleep.sync_count && value.last_sync_epoch == before_sleep.last_sync_epoch &&
          value.epoch == before_sleep.epoch + 120 && value.sync_age_s == before_sleep.sync_age_s + 120,
          "RTC wake retains offline time, sync metadata, and sleep duration in age");
    wall_time += 20; fake::now += 20'000'000;
    check(snapshot().sync_age_s == before_sleep.sync_age_s + 140, "retained age advances on the new boot timer");
    prepare_for_sleep(); service.deinitialize();
    wall_time += 180; fake::now = 0;
    check(service.initialize() == ESP_OK && snapshot().valid &&
          snapshot().sync_age_s == before_sleep.sync_age_s + 320, "consecutive sleeps preserve one RTC timeline");
    service.process(true);
    fake::now += 60'000'000; wall_time += 60; service.process(true);
    check(snapshot().state == State::retrying && snapshot().valid,
          "wake triggers network refresh and reports timeout without losing retained time");
    receive(wall_time);
    check(snapshot().sync_age_s == 0 && snapshot().sync_count == before_sleep.sync_count + 1 &&
          snapshot().state == State::synced, "new SNTP response resets retained age and advances count");

    prepare_for_sleep(); service.deinitialize();
    fake::reset_reason = ESP_RST_SW; fake::now = 0;
    check(service.initialize() == ESP_OK && !snapshot().valid && !snapshot().synced &&
          snapshot().sync_count == 0, "software restart ignores saved sleep validity");
    service.deinitialize(); fake::reset_reason = ESP_RST_DEEPSLEEP;
    check(service.initialize() == ESP_OK && !snapshot().valid,
          "deep-sleep reason alone cannot validate an unsynchronized RTC");
    prepare_for_sleep(); service.deinitialize(); wall_time += 120;
    check(service.initialize() == ESP_OK && !snapshot().valid,
          "sleep before initial SNTP retains the placeholder");

    service.process(true); receive(1791475200);
    prepare_for_sleep(); service.deinitialize(); --wall_time; fake::now = 0;
    check(service.initialize() == ESP_OK && !snapshot().valid,
          "backwards RTC time rejects synchronization metadata");
    service.process(true); receive(1791475200);
    prepare_for_sleep(); service.deinitialize(); wall_time = 0; fake::now = 0;
    check(service.initialize() == ESP_OK && !snapshot().valid,
          "lost RTC time cannot restore synchronization validity");
    service.process(true); receive(1791475200);
    prepare_for_sleep(); service.deinitialize(); fake::reset_reason = ESP_RST_POWERON;
    check(service.initialize() == ESP_OK && !snapshot().valid,
          "power-on reset requires fresh SNTP even with sane wall time and saved metadata");
    service.process(true); receive(4102444800);
    check(!snapshot().valid, "implausible SNTP dates are rejected");
    service.deinitialize();
    std::cout << "Clock service checks passed\n";
}
