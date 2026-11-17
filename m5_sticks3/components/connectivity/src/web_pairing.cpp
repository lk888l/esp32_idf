#include "web_pairing.hpp"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
namespace connectivity
{
namespace
{
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
uint32_t code = 0, generation = 0;
int64_t deadline = 0;
unsigned attempts = 0;
} // namespace
uint32_t open_web_pairing()
{
    const uint32_t fresh = 100000 + esp_random() % 900000;
    portENTER_CRITICAL(&lock);
    code = fresh;
    deadline = esp_timer_get_time() + 120'000'000;
    attempts = 0;
    portEXIT_CRITICAL(&lock);
    return fresh;
}
uint32_t local_web_pairing_code()
{
    portENTER_CRITICAL(&lock);
    const auto value = esp_timer_get_time() < deadline ? code : 0;
    portEXIT_CRITICAL(&lock);
    return value;
}
bool consume_web_pairing(uint32_t supplied)
{
    portENTER_CRITICAL(&lock);
    bool valid = code && esp_timer_get_time() < deadline && attempts < 5;
    if (valid)
    {
        ++attempts;
        valid = supplied == code;
        if (valid || attempts == 5)
            code = 0;
    }
    portEXIT_CRITICAL(&lock);
    return valid;
}
void revoke_web_sessions()
{
    portENTER_CRITICAL(&lock);
    ++generation;
    code = 0;
    portEXIT_CRITICAL(&lock);
}
uint32_t web_session_generation()
{
    portENTER_CRITICAL(&lock);
    auto value = generation;
    portEXIT_CRITICAL(&lock);
    return value;
}
} // namespace connectivity
