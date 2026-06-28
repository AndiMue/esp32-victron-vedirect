#include "OffReason.hpp"

#include <cstring>

const char* OffReason::toString(const char* value)
{
    if (std::strcmp(value, "0x00000000") == 0)
        return "None";

    if (std::strcmp(value, "0x00000001") == 0)
        return "No input power";

    if (std::strcmp(value, "0x00000002") == 0)
        return "Power switch off";

    if (std::strcmp(value, "0x00000004") == 0)
        return "Device mode off";

    if (std::strcmp(value, "0x00000008") == 0)
        return "Remote input";

    if (std::strcmp(value, "0x00000010") == 0)
        return "Protection active";

    if (std::strcmp(value, "0x00000020") == 0)
        return "Pay As You Go";

    if (std::strcmp(value, "0x00000040") == 0)
        return "BMS";

    if (std::strcmp(value, "0x00000080") == 0)
        return "Engine shutdown detection";

    if (std::strcmp(value, "0x00000100") == 0)
        return "Input voltage analysis";

    return value;
}