/******************************************************************************
 * @file ValueConverter.cpp
 ******************************************************************************/

#include "ValueConverter.hpp"

#include <cstdlib>

MeasuredValue ValueConverter::convert(const FieldDefinition& definition,
                                      const char* value)
{
    MeasuredValue result;

    result.field = definition.semantic;
    result.type  = definition.type;
    result.unit  = definition.unit;
    result.valid = true;

    switch (definition.type)
    {
        case ValueType::Integer:

            result.value =
                static_cast<float>(std::atoi(value))
                * definition.scale;

            break;

        case ValueType::Float:

            result.value =
                static_cast<float>(std::atof(value))
                * definition.scale;

            break;

        case ValueType::Boolean:
        case ValueType::String:
        case ValueType::Hex:

            result.valid = false;
            break;
    }

    return result;
}