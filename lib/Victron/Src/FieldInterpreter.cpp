/******************************************************************************
 * @file FieldInterpreter.cpp
 *
 * Ausgabe eines VE.Direct-Frames.
 ******************************************************************************/

#include "FieldInterpreter.hpp"

#include <Arduino.h>
#include <cstring>

#include "FieldCatalog.hpp"
#include "ValueConverter.hpp"
#include "OffReason.hpp"

void FieldInterpreter::print(const VeDirectFrame& frame)
{
    Serial.println();
    Serial.println("========== INTERPRETED FRAME ==========");

    for (size_t i = 0; i < frame.fieldCount; ++i)
    {
        const auto& field = frame.fields[i];

        // Checksum nicht anzeigen
        if (std::strcmp(field.label.data(), "Checksum") == 0)
        {
            continue;
        }

        const FieldDefinition* definition =
            FieldCatalog::find(field.label.data());

        if (definition == nullptr)
        {
            Serial.print("[UNKNOWN] ");
            Serial.print(field.label.data());
            Serial.print(" = ");
            Serial.println(field.value.data());
            continue;
        }

        // Off Reason separat behandeln
        if (definition->semantic == SemanticField::OffReason)
        {
            Serial.printf("%-24s : %s\n",
                          definition->displayName,
                          OffReason::toString(field.value.data()));
            continue;
        }

        MeasuredValue value =
            ValueConverter::convert(*definition,
                                    field.value.data());

        Serial.printf("%-24s : ",
                      definition->displayName);

        if (value.valid)
        {
            switch (value.type)
            {
                case ValueType::Float:

                    Serial.print(value.value, 1);
                    break;

                case ValueType::Integer:

                    Serial.print(static_cast<int32_t>(value.value));
                    break;

                case ValueType::Boolean:
                case ValueType::String:
                case ValueType::Hex:
                default:

                    Serial.print(field.value.data());
                    break;
            }

            if (value.unit[0] != '\0')
            {
                Serial.print(" ");
                Serial.print(value.unit);
            }
        }
        else
        {
            Serial.print(field.value.data());
        }

        Serial.println();
    }

    Serial.println("=======================================");
}