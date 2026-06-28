#pragma once

#include <cstdint>
#include <optional>
#include <vector>
#include <string_view>

#include "DeviceTypes.hpp"
#include "DeviceDescriptors.hpp"
#include "SemanticField.hpp"

namespace victron {

class DeviceCatalog {
public:
    static const DeviceDescriptor* findByPid(uint16_t pid);
    static DeviceType resolveType(uint16_t pid);
    static std::vector<SemanticField> expectedFields(DeviceType type);
    static SemanticField resolveSemanticField(std::string_view label);

    static std::string_view typeName(DeviceType type);
};

} // namespace victron