// Polls the devices once per tick, applies the active profile's curves and collects alert conditions.
#pragma once
#include <string>
#include <utility>
#include <vector>
#include "core/model.h"
#include "win32/commander_pro.h"
#include "win32/corsair_psu.h"
#include "win32/host_sensors.h"

// Samples kept in RAM for the history graphs, one per poll. All series share Engine::historyTimes and
// Engine::historyHead (the next slot to write).
const int kHistorySamples = 3600;

struct History {
    std::vector<float> values = std::vector<float>(kHistorySamples, NAN);
};

struct Reading {
    std::string id, defaultName, unit;
    bool isTemp = false, present = false;
    float value = NAN;
    History history;
};

struct Output {
    std::string id, defaultName;
    bool present = false, isPsu = false;
    int channel = 0;          // Commander header
    float heldTemp = NAN;     // hysteresis memory
    float command = NAN;      // last value sent
    int commandUnit = UNIT_PERCENT;
    bool deviceMode = false;  // PSU on its own logic
    float rpm = NAN;
    History history;  // rpm
    int stallTicks = 0;
    double lastSent = -1e9;
    std::string state;        // short status for the UI
};

class Engine {
public:
    Config* config = nullptr;
    std::vector<Reading> readings;
    std::vector<Output> outputs;
    std::string commanderStatus, psuStatus, cpuStatus, gpuStatus;
    bool detailed = false;  // the window is open: also read rails and voltages
    std::vector<std::pair<std::string, std::string>> alerts;  // currently active: key, message
    std::vector<double> historyTimes = std::vector<double>(kHistorySamples, -1e18);
    int historyHead = 0;
    double lastTick = 0;

    void start(Config* cfg);
    void tick(double now);
    void stop();     // fans to failsafe, PSU to its own logic, devices closed
    void suspend() { stop(); }
    void resume(double now);
    void forceResend() { for (Output& o : outputs) o.lastSent = -1e9; }

    bool commanderOk() const { return commander_.isOpen(); }
    bool psuOk() const { return psu_.isOpen(); }
    bool cpuOk() const { return cpu_.isOpen(); }
    bool gpuOk() const { return gpu_.isOpen(); }

    Reading* reading(const std::string& id);
    Output* output(const std::string& id);
    std::string nameOf(const std::string& id) const;
    float target(const Output& o, int& unit, bool& deviceMode, std::string& why);  // curve result, no side effects

private:
    CommanderPro commander_;
    CorsairPsu psu_;
    CpuSensor cpu_;
    GpuSensor gpu_;
    int commanderFailures_ = 0, psuFailures_ = 0, ticks_ = 0;
    bool psuHot_ = false, icueRunning_ = false;

    void connectDevices();
    void setReading(const std::string& id, float value, bool present = true);
    void addReading(const char* id, const char* name, const char* unit, bool isTemp);
    void readAll();
    void drive(double now);
    void checkAlerts();
    void record(double now);
    float holdTemperature(const Output& o, const Curve& curve, float current) const;
};
