#pragma once

#include <string_view>

class AppModule {
public:
    virtual ~AppModule() = default;

    virtual bool initialize() = 0;
    virtual bool deinitialize() = 0;
    virtual bool is_initialized() const = 0;
    virtual std::string_view name() const = 0;

    virtual void process() {}

protected:
    AppModule() = default;

private:
    AppModule(const AppModule&) = delete;
    AppModule& operator=(const AppModule&) = delete;
};
