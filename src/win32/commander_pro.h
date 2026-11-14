// Corsair Commander Pro (1b1c:0c10): 4 temperature probes, 6 fan headers, rail voltages.
// Protocol as documented by liquidctl and the Linux corsair-cpro driver.
#pragma once
#include <string>
#include "win32/hid.h"

class CommanderPro {
public:
    static const int kTemps = 4, kFans = 6;
    enum FanType { FAN_NONE = 0, FAN_DC = 1, FAN_PWM = 2 };

    bool connect();  // opens and reads firmware, probe and header configuration
    void disconnect() { hid_.close(); }
    bool isOpen() const { return hid_.isOpen(); }

    bool tempConnected[kTemps] = {};
    int fanType[kFans] = {};
    std::string firmware;

    bool readTemp(int probe, float& celsius);
    bool readFanRpm(int fan, int& rpm);
    bool readRail(int rail, float& volts);  // 0 = 12 V, 1 = 5 V, 2 = 3.3 V
    bool setFanPercent(int fan, int percent);
    bool setFanRpm(int fan, int rpm);  // the controller regulates to the target

private:
    HidDevice hid_;
    bool command(uint8_t cmd, const uint8_t* args, int argCount, uint8_t* reply);  // reply: 16 bytes
};
