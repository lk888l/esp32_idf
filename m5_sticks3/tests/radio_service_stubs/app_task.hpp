#pragma once
#include <string_view>
class AppTask {
public:
    AppTask(std::string_view, unsigned, unsigned) {}
    virtual ~AppTask() = default;
protected:
    virtual void main() = 0;
};
