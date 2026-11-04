#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "VeDirectReader.hpp"
#include "VeDirectFrame.hpp"


HardwareSerial Uart1(1);
HardwareSerial Uart2(2);

/******************************************************************************
 * UART1
 ******************************************************************************/
VeDirectReader uart1Reader(Uart1, 16, 17); 


/******************************************************************************
 * UART2
 ******************************************************************************/
VeDirectReader uart2Reader(Uart2, 26, 33); 



void uart1Task(void*)
{
    for (;;)
    {
        while (uart1Reader.available())
        {
            char c = uart1Reader.read();

            if (c == '\r')
    continue;
    else if (c == '\n')
        Serial.println();
    else if (c == '\t')
        Serial.print('\t');
    else if (c >= 32 && c <= 126)
        Serial.write(c);
    else
        Serial.printf("<0x%02X>", (uint8_t)c);
            }
            vTaskDelay(1);
    }
}

void uart2Task(void*)
{
    for (;;)
    {
        while (uart2Reader.available())
        {
            char c = uart2Reader.read();

            if (c == '\r')
    continue;
    else if (c == '\n')
        Serial.println();
    else if (c == '\t')
        Serial.print('\t');
    else if (c >= 32 && c <= 126)
        Serial.write(c);
    else
        Serial.printf("<0x%02X>", (uint8_t)c);
            }
            vTaskDelay(1);
    }
}



void setup()
{
    Serial.begin(115200);

    uart1Reader.begin();
    uart2Reader.begin();

    xTaskCreatePinnedToCore(uart1Task, "UART1", 4096, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(uart2Task, "UART2", 4096, nullptr, 1, nullptr, 1);

    Serial.println();
    Serial.println("--------------------------------");
    Serial.println("Victron VE.Direct Reader");
    Serial.println("--------------------------------");
    Serial.print("sizeof(VeDirectFrame) = ");
    Serial.println(sizeof(VeDirectFrame));
}


/******************************************************************************
 * Hauptschleife
 *
 * Hier findet keine VE.Direct Verarbeitung mehr statt.
 *
 * Die UART-Tasks laufen auf den beiden Kernen permanent im Hintergrund
 * und aktualisieren die Zustände:
 *
 *   uart1MpptState
 *   uart2BatteryState
 *
 * Die loop() dient nur noch dazu, die aktuell bekannten Werte
 * zyklisch auszugeben.
 ******************************************************************************/
void loop()
{
        /*Serial.println();
        Serial.println("================================================");
        Serial.println("AKTUELLER SYSTEMSTATUS");
        Serial.println("================================================");

        /**********************************************************************
         * MPPT-Daten (UART1)
         **********************************************************************/
        /*Serial.println();
        Serial.println("MPPT");

        Serial.print("PV Power        : ");
        Serial.println(uart1MpptState.pvPower);

        Serial.print("Charge Current  : ");
        Serial.println(uart1MpptState.chargeCurrent);

        Serial.print("Load Output     : ");
        Serial.println(uart1MpptState.loadOutputEnabled
                ? "ON"
                : "OFF");

        Serial.print("Off Reason      : ");
        Serial.println(uart1MpptState.offReason);

        /**********************************************************************
         * Batteriedaten vom SmartShunt (UART2)
         **********************************************************************/
        /*Serial.println();
        Serial.println("BATTERY");

        Serial.print("Voltage         : ");
        Serial.println(uart2BatteryState.voltage);

        Serial.print("Current         : ");
        Serial.println(uart2BatteryState.current);

        Serial.print("Power           : ");
        Serial.println(uart2BatteryState.power);

        Serial.print("State Of Charge : ");
        Serial.println(uart2BatteryState.stateOfCharge);

        Serial.println();
        Serial.println("================================================");
    }*/
}


/*#include <Arduino.h>
#include "VeDirectReader.hpp"
#include "VeDirectTextParser.hpp"
#include "FieldInterpreter.hpp"
#include "BatteryStateBuilder.hpp"
#include "MpptStateBuilder.hpp"


HardwareSerial VictronSerial(1);

VeDirectReader reader(
    VictronSerial,
    16,
    17);
VeDirectTextParser parser;
BatteryStateBuilder batteryBuilder;
MpptStateBuilder mpptBuilder;


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
            const VeDirectField* pid = parser.frame().find("PID");

            if (pid)
            {
                Serial.print("FRAME PID = ");
                Serial.println(pid->value.data());
            }
            else
            {
                Serial.println("FRAME PID = NONE");
            }

            FieldInterpreter::print(parser.frame());
            
            batteryBuilder.update(parser.frame());
            mpptBuilder.update(parser.frame());

            const MpptState& mppt = mpptBuilder.state();
            Serial.print("MPPT available before serial printing= ");
            Serial.println(mppt.available);
            if (mppt.available)
            {
                Serial.println();
                Serial.println("=========== MPPT STATE ===========");

                Serial.print("PV Power      : ");
                Serial.println(mppt.pvPower);

                Serial.print("Charge Current: ");
                Serial.println(mppt.chargeCurrent);

                Serial.print("Load Output   : ");
                Serial.println(
                    mppt.loadOutputEnabled ? "ON" : "OFF");

                Serial.print("Off Reason    : ");
                Serial.println(mppt.offReason);

                Serial.println("==================================");

                mpptBuilder.clear();
            }
            Serial.print("MPPT available after serial printing and clear = ");
            Serial.println(mppt.available);

            const BatteryState& battery = batteryBuilder.state();
            Serial.print("Battery available = ");
            Serial.println(battery.available);
            if (battery.available)
            {
                Serial.println();
                Serial.println("========== BATTERY STATE ==========");

                Serial.print("Voltage : ");
                Serial.println(battery.voltage);

                Serial.print("Current : ");
                Serial.println(battery.current);

                Serial.print("Power   : ");
                Serial.println(battery.power);

                Serial.print("SOC     : ");
                Serial.println(battery.stateOfCharge);

                Serial.println("===================================");

                batteryBuilder.clear();
            }
            Serial.print("Battery available after serial printing and clear = ");
            Serial.println(battery.available);

            parser.reset();
            //reader.flush();

            const VeDirectField* pidAfterReset = parser.frame().find("PID");

            if (pidAfterReset)
            {
                Serial.println("PID noch vorhanden");
            }
            else
            {
                Serial.println("PID weg");
            }

        }
    }
}*/