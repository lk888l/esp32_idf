#pragma once
#include <memory>
#include "esp_err.h"
namespace connectivity {
// Application-task owned facade. Transport callbacks only query snapshots or
// enqueue commands. Destruction is permitted only after successful deinitialize.
class Service {
public:
    Service();
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    esp_err_t initialize();
    esp_err_t deinitialize();
    void process();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
