/******************************************************************************
 * @file Pcf8523.cpp
 *
 * Siehe Pcf8523.hpp.
 ******************************************************************************/

#include "Pcf8523.hpp"

/******************************************************************************
 * Codec (ohne Arduino, testbar)
 ******************************************************************************/
namespace Pcf8523Codec
{

bool encode(const Rtc::DateTime& utc, uint8_t regs[TimeRegs])
{
    if (!utc.isValid())
    {
        return false;
    }
    regs[0] = Rtc::toBcd(utc.second);   // OS-Flag (Bit 7) = 0 -> Zeit gültig
    regs[1] = Rtc::toBcd(utc.minute);
    regs[2] = Rtc::toBcd(utc.hour);     // 24-h-Modus
    regs[3] = Rtc::toBcd(utc.day);
    regs[4] = Rtc::weekday(utc);        // 0..6, wird vom Chip nur mitgezählt
    regs[5] = Rtc::toBcd(utc.month);
    regs[6] = Rtc::toBcd(static_cast<uint8_t>(utc.year - 2000));
    return true;
}

bool decode(const uint8_t regs[TimeRegs], Rtc::DateTime& utc)
{
    if (regs[0] & SecondsOs)
    {
        return false;   // Oszillator stand -> Zeit unbrauchbar
    }
    Rtc::DateTime t;
    t.second = Rtc::fromBcd(regs[0] & 0x7F);
    t.minute = Rtc::fromBcd(regs[1] & 0x7F);
    t.hour   = Rtc::fromBcd(regs[2] & 0x3F);
    t.day    = Rtc::fromBcd(regs[3] & 0x3F);
    t.month  = Rtc::fromBcd(regs[5] & 0x1F);
    t.year   = static_cast<uint16_t>(2000 + Rtc::fromBcd(regs[6]));
    if (!t.isValid())
    {
        return false;
    }
    utc = t;
    return true;
}

uint8_t runningControl1(uint8_t control1)
{
    return static_cast<uint8_t>(control1 & ~(Control1Stop | Control1Mode12));
}

uint8_t runningControl3(uint8_t control3)
{
    // PM = 000: Umschaltung auf Knopfzelle im Standardmodus, Überwachung an.
    // Bit 3..0: BSF löschen, BLF ist nur lesbar, Interrupts (BSIE/BLIE) aus.
    return static_cast<uint8_t>(control3 & ~Control3PmMask & ~0x0F);
}

} // namespace Pcf8523Codec

/******************************************************************************
 * I²C (nur mit Arduino)
 ******************************************************************************/
#ifdef ARDUINO

bool Pcf8523::readRegs(uint8_t reg, uint8_t* data, uint8_t count)
{
    m_wire.beginTransmission(m_address);
    m_wire.write(reg);
    if (m_wire.endTransmission(false) != 0)
    {
        return false;
    }
    if (m_wire.requestFrom(static_cast<int>(m_address), static_cast<int>(count)) != count)
    {
        return false;
    }
    for (uint8_t i = 0; i < count; ++i)
    {
        data[i] = static_cast<uint8_t>(m_wire.read());
    }
    return true;
}

bool Pcf8523::writeRegs(uint8_t reg, const uint8_t* data, uint8_t count)
{
    m_wire.beginTransmission(m_address);
    m_wire.write(reg);
    m_wire.write(data, count);
    return m_wire.endTransmission() == 0;
}

bool Pcf8523::begin()
{
    uint8_t c1 = 0;
    uint8_t c3 = 0;
    if (!readRegs(Pcf8523Codec::RegControl1, &c1, 1) || !readRegs(Pcf8523Codec::RegControl3, &c3, 1))
    {
        return false;   // Chip antwortet nicht
    }
    const uint8_t want1 = Pcf8523Codec::runningControl1(c1);
    if (want1 != c1 && !writeRegs(Pcf8523Codec::RegControl1, &want1, 1))
    {
        return false;
    }
    if ((c3 & Pcf8523Codec::Control3PmMask) != 0)
    {
        const uint8_t want3 = Pcf8523Codec::runningControl3(c3);
        if (!writeRegs(Pcf8523Codec::RegControl3, &want3, 1))
        {
            return false;
        }
    }
    return true;
}

bool Pcf8523::read(Rtc::DateTime& utc)
{
    uint8_t regs[Pcf8523Codec::TimeRegs];
    if (!readRegs(Pcf8523Codec::RegSeconds, regs, Pcf8523Codec::TimeRegs))
    {
        return false;
    }
    return Pcf8523Codec::decode(regs, utc);
}

bool Pcf8523::write(const Rtc::DateTime& utc)
{
    uint8_t regs[Pcf8523Codec::TimeRegs];
    if (!Pcf8523Codec::encode(utc, regs))
    {
        return false;
    }
    return writeRegs(Pcf8523Codec::RegSeconds, regs, Pcf8523Codec::TimeRegs);
}

bool Pcf8523::batteryLow()
{
    uint8_t c3 = 0;
    if (!readRegs(Pcf8523Codec::RegControl3, &c3, 1))
    {
        return false;
    }
    return (c3 & Pcf8523Codec::Control3Blf) != 0;
}

#endif // ARDUINO
