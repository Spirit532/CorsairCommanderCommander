#include "core/model.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

void Curve::sortPoints() {
    std::sort(points.begin(), points.end(), [](const CurvePoint& a, const CurvePoint& b) { return a.temp < b.temp; });
}

float Curve::evaluate(float temp) const {
    if (points.empty()) return fixed;
    if (!(temp > points.front().temp)) return points.front().value;
    if (temp >= points.back().temp) return points.back().value;
    for (size_t i = 1; i < points.size(); i++) {
        const CurvePoint &a = points[i - 1], &b = points[i];
        if (temp <= b.temp) {
            float span = b.temp - a.temp;
            return span > 0 ? a.value + (b.value - a.value) * (temp - a.temp) / span : b.value;
        }
    }
    return points.back().value;
}

Curve* Config::findCurve(const std::string& name) {
    for (Curve& c : curves)
        if (c.name == name) return &c;
    return nullptr;
}

Profile* Config::active() {
    for (Profile& p : profiles)
        if (p.name == activeProfile) return &p;
    if (profiles.empty()) profiles.push_back({"Default", {}});
    activeProfile = profiles.front().name;
    return &profiles.front();
}

// File format: INI-like sections, repeated [curve] and [profile] sections. --------------------

namespace {

FILE* openPath(const char* path, const char* mode) {
#ifdef _WIN32
    wchar_t widePath[1024], wideMode[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, widePath, 1024)) return nullptr;
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wideMode, 8);
    return _wfopen(widePath, wideMode);
#else
    return fopen(path, mode);
#endif
}

const char* const kModeNames[] = {"curve", "fixed", "device"};
const char* const kUnitNames[] = {"percent", "rpm"};

int indexOf(const char* const* names, int count, const std::string& value, int fallback) {
    for (int i = 0; i < count; i++)
        if (value == names[i]) return i;
    return fallback;
}

template <typename Visitor>
void forEachSetting(Settings& s, Visitor visit) {
    visit("poll_seconds", &s.pollSeconds, nullptr, nullptr);
    visit("start_with_windows", nullptr, &s.startWithWindows, nullptr);
    visit("open_window_at_start", nullptr, &s.openWindowAtStart, nullptr);
    visit("ui_scale", &s.uiScale, nullptr, nullptr);
    visit("failsafe_percent", nullptr, nullptr, &s.failsafePercent);
    visit("alert_coolant", nullptr, nullptr, &s.alertCoolant);
    visit("alert_cpu", nullptr, nullptr, &s.alertCpu);
    visit("alert_gpu", nullptr, nullptr, &s.alertGpu);
    visit("alert_psu", nullptr, nullptr, &s.alertPsu);
    visit("psu_auto_above", nullptr, nullptr, &s.psuAutoAbove);
    visit("history_window", &s.historyWindow, nullptr, nullptr);
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

}  // namespace

bool loadConfig(const char* path, Config& config) {
    FILE* f = openPath(path, "rb");
    if (!f) return false;
    config = Config();
    std::string section;
    char buffer[4096];
    while (fgets(buffer, sizeof buffer, f)) {
        std::string line = trim(buffer);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            section = line.substr(1, line.find(']') - 1);
            if (section == "curve") config.curves.emplace_back();
            if (section == "profile") config.profiles.emplace_back();
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (section == "settings") {
            if (key == "active_profile") config.activeProfile = value;
            forEachSetting(config.settings, [&](const char* name, int* i, bool* b, float* fl) {
                if (key != name) return;
                if (i) *i = atoi(value.c_str());
                if (b) *b = value == "1" || value == "true";
                if (fl) *fl = (float)atof(value.c_str());
            });
        } else if (section == "names") {
            config.names[key] = value;
        } else if (section == "curve" && !config.curves.empty()) {
            Curve& c = config.curves.back();
            if (key == "name") c.name = value;
            else if (key == "mode") c.mode = indexOf(kModeNames, 3, value, MODE_CURVE);
            else if (key == "unit") c.unit = indexOf(kUnitNames, 2, value, UNIT_PERCENT);
            else if (key == "sensor") c.sensor = value;
            else if (key == "fixed") c.fixed = (float)atof(value.c_str());
            else if (key == "hysteresis") c.hysteresis = (float)atof(value.c_str());
            else if (key == "auto_below") c.autoBelow = (float)atof(value.c_str());
            else if (key == "points") {
                c.points.clear();
                const char* p = value.c_str();
                while (*p) {
                    char* end;
                    float t = strtof(p, &end);
                    if (end == p || *end != ':') break;
                    float v = strtof(end + 1, &end);
                    c.points.push_back({t, v});
                    p = end;
                    while (*p == ' ') p++;
                }
                c.sortPoints();
            }
        } else if (section == "profile" && !config.profiles.empty()) {
            Profile& pr = config.profiles.back();
            if (key == "name") pr.name = value;
            else pr.assignments[key] = value;
        }
    }
    fclose(f);
    if (config.settings.pollSeconds < 1) config.settings.pollSeconds = 1;
    if (config.settings.pollSeconds > 10) config.settings.pollSeconds = 10;
    if (config.settings.historyWindow < 0 || config.settings.historyWindow >= kHistoryWindowCount)
        config.settings.historyWindow = 3;
    config.active();
    return true;
}

bool saveConfig(const char* path, const Config& config) {
    std::string tmp = std::string(path) + ".tmp";
    FILE* f = openPath(tmp.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "# CorsairCommanderCommander settings\n\n[settings]\nactive_profile=%s\n", config.activeProfile.c_str());
    Settings s = config.settings;
    forEachSetting(s, [&](const char* name, int* i, bool* b, float* fl) {
        if (i) fprintf(f, "%s=%d\n", name, *i);
        if (b) fprintf(f, "%s=%d\n", name, *b ? 1 : 0);
        if (fl) fprintf(f, "%s=%g\n", name, *fl);
    });
    fprintf(f, "\n[names]\n");
    for (auto& n : config.names) fprintf(f, "%s=%s\n", n.first.c_str(), n.second.c_str());
    for (const Curve& c : config.curves) {
        fprintf(f, "\n[curve]\nname=%s\nmode=%s\nunit=%s\nsensor=%s\nfixed=%g\nhysteresis=%g\nauto_below=%g\npoints=",
                c.name.c_str(), kModeNames[c.mode], kUnitNames[c.unit], c.sensor.c_str(), c.fixed, c.hysteresis,
                c.autoBelow);
        for (size_t i = 0; i < c.points.size(); i++)
            fprintf(f, "%s%g:%g", i ? " " : "", c.points[i].temp, c.points[i].value);
        fprintf(f, "\n");
    }
    for (const Profile& p : config.profiles) {
        fprintf(f, "\n[profile]\nname=%s\n", p.name.c_str());
        for (auto& a : p.assignments) fprintf(f, "%s=%s\n", a.first.c_str(), a.second.c_str());
    }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) return false;
#ifdef _WIN32
    wchar_t from[1024], to[1024];
    MultiByteToWideChar(CP_UTF8, 0, tmp.c_str(), -1, from, 1024);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, to, 1024);
    return MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING) != FALSE;
#else
    return rename(tmp.c_str(), path) == 0;
#endif
}
