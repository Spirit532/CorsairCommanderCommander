#include "win32/commander_pro.h"
#include <cstdio>
#include <cstring>

namespace {
enum : uint8_t {
    CMD_GET_FIRMWARE = 0x02,
    CMD_GET_TEMP_CONFIG = 0x10,
    CMD_GET_TEMP = 0x11,
    CMD_GET_VOLTS = 0x12,
    CMD_GET_FAN_MODES = 0x20,
    CMD_GET_FAN_RPM = 0x21,
    CMD_SET_FAN_DUTY = 0x23,
    CMD_SET_FAN_TARGET = 0x24,
};
int be16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
}  // namespace

bool CommanderPro::command(uint8_t cmd, const uint8_t* args, int argCount, uint8_t* reply) {
    uint8_t out[64] = {cmd};
    if (argCount > 0) memcpy(out + 1, args, argCount);
    hid_.flushInput();
    if (!hid_.write(out, sizeof out)) return false;
    uint8_t in[64] = {};
    int n = hid_.read(in, sizeof in);
    if (n < 3) return false;
    memcpy(reply, in, 16);
    return in[0] == 0;  // status byte
}

bool CommanderPro::connect() {
    if (!hid_.open(0x1b1c, 0x0c10, 63)) return false;
    uint8_t r[16];
    if (!command(CMD_GET_FIRMWARE, nullptr, 0, r)) {
        hid_.close();
        return false;
    }
    char text[32];
    snprintf(text, sizeof text, "%d.%d.%d", r[1], r[2], r[3]);
    firmware = text;
    if (command(CMD_GET_TEMP_CONFIG, nullptr, 0, r))
        for (int i = 0; i < kTemps; i++) tempConnected[i] = r[1 + i] == 1;
    if (command(CMD_GET_FAN_MODES, nullptr, 0, r))
        for (int i = 0; i < kFans; i++) fanType[i] = r[1 + i] <= FAN_PWM ? r[1 + i] : FAN_NONE;
    return true;
}

bool CommanderPro::readTemp(int probe, float& celsius) {
    uint8_t arg = (uint8_t)probe, r[16];
    if (!command(CMD_GET_TEMP, &arg, 1, r)) return false;
    celsius = be16(r + 1) / 100.0f;
    return true;
}

bool CommanderPro::readFanRpm(int fan, int& rpm) {
    uint8_t arg = (uint8_t)fan, r[16];
    if (!command(CMD_GET_FAN_RPM, &arg, 1, r)) return false;
    rpm = be16(r + 1);
    return true;
}

bool CommanderPro::readRail(int rail, float& volts) {
    uint8_t arg = (uint8_t)rail, r[16];
    if (!command(CMD_GET_VOLTS, &arg, 1, r)) return false;
    volts = be16(r + 1) / 1000.0f;
    return true;
}

bool CommanderPro::setFanPercent(int fan, int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint8_t args[2] = {(uint8_t)fan, (uint8_t)percent}, r[16];
    return command(CMD_SET_FAN_DUTY, args, 2, r);
}

bool CommanderPro::setFanRpm(int fan, int rpm) {
    if (rpm < 0) rpm = 0;
    if (rpm > 0xFFFF) rpm = 0xFFFF;
    uint8_t args[3] = {(uint8_t)fan, (uint8_t)(rpm >> 8), (uint8_t)rpm}, r[16];
    return command(CMD_SET_FAN_TARGET, args, 3, r);
}
