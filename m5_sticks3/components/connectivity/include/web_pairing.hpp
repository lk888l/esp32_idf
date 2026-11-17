#pragma once
#include <cstdint>
namespace connectivity
{
// Only physical UI/USB may call this. The code is one-use, valid for 120 s.
uint32_t open_web_pairing();
uint32_t local_web_pairing_code();
bool consume_web_pairing(uint32_t code);
void revoke_web_sessions();
uint32_t web_session_generation();
} // namespace connectivity
