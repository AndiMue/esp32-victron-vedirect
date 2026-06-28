#pragma once

#include <cstdint>
#include <string_view>
#include <vector>
#include "DeviceTypes.hpp"
#include "SemanticField.hpp"

namespace victron {

struct DeviceDescriptor {
    uint16_t pid;
    DeviceType type;
    std::string_view name;
    std::vector<SemanticField> expectedFields;
};

} // namespace victron