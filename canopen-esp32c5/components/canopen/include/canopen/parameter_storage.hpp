#pragma once

#include <cstdint>

namespace canopen {

// Platform boundary for durable CANopen communication parameters.  The core
// deliberately owns neither flash nor a filesystem; a target adapter provides
// the commit operation and only reports success after the value is durable.
class ParameterStorage {
public:
    virtual ~ParameterStorage() = default;
    virtual bool store_node_id(uint8_t node_id) = 0;
};

} // namespace canopen
