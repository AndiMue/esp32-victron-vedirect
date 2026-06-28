/******************************************************************************
 * @file DeviceInterpreter.hpp
 *
 * Gibt erkannte Geräteinformationen aus.
 ******************************************************************************/

#pragma once

#include "VeDirectFrame.hpp"

class DeviceInterpreter
{
public:

    static void print(const VeDirectFrame& frame);
};