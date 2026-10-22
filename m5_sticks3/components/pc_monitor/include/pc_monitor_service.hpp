#pragma once

#include "pc_monitor_protocol.hpp"

namespace pc_monitor {

// Single application-task owner. Nonblocking, at most four packets per process;
// no extra task or heap allocation is needed for steady-state reception.
class Service final {
public:
    Service() = default;
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    void process(bool wifi_enabled);
    void deinitialize();

private:
    void close_transport();
    void failed(int error);
    void open_transport();
    void poll();
    void publish();
    int socket_ = -1;
    bool started_ = false;
    uint32_t retry_at_ = 0, received_at_ = 0;
    LatestSample latest_{};
    Snapshot state_{};
    char packet_[max_packet_bytes]{};
};

// Safe from the LVGL, HTTP, BLE and console tasks. Age is computed at read time.
Snapshot snapshot();

} // namespace pc_monitor
