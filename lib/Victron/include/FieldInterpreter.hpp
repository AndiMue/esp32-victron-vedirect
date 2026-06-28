/******************************************************************************
 * @file FieldInterpreter.hpp
 *
 * Ausgabe eines VE.Direct-Frames mit semantischen Namen.
 ******************************************************************************/

#pragma once

#include "VeDirectFrame.hpp"

class FieldInterpreter
{
public:

    static void print(const VeDirectFrame& frame);
};