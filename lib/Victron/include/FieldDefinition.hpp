/******************************************************************************
 * @file FieldDefinition.hpp
 *
 * Definition eines VE.Direct-Feldes.
 ******************************************************************************/

#pragma once

#include "SemanticField.hpp"

enum class ValueType
{
    Integer,
    Float,
    String,
    Boolean,
    Hex
};

struct FieldDefinition
{
    const char* label;

    const char* displayName;

    SemanticField semantic;

    ValueType type;

    float scale;

    const char* unit;
};