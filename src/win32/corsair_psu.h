// Corsair HXi and RMi power supplies: PMBus over HID, as documented by liquidctl.
#pragma once
#include <string>
#include "win32/hid.h"

class CorsairPsu {
public:
    struct Rail {
        float volts = 0, amps = 0, watts = 0;
    };
    struct Status {
        float tempVrm = 0, tempCase = 0, fanRpm = 0, inputVolts = 0, outputWatts = 0;
        bool softwareFan = false;
        Rail rails[3];  // +12 V, +5 V, +3.3 V
        bool hasRails = false;
    };

    bool connect();  // the first supported PSU found
    std::string model;
    void disconnect();
    bool isOpen() const { return hid_.isOpen(); }

    bool read(Status& status, bool withRails);
    bool setHardwareFan();  // the PSU's own fan logic
    bool setFanPercent(int percent);  // switches to software control

private:
    HidDevice hid_;
    bool softwareMode_ = false;
    bool exec(bool isRead, uint8_t command, const uint8_t* data, int dataLength, uint8_t* reply);
    bool readFloat(uint8_t command, float& value);
};
