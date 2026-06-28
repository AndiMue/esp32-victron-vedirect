/******************************************************************************
 * @file FieldCatalog.hpp
 *
 * Zuordnung von VE.Direct-Labels zu semantischen Feldern.
 ******************************************************************************/

#pragma once

#include "FieldDefinition.hpp"

class FieldCatalog
{
public:

    static const FieldDefinition* find(const char* label);
};