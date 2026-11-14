#include "win32/corsair_psu.h"
#include <cmath>
#include <cstring>

namespace {
const uint8_t kAddressWrite = 0x02, kAddressRead = 0x03;
enum : uint8_t {
    PAGE = 0x00,
    FAN_COMMAND_1 = 0x3B,
    READ_VIN = 0x88,
    READ_VOUT = 0x8B,
    READ_IOUT = 0x8C,
    READ_TEMPERATURE_1 = 0x8D,
    READ_TEMPERATURE_2 = 0x8E,
    READ_FAN_SPEED_1 = 0x90,
    READ_POUT = 0x96,
    CORSAIR_OUTPUT_POWER = 0xEE,
    CORSAIR_FAN_MODE = 0xF0,
};

// PMBus LINEAR11: 5-bit signed exponent, 11-bit signed mantissa, little endian.
float linear11(const uint8_t* p) {
    int raw = p[0] | (p[1] << 8);
    int exponent = raw >> 11, mantissa = raw & 0x7FF;
    if (exponent > 15) exponent -= 32;
    if (mantissa > 1023) mantissa -= 2048;
    return (float)ldexp((double)mantissa, exponent);
}
}  // namespace

bool CorsairPsu::exec(bool isRead, uint8_t command, const uint8_t* data, int dataLength, uint8_t* reply) {
    uint8_t out[64] = {isRead ? kAddressRead : kAddressWrite, command};
    if (dataLength > 0) memcpy(out + 2, data, dataLength);
    if (!hid_.write(out, sizeof out)) return false;
    uint8_t in[64] = {};
    if (hid_.read(in, sizeof in) < 4) return false;
    if (in[0] != out[0] || in[1] != out[1]) return false;  // another program is talking to the PSU
    if (reply) memcpy(reply, in, 64);
    return true;
}

bool CorsairPsu::readFloat(uint8_t command, float& value) {
    uint8_t r[64];
    if (!exec(true, command, nullptr, 0, r)) return false;
    value = linear11(r + 2);
    return std::isfinite(value);
}

bool CorsairPsu::connect() {
    static const struct {
        uint16_t pid;
        const char* name;
    } models[] = {
        {0x1c05, "HX750i"},  {0x1c06, "HX850i"},  {0x1c07, "HX1000i"}, {0x1c08, "HX1200i"},
        {0x1c0a, "RM650i"},  {0x1c0b, "RM750i"},  {0x1c0c, "RM850i"},  {0x1c0d, "RM1000i"},
        {0x1c1e, "HX1000i"}, {0x1c1f, "HX1500i"}, {0x1c23, "HX1200i"}, {0x1c27, "HX1200i"},
    };
    for (auto& m : models)
        if (hid_.open(0x1b1c, m.pid, 63)) {
            model = m.name;
            break;
        }
    if (!hid_.isOpen()) return false;
    hid_.flushInput();
    const uint8_t init[2] = {0xFE, 0x03};
    uint8_t in[64];
    if (!hid_.write(init, 2) || hid_.read(in, sizeof in) < 0) {
        hid_.close();
        return false;
    }
    uint8_t r[64];
    softwareMode_ = exec(true, CORSAIR_FAN_MODE, nullptr, 0, r) && r[2] == 1;
    return true;
}

void CorsairPsu::disconnect() {
    if (isOpen()) setHardwareFan();
    hid_.close();
}

bool CorsairPsu::read(Status& s, bool withRails) {
    hid_.flushInput();
    uint8_t page0 = 0, r[64];
    if (!exec(false, PAGE, &page0, 1, nullptr)) return false;
    bool ok = readFloat(READ_TEMPERATURE_1, s.tempVrm) && readFloat(READ_TEMPERATURE_2, s.tempCase) &&
              readFloat(READ_FAN_SPEED_1, s.fanRpm) && readFloat(READ_VIN, s.inputVolts) &&
              readFloat(CORSAIR_OUTPUT_POWER, s.outputWatts);
    if (!ok) return false;
    if (exec(true, CORSAIR_FAN_MODE, nullptr, 0, r)) s.softwareFan = r[2] == 1;
    s.hasRails = false;
    if (withRails) {
        bool railsOk = true;
        for (uint8_t rail = 0; rail < 3 && railsOk; rail++) {
            railsOk = exec(false, PAGE, &rail, 1, nullptr) && readFloat(READ_VOUT, s.rails[rail].volts) &&
                      readFloat(READ_IOUT, s.rails[rail].amps) && readFloat(READ_POUT, s.rails[rail].watts);
        }
        exec(false, PAGE, &page0, 1, nullptr);
        s.hasRails = railsOk;
    }
    return true;
}

bool CorsairPsu::setHardwareFan() {
    uint8_t mode = 0;
    softwareMode_ = false;
    return exec(false, CORSAIR_FAN_MODE, &mode, 1, nullptr);
}

bool CorsairPsu::setFanPercent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint8_t mode = 1;  // always: the PSU may have dropped back to its own logic on its own
    if (!exec(false, CORSAIR_FAN_MODE, &mode, 1, nullptr)) return false;
    softwareMode_ = true;
    uint8_t duty = (uint8_t)percent;
    return exec(false, FAN_COMMAND_1, &duty, 1, nullptr);
}
