#include <Arduino.h>

#include "VeDirectReader.hpp"
#include "VeDirectTextParser.hpp"
#include "FieldInterpreter.hpp"
#include "DeviceCatalog.hpp"
#include "DeviceInterpreter.hpp"



HardwareSerial VictronSerial(1);

VeDirectReader reader(
    VictronSerial,
    16,
    17);

VeDirectTextParser parser;

void setup()
{
    Serial.begin(115200);

    reader.begin();

    Serial.println();
    Serial.println("--------------------------------");
    Serial.println("Victron VE.Direct Reader");
    Serial.println("--------------------------------");
}

void loop()
{
    while (reader.available())
    {
        if (parser.parse(reader.read()))
        {
            FieldInterpreter::print(parser.frame());

            DeviceInterpreter::print(parser.frame());

            parser.reset();
        }
    }
}