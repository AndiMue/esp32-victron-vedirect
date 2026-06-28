#pragma once

#include "SemanticField.hpp"
#include "FieldDefinition.hpp"

struct MeasuredValue
{
    SemanticField field = SemanticField::Unknown;

    ValueType type = ValueType::Integer;

    float value = 0.0f;

    const char* unit = "";

    bool valid = false;
};