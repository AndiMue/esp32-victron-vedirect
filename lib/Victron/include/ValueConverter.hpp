#pragma once

#include "MeasuredValue.hpp"
#include "FieldDefinition.hpp"

class ValueConverter
{
public:

    static MeasuredValue convert(const FieldDefinition& definition,
                                 const char* value);
};