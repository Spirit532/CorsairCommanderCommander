// Fan curves, profiles and settings. Platform independent.
#pragma once
#include <cmath>
#include <map>
#include <string>
#include <vector>

enum CurveMode { MODE_CURVE, MODE_FIXED, MODE_DEVICE };  // MODE_DEVICE: the device's own logic (PSU auto)
enum CurveUnit { UNIT_PERCENT, UNIT_RPM };

struct CurvePoint {
    float temp = 0, value = 0;
};

struct Curve {
    std::string name;
    int mode = MODE_CURVE;
    int unit = UNIT_PERCENT;
    std::string sensor;     // sensor id driving the curve
    float fixed = 40;       // MODE_FIXED value, in `unit`
    float hysteresis = 1;   // degrees the temperature must fall before the output follows it down
    float autoBelow = 0;    // PSU: hand the fan back to the PSU's own logic below this temperature (0 = off)
    std::vector<CurvePoint> points;  // ascending temperature

    void sortPoints();
    float evaluate(float temp) const;  // linear between points, flat beyond the ends
};

struct Profile {
    std::string name;
    std::map<std::string, std::string> assignments;  // output id -> curve name
};

struct Settings {
    int pollSeconds = 1;
    bool startWithWindows = true;
    bool openWindowAtStart = false;
    int uiScale = 0;              // 0 = follow DPI
    float failsafePercent = 70;   // Commander fans when their sensor is lost, and on exit
    float alertCoolant = 45;      // Commander probes
    float alertCpu = 95, alertGpu = 88, alertPsu = 70;
    float psuAutoAbove = 60;      // PSU fan goes back to its own logic above this PSU temperature
    int historyWindow = 3;        // index into kHistoryWindows
};

// Selectable history spans, seconds.
const int kHistoryWindows[] = {30, 60, 120, 300, 600, 900, 1800, 3600};
const int kHistoryWindowCount = 8;

struct Config {
    Settings settings;
    std::map<std::string, std::string> names;  // sensor/output id -> user name
    std::vector<Curve> curves;
    std::vector<Profile> profiles;
    std::string activeProfile;

    Curve* findCurve(const std::string& name);
    Profile* active();
};

bool loadConfig(const char* path, Config& config);
bool saveConfig(const char* path, const Config& config);

// Imports curves, assignments and names from iCUE's CUE5 folder (%APPDATA%\Corsair\CUE5).
// Returns a log of what was found, empty on failure.
std::string importIcue(const std::string& cue5Folder, Config& config);
