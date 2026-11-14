#include "win32/engine.h"
#include <windows.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cstdio>

namespace {
bool processRunning(const wchar_t* const* names, int count) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry = {sizeof entry};
    bool found = false;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry))
        for (int i = 0; i < count; i++)
            if (!_wcsicmp(entry.szExeFile, names[i])) found = true;
    CloseHandle(snapshot);
    return found;
}

std::string format(const char* fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof buffer, fmt, args);
    va_end(args);
    return buffer;
}
}  // namespace

void Engine::addReading(const char* id, const char* name, const char* unit, bool isTemp) {
    Reading r;
    r.id = id;
    r.defaultName = name;
    r.unit = unit;
    r.isTemp = isTemp;
    readings.push_back(r);
}

void Engine::start(Config* cfg) {
    config = cfg;
    readings.clear();
    outputs.clear();
    for (int i = 0; i < CommanderPro::kTemps; i++) {
        std::string id = format("cp.t%d", i), name = format("Probe %d", i + 1);
        addReading(id.c_str(), name.c_str(), "C", true);
    }
    addReading("cpu.tctl", "CPU Tctl", "C", true);
    addReading("cpu.ccd1", "CPU CCD1", "C", true);
    addReading("cpu.ccd2", "CPU CCD2", "C", true);
    addReading("gpu.temp", "GPU", "C", true);
    addReading("psu.tvrm", "PSU VRM", "C", true);
    addReading("psu.tcase", "PSU case", "C", true);
    addReading("gpu.power", "GPU power", "W", false);
    addReading("psu.power", "PSU output", "W", false);
    addReading("psu.vin", "PSU input", "V", false);
    const char* rails[3] = {"12v", "5v", "3v3"};
    const char* railNames[3] = {"+12V", "+5V", "+3.3V"};
    for (int i = 0; i < 3; i++) {
        addReading(format("psu.%s.w", rails[i]).c_str(), format("PSU %s", railNames[i]).c_str(), "W", false);
        addReading(format("psu.%s.a", rails[i]).c_str(), format("PSU %s", railNames[i]).c_str(), "A", false);
        addReading(format("psu.%s.v", rails[i]).c_str(), format("PSU %s", railNames[i]).c_str(), "V", false);
    }
    for (int i = 0; i < 3; i++)
        addReading(format("cp.%s", rails[i]).c_str(), format("Commander %s", railNames[i]).c_str(), "V", false);
    for (int i = 0; i < CommanderPro::kFans; i++) {
        Output o;
        o.id = format("cp.f%d", i);
        o.defaultName = format("Fan %d", i + 1);
        o.channel = i;
        outputs.push_back(o);
    }
    Output psuFan;
    psuFan.id = "psu.fan";
    psuFan.defaultName = "PSU fan";
    psuFan.isPsu = true;
    outputs.push_back(psuFan);
    connectDevices();
}

void Engine::connectDevices() {
    if (!commander_.isOpen()) {
        commanderStatus = commander_.connect() ? "Commander Pro fw " + commander_.firmware : "Commander Pro not found";
        for (Output& o : outputs)
            if (!o.isPsu) o.present = commander_.isOpen() && commander_.fanType[o.channel] != CommanderPro::FAN_NONE;
        commanderFailures_ = 0;
    }
    if (!psu_.isOpen()) {
        psuStatus = psu_.connect() ? psu_.model : "PSU not found";
        output("psu.fan")->present = psu_.isOpen();
        psuFailures_ = 0;
    }
    std::string error;
    if (!cpu_.isOpen()) cpuStatus = cpu_.open(error) ? "CPU via PawnIO" : "CPU: " + error;
    if (!gpu_.isOpen()) gpuStatus = gpu_.open(error) ? "GPU via NVML" : "GPU: " + error;
    forceResend();
}

void Engine::resume(double now) {
    stop();
    connectDevices();
    tick(now);
}

void Engine::stop() {
    if (commander_.isOpen()) {
        for (Output& o : outputs)
            if (!o.isPsu && o.present) commander_.setFanPercent(o.channel, (int)config->settings.failsafePercent);
        commander_.disconnect();
    }
    psu_.disconnect();  // back to the PSU's own fan logic
    cpu_.close();
    gpu_.close();
    for (Reading& r : readings) r.present = false;
}

Reading* Engine::reading(const std::string& id) {
    for (Reading& r : readings)
        if (r.id == id) return &r;
    return nullptr;
}

Output* Engine::output(const std::string& id) {
    for (Output& o : outputs)
        if (o.id == id) return &o;
    return nullptr;
}

std::string Engine::nameOf(const std::string& id) const {
    auto it = config->names.find(id);
    if (it != config->names.end() && !it->second.empty()) return it->second;
    for (const Reading& r : readings)
        if (r.id == id) return r.defaultName;
    for (const Output& o : outputs)
        if (o.id == id) return o.defaultName;
    return id;
}

void Engine::setReading(const std::string& id, float value, bool present) {
    if (Reading* r = reading(id)) {
        r->present = present && std::isfinite(value);
        r->value = r->present ? value : NAN;
    }
}

void Engine::readAll() {
    if (commander_.isOpen()) {
        bool ok = true;
        for (int i = 0; i < CommanderPro::kTemps; i++) {
            float t = NAN;
            if (!commander_.tempConnected[i]) {
                setReading(format("cp.t%d", i), NAN, false);
                continue;
            }
            ok = commander_.readTemp(i, t) && ok;
            setReading(format("cp.t%d", i), t);
        }
        for (Output& o : outputs) {
            if (o.isPsu || !o.present) continue;
            int rpm = 0;
            o.rpm = commander_.readFanRpm(o.channel, rpm) ? (float)rpm : NAN;
            ok = ok && std::isfinite(o.rpm);
        }
        if (detailed) {
            const char* rails[3] = {"cp.12v", "cp.5v", "cp.3v3"};
            for (int i = 0; i < 3; i++) {
                float v = NAN;
                commander_.readRail(i, v);
                setReading(rails[i], v);
            }
        }
        commanderFailures_ = ok ? 0 : commanderFailures_ + 1;
        if (commanderFailures_ >= 5) {
            commander_.disconnect();
            commanderStatus = "Commander Pro lost";
        }
    }
    if (psu_.isOpen()) {
        CorsairPsu::Status s;
        if (psu_.read(s, detailed)) {
            psuFailures_ = 0;
            setReading("psu.tvrm", s.tempVrm);
            setReading("psu.tcase", s.tempCase);
            setReading("psu.power", s.outputWatts);
            setReading("psu.vin", s.inputVolts);
            output("psu.fan")->rpm = s.fanRpm;
            output("psu.fan")->deviceMode = !s.softwareFan;
            const char* rails[3] = {"12v", "5v", "3v3"};
            for (int i = 0; i < 3; i++) {
                setReading(format("psu.%s.v", rails[i]), s.rails[i].volts, s.hasRails);
                setReading(format("psu.%s.a", rails[i]), s.rails[i].amps, s.hasRails);
                setReading(format("psu.%s.w", rails[i]), s.rails[i].watts, s.hasRails);
            }
        } else if (++psuFailures_ >= 5) {
            psu_.disconnect();
            psuStatus = "PSU lost";
        }
    }
    float tctl = NAN, ccd[2] = {NAN, NAN}, gpuTemp = NAN, gpuWatts = NAN;
    if (cpu_.isOpen()) cpu_.read(tctl, ccd);
    setReading("cpu.tctl", tctl);
    setReading("cpu.ccd1", ccd[0]);
    setReading("cpu.ccd2", ccd[1]);
    if (gpu_.isOpen()) gpu_.read(gpuTemp, gpuWatts);
    setReading("gpu.temp", gpuTemp);
    setReading("gpu.power", gpuWatts);
}

float Engine::target(const Output& o, int& unit, bool& deviceMode, std::string& why) {
    deviceMode = false;
    unit = UNIT_PERCENT;
    Profile* profile = config->active();
    auto it = profile->assignments.find(o.id);
    Curve* curve = it == profile->assignments.end() ? nullptr : config->findCurve(it->second);
    if (!curve) {
        why = "unassigned";
        deviceMode = o.isPsu;
        return NAN;
    }
    unit = o.isPsu ? UNIT_PERCENT : curve->unit;
    if (curve->mode == MODE_DEVICE) {
        why = o.isPsu ? "auto" : "no device mode";
        deviceMode = o.isPsu;
        return NAN;
    }
    float value;
    bool needsSensor = curve->mode == MODE_CURVE || (o.isPsu && curve->autoBelow > 0);
    float held = NAN;
    if (needsSensor) {
        Reading* r = reading(curve->sensor);
        if (!r || !r->present) {
            why = "sensor lost";
            unit = UNIT_PERCENT;
            deviceMode = o.isPsu;
            return o.isPsu ? NAN : config->settings.failsafePercent;
        }
        held = holdTemperature(o, *curve, r->value);
    }
    // The PSU can't run its fan below about a third in software mode; below autoBelow its own logic
    // (zero-RPM at low load) takes over instead. Hysteresis comes from the held temperature.
    if (o.isPsu && curve->autoBelow > 0 && held < curve->autoBelow) {
        why = format("auto below %.0fC (%.1fC)", curve->autoBelow, held);
        deviceMode = true;
        return NAN;
    }
    if (curve->mode == MODE_FIXED) {
        value = curve->fixed;
        why = "fixed";
    } else {
        value = curve->evaluate(held);
        why = format("%.1fC", held);
    }
    if (o.isPsu && curve->unit == UNIT_RPM) value = value / 50.0f;  // rpm curves on the PSU: 5000 = 100 %
    return value;
}

// Follows the temperature up at once and down only once it has dropped by the curve's hysteresis.
float Engine::holdTemperature(const Output& o, const Curve& curve, float current) const {
    float held = std::isfinite(o.heldTemp) ? o.heldTemp : current;
    if (held < current) held = current;
    if (held > current + curve.hysteresis) held = current + curve.hysteresis;
    return held;
}

void Engine::drive(double now) {
    float psuTemp = NAN;
    if (Reading* a = reading("psu.tvrm"))
        if (a->present) psuTemp = a->value;
    if (Reading* b = reading("psu.tcase"))
        if (b->present && !(psuTemp >= b->value)) psuTemp = b->value;
    if (std::isfinite(psuTemp)) {
        if (psuTemp > config->settings.psuAutoAbove) psuHot_ = true;
        if (psuTemp < config->settings.psuAutoAbove - 5) psuHot_ = false;
    }
    for (Output& o : outputs) {
        if (!o.present) {
            o.state = "not connected";
            continue;
        }
        int unit;
        bool deviceMode;
        std::string why;
        float value = target(o, unit, deviceMode, why);
        // Remember the hysteresis-held temperature for the next tick.
        Profile* profile = config->active();
        auto it = profile->assignments.find(o.id);
        if (Curve* c = it == profile->assignments.end() ? nullptr : config->findCurve(it->second))
            if (c->mode == MODE_CURVE || (o.isPsu && c->autoBelow > 0))
                if (Reading* r = reading(c->sensor); r && r->present) o.heldTemp = holdTemperature(o, *c, r->value);
        if (o.isPsu && psuHot_) {
            deviceMode = true;
            why = "PSU hot: auto";
        }
        o.state = why;
        bool resend = now - o.lastSent > 30;
        if (o.isPsu) {
            if (deviceMode) {
                if ((!o.deviceMode || resend) && psu_.setHardwareFan()) o.lastSent = now;
                o.deviceMode = true;
                o.command = NAN;
            } else if (std::isfinite(value)) {
                int pct = (int)lroundf(value);
                if (o.deviceMode || !(fabsf(pct - o.command) < 1) || resend) {
                    if (psu_.setFanPercent(pct)) {
                        o.command = (float)pct;
                        o.lastSent = now;
                    }
                }
                o.deviceMode = false;
                o.commandUnit = UNIT_PERCENT;
            } else {
                continue;
            }
        } else {
            if (!std::isfinite(value)) {
                o.command = NAN;  // unassigned: leave the header alone
                continue;
            }
            int v = (int)lroundf(value);
            bool changed = unit != o.commandUnit || !(fabsf(v - o.command) < (unit == UNIT_RPM ? 10 : 1));
            if (changed || resend) {
                bool ok = unit == UNIT_RPM ? commander_.setFanRpm(o.channel, v) : commander_.setFanPercent(o.channel, v);
                if (ok) {
                    o.command = (float)v;
                    o.commandUnit = unit;
                    o.lastSent = now;
                }
            }
        }
    }
}

void Engine::checkAlerts() {
    alerts.clear();
    const Settings& s = config->settings;
    auto add = [&](const std::string& key, const std::string& message) { alerts.push_back({key, message}); };
    for (Reading& r : readings) {
        if (!r.isTemp) continue;
        if (r.id.rfind("cp.t", 0) == 0) {
            int probe = r.id[4] - '0';
            if (commander_.isOpen() && commander_.tempConnected[probe] && (!r.present || r.value <= 1 || r.value > 100))
                add("bad." + r.id, "Commander probe \"" + nameOf(r.id) + "\" reads nonsense. Check the probe cable.");
            else if (r.present && r.value > s.alertCoolant)
                add("hot." + r.id, format("%s is at %.1f C (limit %.0f C).", nameOf(r.id).c_str(), r.value, s.alertCoolant));
        }
        if (!r.present) continue;
        if (r.id == "cpu.tctl" && r.value > s.alertCpu)
            add("hot.cpu", format("CPU is at %.1f C (limit %.0f C).", r.value, s.alertCpu));
        if (r.id == "gpu.temp" && r.value > s.alertGpu)
            add("hot.gpu", format("GPU is at %.1f C (limit %.0f C).", r.value, s.alertGpu));
        if ((r.id == "psu.tvrm" || r.id == "psu.tcase") && r.value > s.alertPsu)
            add("hot.psu", format("PSU is at %.1f C (limit %.0f C).", r.value, s.alertPsu));
        if (r.value < -20 || r.value > 125) add("bad." + r.id, nameOf(r.id) + " reads an impossible temperature.");
    }
    for (Output& o : outputs) {
        bool spinning = !(o.rpm < 1);
        bool shouldSpin = o.isPsu ? (!o.deviceMode && o.command >= 30)
                                  : (o.commandUnit == UNIT_RPM ? o.command >= 400 : o.command >= 30);
        o.stallTicks = (o.present && shouldSpin && !spinning) ? o.stallTicks + 1 : 0;
        if (o.stallTicks >= 5) add("stall." + o.id, nameOf(o.id) + " is not spinning although it should.");
    }
    if (commanderStatus == "Commander Pro lost") add("lost.cp", "The Commander Pro stopped responding.");
    if (psuStatus == "PSU lost") add("lost.psu", "The PSU stopped responding.");
    if (icueRunning_)
        add("icue", "iCUE or the Corsair service is running and talking to the same devices.\n"
                    "Stop and disable the Corsair services, or readings and fan control will glitch.");
}

void Engine::tick(double now) {
    if (ticks_ % 30 == 0) {
        static const wchar_t* const icue[] = {L"Corsair.Service.exe", L"iCUE.exe"};
        icueRunning_ = processRunning(icue, 2);
        if (!commander_.isOpen() || !psu_.isOpen()) connectDevices();
    }
    ticks_++;
    readAll();
    drive(now);
    checkAlerts();
    record(now);
}

void Engine::record(double now) {
    lastTick = now;
    historyTimes[historyHead] = now;
    for (Reading& r : readings) r.history.values[historyHead] = r.present ? r.value : NAN;
    for (Output& o : outputs) o.history.values[historyHead] = o.present ? o.rpm : NAN;
    historyHead = (historyHead + 1) % kHistorySamples;
}
