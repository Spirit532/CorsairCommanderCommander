#include "app.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace theme;

namespace {

const char* const kModeLabels[] = {"Curve", "Fixed", "Device auto"};
const char* const kUnitLabels[] = {"Percent", "RPM"};

const char* curveItem(int v, void* ctx) {
    Config* c = (Config*)ctx;
    return v < 0 || v >= (int)c->curves.size() ? "(none)" : c->curves[v].name.c_str();
}
const char* profileItem(int v, void* ctx) {
    Config* c = (Config*)ctx;
    return v < 0 || v >= (int)c->profiles.size() ? "" : c->profiles[v].name.c_str();
}
const char* modeItem(int v, void*) { return kModeLabels[v < 0 ? 0 : (v > 2 ? 2 : v)]; }
const char* unitItem(int v, void*) { return kUnitLabels[v ? 1 : 0]; }
const char* sensorItem(int v, void* ctx) {
    App* app = (App*)ctx;
    static std::string name;
    name = v < 0 || v >= (int)app->tempSensors.size() ? "(none)" : app->engine.nameOf(app->tempSensors[v]);
    return name.c_str();
}

std::string uniqueName(const Config& config, const std::string& base, bool profiles) {
    for (int n = 1;; n++) {
        std::string name = n == 1 ? base : base + " " + std::to_string(n);
        bool taken = false;
        if (profiles) {
            for (auto& p : config.profiles) taken = taken || p.name == name;
        } else {
            for (auto& c : config.curves) taken = taken || c.name == name;
        }
        if (!taken) return name;
    }
}

}  // namespace

// Lifecycle ----------------------------------------------------------------------------------

void App::init(const std::string& configPath, const AppHooks& appHooks) {
    configPath_ = configPath;
    hooks = appHooks;
    if (!loadConfig(configPath_.c_str(), config)) {
        config = Config();
        importFromIcue(true);
        if (config.curves.empty()) {  // no iCUE: a sensible starting point
            Curve c;
            c.name = "Default";
            c.unit = UNIT_PERCENT;
            c.sensor = "cp.t0";
            c.points = {{25, 0}, {30, 25}, {35, 45}, {40, 70}, {45, 100}};
            config.curves.push_back(c);
        }
        config.active();
        save();
    }
    if (hooks.setStartWithWindows && config.settings.startWithWindows) hooks.setStartWithWindows(true);
    engine.start(&config);
    // Open the editor on the first curve a fan actually uses.
    for (auto& a : config.active()->assignments)
        for (size_t k = 0; k < config.curves.size(); k++)
            if (config.curves[k].name == a.second && config.curves[k].mode == MODE_CURVE) {
                curveIndex_ = (int)k;
                return;
            }
}

void App::save() {
    if (saveConfig(configPath_.c_str(), config)) configDirty = false;
}

void App::revert() {
    Config loaded;
    if (!loadConfig(configPath_.c_str(), loaded)) return;
    config = loaded;
    configDirty = false;
    engine.forceResend();
}

void App::importFromIcue(bool quiet) {
    std::string log = importIcue(hooks.icueFolder, config);
    if (!log.empty()) {
        markDirty();
        engine.forceResend();
    }
    if (!quiet && hooks.message)
        hooks.message("Import from iCUE", log.empty() ? "No iCUE cooling setup found." : log.c_str());
}

void App::setActiveProfile(const std::string& name) {
    config.activeProfile = name;
    for (Output& o : engine.outputs) o.heldTemp = NAN;
    engine.forceResend();
    markDirty();
}

std::string App::valueText(const Reading& r) const {
    char text[32];
    if (!r.present) return "--";
    if (r.unit == "C") snprintf(text, sizeof text, "%.1f C", r.value);
    else if (r.unit == "W") snprintf(text, sizeof text, "%.0f W", r.value);
    else if (r.unit == "A") snprintf(text, sizeof text, "%.1f A", r.value);
    else snprintf(text, sizeof text, "%.2f V", r.value);
    return text;
}

// Frame --------------------------------------------------------------------------------------

int App::dropdown(uintptr_t id, int x, int y, int w, int h, int current, int lo, int count, ItemNameFn name,
                  void* context) {
    auto it = drops_.find(id);
    int result = it != drops_.end() && it->second.value != it->second.shown ? it->second.value : current;
    if (result < lo || result >= lo + count) result = current;
    DropSlot& slot = drops_[id];
    slot.shown = slot.value = result;
    ui_.dropdown(id, x, y, w, h, &slot.value, lo, count, name, context);
    return result;
}

bool App::drawFrame(const UiInput& input, int scale) {
    if (!canvas_.resize(kWidth, kHeight, scale)) return false;
    tempSensors.clear();
    for (const Reading& r : engine.readings)
        if (r.isTemp) tempSensors.push_back(r.id);
    if (curveIndex_ >= (int)config.curves.size()) curveIndex_ = (int)config.curves.size() - 1;
    if (curveIndex_ < 0) curveIndex_ = 0;

    hoverId_.clear();
    ui_.begin(canvas_, input);
    canvas_.fillRect(0, 0, kWidth, kHeight, background);
    drawHeader();
    drawSensors(8, 38, 540, kHeight - 46);
    drawOutputs(556, 38, 318, kHeight - 46);
    drawCurveEditor(882, 38, 310, kHeight - 46);
    ui_.end();
    redrawNeeded = false;
    return ui_.changed;
}

void App::drawHeader() {
    Canvas& c = canvas_;
    c.fillRect(0, 0, kWidth, 30, panel);
    c.fillRect(0, 30, kWidth, 1, border);
    int x = c.text(12, 11, "Corsair", output, true);
    x = c.text(x + 4, 11, "Commander", theme::text, true);
    x = c.text(x + 4, 11, "Commander", dim, true);
    c.text(x + 6, 11, "v" APP_VERSION, faint);

    c.text(250, 11, "Profile", dim);
    int profile = 0;
    for (size_t i = 0; i < config.profiles.size(); i++)
        if (config.profiles[i].name == config.activeProfile) profile = (int)i;
    int before = profile;
    profile = dropdown(widgetId("profile"), 290, 5, 130, 20, profile, 0, (int)config.profiles.size(), profileItem, &config);
    if (profile != before && profile < (int)config.profiles.size()) setActiveProfile(config.profiles[profile].name);
    if (shownProfile_ != config.activeProfile) {
        shownProfile_ = config.activeProfile;
        profileNameBuffer_ = shownProfile_;
    }
    if (ui_.textField(widgetId("profile.name"), 424, 5, 110, 20, profileNameBuffer_, "name") &&
        !profileNameBuffer_.empty() && profileNameBuffer_ != config.activeProfile) {
        std::string name = uniqueName(config, profileNameBuffer_, true);
        config.active()->name = name;
        config.activeProfile = shownProfile_ = profileNameBuffer_ = name;
        markDirty();
    }
    ui_.hint("Rename the profile: type and press Enter");
    if (ui_.button(widgetId("profile.new"), 538, 5, 40, 20, "Copy")) {
        Profile copy = *config.active();
        copy.name = uniqueName(config, copy.name, true);
        config.profiles.push_back(copy);
        setActiveProfile(copy.name);
    }
    ui_.hint("New profile with the same assignments");
    if (ui_.button(widgetId("profile.delete"), 582, 5, 36, 20, "Del") && config.profiles.size() > 1) {
        for (size_t i = 0; i < config.profiles.size(); i++)
            if (config.profiles[i].name == config.activeProfile) {
                config.profiles.erase(config.profiles.begin() + i);
                break;
            }
        setActiveProfile(config.profiles.front().name);
    }

    struct Dot {
        const std::string* status;
        bool ok;
        const char* label;
    } dots[] = {
        {&engine.commanderStatus, engine.commanderOk(), "CP"},
        {&engine.psuStatus, engine.psuOk(), "PSU"},
        {&engine.cpuStatus, engine.cpuOk(), "CPU"},
        {&engine.gpuStatus, engine.gpuOk(), "GPU"},
    };
    int dx = 800;
    for (auto& d : dots) {
        c.disc(dx + 4, 15, 4, d.ok ? good : bad);
        int end = c.text(dx + 12, 11, d.label, d.ok ? theme::text : dim);
        ui_.drag(widgetId("dot", dx), dx, 5, end - dx, 20);  // hover target for the hint
        ui_.hint(d.status->c_str(), d.ok ? good : bad);
        dx = end + 12;
    }

    if (ui_.button(widgetId("import"), 996, 5, 76, 20, "Import iCUE")) importFromIcue(false);
    ui_.hint("Merge curves, assignments and names from iCUE");
    if (ui_.button(widgetId("save"), 1076, 5, 56, 20, "Save", configDirty, warn)) save();
    if (ui_.button(widgetId("revert"), 1136, 5, 56, 20, "Revert")) revert();
}

namespace {
const char* historyItem(int v, void*) {
    static const char* const names[kHistoryWindowCount] = {"30 s", "1 min", "2 min", "5 min",
                                                           "10 min", "15 min", "30 min", "1 h"};
    return names[v < 0 ? 0 : (v >= kHistoryWindowCount ? kHistoryWindowCount - 1 : v)];
}
}  // namespace

bool App::psuDriven(const Curve& curve) {
    auto& a = config.active()->assignments;
    auto it = a.find("psu.fan");
    return it != a.end() && it->second == curve.name;
}

void App::drawName(const std::string& id, int x, int y, int w, int h, uint32_t color, bool bold) {
    Canvas& c = canvas_;
    bool hover = ui_.hovered(x, y, w, h);
    if (editingId_ != id && hover && ui_.input.leftDouble) {
        editingId_ = id;
        nameBuffer_ = engine.nameOf(id);
    }
    if (editingId_ == id) {
        if (ui_.textField(widgetId("name", 0), x, y, w, h, nameBuffer_, engine.nameOf(id).c_str())) {
            bool isDefault = nameBuffer_.empty();
            for (const Reading& r : engine.readings) isDefault = isDefault || (r.id == id && r.defaultName == nameBuffer_);
            for (const Output& o : engine.outputs) isDefault = isDefault || (o.id == id && o.defaultName == nameBuffer_);
            if (isDefault)
                config.names.erase(id);  // empty or the default name: back to the default
            else
                config.names[id] = nameBuffer_;
            markDirty();
            editingId_.clear();
        } else if (!ui_.hasTextFocus() || ui_.input.key == 0x1B) {
            editingId_.clear();  // Escape or clicked elsewhere: cancel
        }
        return;
    }
    c.textClipped(x + 2, y + (h - 8) / 2, w - 4, engine.nameOf(id).c_str(), color, bold);
}

void App::drawLargeGraph(int x, int y, int w, int h) {
    Canvas& c = canvas_;
    if (h < 60) return;
    c.fillRect(x, y - 4, w, 1, border);
    std::string id = !hoverId_.empty() ? hoverId_ : graphId_;
    const History* history = nullptr;
    std::string unit = "rpm";
    float minSpan = 100;
    if (Reading* r = engine.reading(id)) {
        history = &r->history;
        unit = r->unit;
        minSpan = unit == "C" ? 2.0f : (unit == "W" ? 10.0f : (unit == "A" ? 0.5f : 0.05f));
    } else if (Output* o = engine.output(id)) {
        history = &o->history;
    }
    if (!history) {
        c.text(x + 9, y + 6, "Click or hover a sensor to graph it here.", faint);
        return;
    }
    char text[96];
    snprintf(text, sizeof text, "%s  (%s)", engine.nameOf(id).c_str(), historyItem(config.settings.historyWindow, nullptr));
    c.text(x + 9, y + 4, text, theme::text, true);
    int gx = x + 46, gy = y + 20, gw = w - 56, gh = h - 40;
    c.fillRect(gx, gy, gw, gh, field);
    c.strokeRect(gx, gy, gw, gh, border);
    for (int i = 1; i < 4; i++) c.fillRect(gx + 1, gy + gh * i / 4, gw - 2, 1, blendColor(field, border, 120));
    HistoryStats st = drawHistory(*history, gx + 1, gy + 2, gw - 2, gh - 4, id == hoverId_ ? warn : input, minSpan);
    if (!st.count) {
        c.text(gx + 8, gy + 8, "no samples yet", faint);
        return;
    }
    const char* fmt = unit == "C" ? "%.1f" : (unit == "V" ? "%.2f" : (unit == "A" ? "%.1f" : "%.0f"));
    for (int i = 0; i <= 2; i++) {
        float v = st.scaleHi - (st.scaleHi - st.scaleLo) * i / 2;
        snprintf(text, sizeof text, fmt, v);
        c.text(gx - 4 - c.textWidth(text), gy + 2 + (gh - 4) * i / 2 - 4, text, dim);
    }
    snprintf(text, sizeof text, "-%s", historyItem(config.settings.historyWindow, nullptr));
    c.text(gx, gy + gh + 4, text, dim);
    c.text(gx + gw - c.textWidth("now"), gy + gh + 4, "now", dim);
    char stats[96], f1[16], f2[16], f3[16];
    snprintf(f1, sizeof f1, fmt, st.min);
    snprintf(f2, sizeof f2, fmt, st.avg);
    snprintf(f3, sizeof f3, fmt, st.max);
    snprintf(stats, sizeof stats, "min %s  avg %s  max %s %s", f1, f2, f3, unit.c_str());
    c.text(gx + (gw - c.textWidth(stats)) / 2, gy + gh + 4, stats, theme::text);
}


// Min/max per pixel column over the selected span, scaled to the span's own range.
App::HistoryStats App::drawHistory(const History& history, int x, int y, int w, int h, uint32_t color, float minSpan) {
    Canvas& c = canvas_;
    HistoryStats stats;
    double span = kHistoryWindows[config.settings.historyWindow], start = engine.lastTick - span;
    std::vector<float> lo(w, INFINITY), hi(w, -INFINITY);
    double sum = 0;
    for (int k = 0; k < kHistorySamples; k++) {
        int i = (engine.historyHead - 1 - k + kHistorySamples) % kHistorySamples;
        double t = engine.historyTimes[i];
        if (t < start) break;
        float v = history.values[i];
        if (!std::isfinite(v)) continue;
        int col = (int)((t - start) / span * (w - 1));
        col = col < 0 ? 0 : (col >= w ? w - 1 : col);
        lo[col] = std::min(lo[col], v);
        hi[col] = std::max(hi[col], v);
        stats.min = std::min(stats.min, v);
        stats.max = std::max(stats.max, v);
        sum += v;
        stats.count++;
    }
    c.fillRect(x, y + h - 1, w, 1, blendColor(theme::panel, border, 128));
    if (!stats.count) return stats;
    stats.avg = (float)(sum / stats.count);
    float a = stats.min, b = stats.max;
    if (b - a < minSpan) {
        float mid = (a + b) / 2;
        a = mid - minSpan / 2;
        b = mid + minSpan / 2;
    }
    stats.scaleLo = a;
    stats.scaleHi = b;
    auto py = [&](float v) { return y + h - 1 - (int)lroundf((v - a) / (b - a) * (h - 1)); };
    float prevLo = NAN, prevHi = NAN;
    for (int col = 0; col < w; col++) {
        if (lo[col] > hi[col]) continue;
        float l = lo[col], u = hi[col];
        if (prevHi < l) l = prevHi;  // join to the previous column
        if (prevLo > u) u = prevLo;
        int top = py(u), bottom = py(l);
        c.fillRect(x + col, top, 1, bottom - top + 1, color);
        prevLo = lo[col];
        prevHi = hi[col];
    }
    return stats;
}

void App::drawSensors(int x, int y, int w, int h) {
    Canvas& c = canvas_;
    ui_.panel(x, y, w, h, "SENSORS", input);
    c.text(x + w - 170, y + 5, "History", dim);
    config.settings.historyWindow = dropdown(widgetId("history"), x + w - 124, y + 1, 120, 16,
                                             config.settings.historyWindow, 0, kHistoryWindowCount, historyItem, nullptr);
    const int graphX = x + 262, graphW = w - 272;
    int rowY = y + 24;
    auto section = [&](const char* title) {
        c.text(x + 10, rowY + 2, title, dim);
        rowY += 16;
    };
    char tip[128];
    auto row = [&](const std::string& id, const std::string& valueText, bool present, bool hot, const History& history,
                   const char* unit, float minSpan) {
        bool selected = graphId_ == id, hover = ui_.hovered(x + 4, rowY - 2, w - 8, 18);
        if (selected || hover) c.fillRect(x + 4, rowY - 2, w - 8, 18, selected ? fieldActive : fieldHover);
        if (hover) hoverId_ = id;
        if (hover && ui_.input.leftPressed) graphId_ = id;
        uint32_t color = !present ? faint : (hot ? bad : theme::text);
        drawName(id, x + 8, rowY - 1, 180, 16, color, false);
        c.text(x + 250 - c.textWidth(valueText.c_str()), rowY + 3, valueText.c_str(), color);
        HistoryStats st = drawHistory(history, graphX, rowY - 1, graphW, 15, hot ? bad : input, minSpan);
        ui_.drag(widgetId("hist", (int)(rowY)), graphX, rowY - 2, graphW, 18);  // hover target for the hint
        if (st.count) {
            snprintf(tip, sizeof tip, "%s  min %.1f  avg %.1f  max %.1f %s", historyItem(config.settings.historyWindow, nullptr),
                     st.min, st.avg, st.max, unit);
            ui_.hint(tip);
        }
        rowY += 18;
    };
    auto minSpanFor = [](const std::string& unit) {
        return unit == "C" ? 2.0f : (unit == "W" ? 10.0f : (unit == "A" ? 0.5f : 0.05f));
    };
    section("TEMPERATURES");
    for (const Reading& r : engine.readings) {
        if (!r.isTemp || !(r.present || r.id.rfind("cp.t", 0) == 0)) continue;
        bool hot = r.present && ((r.id.rfind("cp.t", 0) == 0 && r.value > config.settings.alertCoolant) ||
                                 (r.id == "cpu.tctl" && r.value > config.settings.alertCpu) ||
                                 (r.id == "gpu.temp" && r.value > config.settings.alertGpu));
        row(r.id, valueText(r), r.present, hot, r.history, "C", 2.0f);
    }
    rowY += 4;
    section("FANS");
    for (const Output& o : engine.outputs) {
        if (!o.present) continue;
        char v[24];
        snprintf(v, sizeof v, std::isfinite(o.rpm) ? "%.0f rpm" : "--", o.rpm);
        row(o.id, v, true, o.stallTicks >= 5, o.history, "rpm", 100.0f);
    }
    rowY += 4;
    section("POWER AND RAILS");
    for (const Reading& r : engine.readings)
        if (!r.isTemp && r.present) row(r.id, valueText(r), true, false, r.history, r.unit.c_str(), minSpanFor(r.unit));
    if (!engine.detailed) c.text(x + 10, rowY + 2, "(rails are read while this window is open)", faint);

    c.text(x + 10, y + h - 14, "Click: show in the large graph. Double click a name: rename. Hover a graph: min/avg/max.", faint);
}

void App::drawOutputs(int x, int y, int w, int h) {
    Canvas& c = canvas_;
    ui_.panel(x, y, w, h, "OUTPUTS", output);
    Profile* profile = config.active();
    int rowY = y + 24;
    for (size_t i = 0; i < engine.outputs.size(); i++) {
        Output& o = engine.outputs[i];
        bool selected = graphId_ == o.id, hover = ui_.hovered(x + 4, rowY - 2, w - 8, 14);
        if (selected || hover) c.fillRect(x + 4, rowY - 2, w - 8, 38, selected ? fieldActive : fieldHover);
        if (hover && o.present) hoverId_ = o.id;
        if (hover && ui_.input.leftPressed) {
            if (o.present) graphId_ = o.id;
            auto assigned = profile->assignments.find(o.id);
            for (size_t k = 0; assigned != profile->assignments.end() && k < config.curves.size(); k++)
                if (config.curves[k].name == assigned->second) curveIndex_ = (int)k;
        }
        drawName(o.id, x + 8, rowY - 3, 180, 15, o.present ? theme::text : faint, true);
        char rpm[48];
        if (o.present && std::isfinite(o.rpm))
            snprintf(rpm, sizeof rpm, "%.0f rpm", o.rpm);
        else
            snprintf(rpm, sizeof rpm, "%s", o.present ? "--" : "not connected");
        c.text(x + w - 12 - c.textWidth(rpm), rowY, rpm, o.present ? theme::text : faint);

        int curve = -1;
        auto it = profile->assignments.find(o.id);
        if (it != profile->assignments.end())
            for (size_t k = 0; k < config.curves.size(); k++)
                if (config.curves[k].name == it->second) curve = (int)k;
        int before = curve;
        curve = dropdown(widgetId("assign", (int)i), x + 10, rowY + 14, 150, 18, curve, -1,
                         (int)config.curves.size() + 1, curveItem, &config);
        if (curve != before) {
            if (curve < 0)
                profile->assignments.erase(o.id);
            else
                profile->assignments[o.id] = config.curves[curve].name;
            if (curve >= 0) curveIndex_ = curve;
            o.heldTemp = NAN;
            engine.forceResend();
            markDirty();
        }
        char state[96];
        if (o.isPsu && o.deviceMode)
            snprintf(state, sizeof state, "PSU auto  %s", o.state.c_str());
        else if (std::isfinite(o.command))
            snprintf(state, sizeof state, "%.0f%s  %s", o.command, o.commandUnit == UNIT_RPM ? " rpm" : " %",
                     o.state.c_str());
        else
            snprintf(state, sizeof state, "%s", o.state.c_str());
        c.textClipped(x + 168, rowY + 19, w - 180, state, dim);
        rowY += 42;
    }
    int graphY = drawSettings(x, rowY + 4, w) + 8;
    drawLargeGraph(x + 1, graphY, w - 2, y + h - graphY - 1);
}

int App::drawSettings(int x, int y, int w) {
    Canvas& c = canvas_;
    Settings& s = config.settings;
    c.fillRect(x + 1, y, w - 2, 1, border);
    c.text(x + 10, y + 8, "SETTINGS", dim);
    int rowY = y + 24;
    auto slider = [&](const char* label, float* value, float lo, float hi, float reset, const char* format, float snap,
                      const char* tip) {
        c.text(x + 10, rowY + 4, label, theme::text);
        if (ui_.slider(widgetId(value), x + 150, rowY, w - 160, 16, value, lo, hi, reset, format, snap)) markDirty();
        ui_.hint(tip);
        rowY += 20;
    };
    float poll = (float)s.pollSeconds;
    slider("Poll interval", &poll, 1, 10, 1, "%.0f s", 1, "How often sensors are read and fans set");
    s.pollSeconds = (int)lroundf(poll);
    slider("Failsafe fans", &s.failsafePercent, 0, 100, 70, "%.0f %%", 5,
           "Commander fans when their sensor is lost, and when this app exits");
    slider("Alert: probes", &s.alertCoolant, 25, 70, 45, "%.0f C", 1, "Any Commander probe above this");
    slider("Alert: CPU", &s.alertCpu, 60, 105, 95, "%.0f C", 1, "CPU Tctl above this");
    slider("Alert: GPU", &s.alertGpu, 60, 100, 88, "%.0f C", 1, "GPU above this");
    slider("Alert: PSU", &s.alertPsu, 40, 90, 70, "%.0f C", 1, "PSU temperature above this");
    slider("PSU auto above", &s.psuAutoAbove, 40, 80, 60, "%.0f C", 1,
           "The PSU fan returns to its own logic above this PSU temperature");
    rowY += 4;
    if (ui_.checkbox(widgetId(&s.startWithWindows), x + 10, rowY, "Start with Windows", &s.startWithWindows)) {
        if (hooks.setStartWithWindows) hooks.setStartWithWindows(s.startWithWindows);
        markDirty();
    }
    ui_.hint("Scheduled task at logon, elevated (PawnIO needs administrator)");
    if (ui_.checkbox(widgetId(&s.openWindowAtStart), x + 160, rowY, "Open window at start", &s.openWindowAtStart))
        markDirty();
    return rowY + 16;
}

void App::drawCurveEditor(int x, int y, int w, int h) {
    Canvas& c = canvas_;
    ui_.panel(x, y, w, h, "CURVE", good);
    int rowY = y + 24;
    int before = curveIndex_;
    curveIndex_ = dropdown(widgetId("curve"), x + 10, rowY, 150, 20, curveIndex_, 0, (int)config.curves.size(),
                           curveItem, &config);
    if (ui_.button(widgetId("curve.new"), x + 166, rowY, 42, 20, "New")) {
        Curve n;
        n.name = uniqueName(config, "Curve", false);
        n.sensor = tempSensors.empty() ? std::string() : tempSensors.front();
        n.points = {{25, 20}, {35, 50}, {45, 100}};
        config.curves.push_back(n);
        curveIndex_ = (int)config.curves.size() - 1;
        markDirty();
    }
    if (ui_.button(widgetId("curve.copy"), x + 212, rowY, 42, 20, "Copy") && !config.curves.empty()) {
        Curve n = config.curves[curveIndex_];
        n.name = uniqueName(config, n.name, false);
        config.curves.push_back(n);
        curveIndex_ = (int)config.curves.size() - 1;
        markDirty();
    }
    if (ui_.button(widgetId("curve.delete"), x + 258, rowY, 42, 20, "Del") && !config.curves.empty()) {
        std::string name = config.curves[curveIndex_].name;
        config.curves.erase(config.curves.begin() + curveIndex_);
        for (Profile& p : config.profiles)
            for (auto it = p.assignments.begin(); it != p.assignments.end();)
                it = it->second == name ? p.assignments.erase(it) : std::next(it);
        curveIndex_ = 0;
        markDirty();
    }
    ui_.hint("Delete this curve; outputs using it become unassigned");
    if (config.curves.empty()) {
        c.text(x + 10, rowY + 30, "No curves. Press New.", dim);
        return;
    }
    if (curveIndex_ != before) shownCurve_.clear();
    Curve& curve = config.curves[curveIndex_];
    if (shownCurve_ != curve.name) {
        shownCurve_ = curve.name;
        curveNameBuffer_ = curve.name;
    }
    rowY += 28;
    c.text(x + 10, rowY + 6, "Name", dim);
    if (ui_.textField(widgetId("curve.name"), x + 60, rowY, w - 70, 20, curveNameBuffer_, "name") &&
        !curveNameBuffer_.empty() && curveNameBuffer_ != curve.name) {
        std::string old = curve.name, name = uniqueName(config, curveNameBuffer_, false);
        for (Profile& p : config.profiles)
            for (auto& a : p.assignments)
                if (a.second == old) a.second = name;
        curve.name = shownCurve_ = curveNameBuffer_ = name;
        markDirty();
    }
    ui_.hint("Type and press Enter");
    rowY += 26;
    c.text(x + 10, rowY + 6, "Mode", dim);
    int mode = dropdown(widgetId("curve.mode"), x + 60, rowY, 110, 20, curve.mode, 0, 3, modeItem, nullptr);
    int unit = dropdown(widgetId("curve.unit"), x + 176, rowY, 80, 20, curve.unit, 0, 2, unitItem, nullptr);
    ui_.hint("RPM: the Commander regulates to the speed. Percent: fixed duty (0 % stops PWM fans)");
    if (mode != curve.mode || unit != curve.unit) {
        if (unit != curve.unit) {  // keep the curve's shape on the new scale
            float k = unit == UNIT_RPM ? 50.0f : 0.02f;
            for (CurvePoint& p : curve.points) p.value = roundf(p.value * k);
            curve.fixed = roundf(curve.fixed * k);
        }
        curve.mode = mode;
        curve.unit = unit;
        engine.forceResend();
        markDirty();
    }
    rowY += 26;
    c.text(x + 10, rowY + 6, "Sensor", dim);
    int sensor = -1;
    for (size_t i = 0; i < tempSensors.size(); i++)
        if (tempSensors[i] == curve.sensor) sensor = (int)i;
    int sensorBefore = sensor;
    sensor = dropdown(widgetId("curve.sensor"), x + 60, rowY, 196, 20, sensor, -1, (int)tempSensors.size() + 1,
                      sensorItem, this);
    if (sensor != sensorBefore) {
        curve.sensor = sensor >= 0 ? tempSensors[sensor] : std::string();
        markDirty();
    }
    if (Reading* r = engine.reading(curve.sensor)) {
        std::string v = valueText(*r);
        c.text(x + w - 10 - c.textWidth(v.c_str()), rowY + 6, v.c_str(), good);
    }
    rowY += 26;
    c.text(x + 10, rowY + 4, "Hysteresis", dim);
    if (ui_.slider(widgetId(&curve.hysteresis), x + 80, rowY, w - 90, 16, &curve.hysteresis, 0, 5, 1, "%.1f C", 0.5f))
        markDirty();
    ui_.hint("Degrees the temperature must drop before the fans slow down");
    rowY += 22;
    if (curve.mode == MODE_FIXED && psuDriven(curve)) {  // curves get a draggable marker on the graph instead
        c.text(x + 10, rowY + 4, "PSU auto <", dim);
        if (ui_.slider(widgetId(&curve.autoBelow), x + 80, rowY, w - 90, 16, &curve.autoBelow, 0, 80, 0,
                       curve.autoBelow > 0 ? "%.0f C" : "off", 1))
            markDirty();
        ui_.hint("PSU only: below this the PSU's own fan logic runs (zero RPM). 0 = off");
        rowY += 22;
    }

    if (curve.mode == MODE_FIXED) {
        float hi = curve.unit == UNIT_RPM ? 5000.0f : 100.0f;
        c.text(x + 10, rowY + 4, "Value", dim);
        if (ui_.slider(widgetId(&curve.fixed), x + 80, rowY, w - 90, 16, &curve.fixed, 0, hi,
                       curve.unit == UNIT_RPM ? 1000.0f : 40.0f, curve.unit == UNIT_RPM ? "%.0f rpm" : "%.0f %%",
                       curve.unit == UNIT_RPM ? 100.0f : 5.0f))
            markDirty();
        curve.fixed = roundf(curve.fixed);
        rowY += 24;
    } else if (curve.mode == MODE_CURVE) {
        drawGraph(curve, x + 10, rowY, w - 20, 250);
        rowY += 258;
    } else {
        c.text(x + 10, rowY + 4, "The device runs its own fan logic (PSU only).", dim);
        rowY += 20;
    }

    // Who uses this curve in the active profile.
    std::string users;
    for (auto& a : config.active()->assignments)
        if (a.second == curve.name) users += (users.empty() ? "" : ", ") + engine.nameOf(a.first);
    c.text(x + 10, rowY + 4, "Used by", dim);
    c.textClipped(x + 60, rowY + 4, w - 70, users.empty() ? "nothing in this profile" : users.c_str(), theme::text);
    rowY += 18;
    if (curve.mode == MODE_CURVE) {
        c.text(x + 10, rowY + 4, "Drag points. Right-click: add or remove a point.", faint);
        c.text(x + 10, rowY + 18, psuDriven(curve) ? "Wheel: fine-tune. Shift: snap. A: PSU auto below."
                                                   : "Wheel over a point: fine-tune. Shift: snap.", faint);
    }
}

void App::drawGraph(Curve& curve, int x, int y, int w, int h) {
    Canvas& c = canvas_;
    const bool rpm = curve.unit == UNIT_RPM;
    if (curve.points.empty()) curve.points = {{25, rpm ? 500.0f : 20.0f}, {45, rpm ? 2000.0f : 100.0f}};
    // Scale; frozen while a point is dragged so it doesn't run away under the mouse.
    if (!graphDragging_) {
        float tMax = 60, vMax = rpm ? 1000.0f : 100.0f;
        for (const CurvePoint& p : curve.points) {
            tMax = std::max(tMax, p.temp + 10);
            vMax = std::max(vMax, p.value);
        }
        graphTMax_ = std::min(120.0f, ceilf(tMax / 10) * 10);
        graphYMax_ = rpm ? (vMax <= 2500 ? 2500.0f : 5000.0f) : 100.0f;
    }
    const float tMin = 10, tMax = graphTMax_, yMax = graphYMax_;
    int gx = x + 34, gy = y + 4, gw = w - 40, gh = h - 22;
    auto px = [&](float t) { return gx + (int)lroundf((t - tMin) / (tMax - tMin) * gw); };
    auto py = [&](float v) { return gy + gh - (int)lroundf(v / yMax * gh); };
    auto toTemp = [&](int mx) { return tMin + (float)(mx - gx) / gw * (tMax - tMin); };
    auto toValue = [&](int my) { return (float)(gy + gh - my) / gh * yMax; };

    c.fillRect(gx, gy, gw, gh, field);
    char label[24];
    for (float t = tMin; t <= tMax + 0.1f; t += 10) {
        c.fillRect(px(t), gy, 1, gh, blendColor(field, border, 160));
        snprintf(label, sizeof label, "%.0f", t);
        c.text(px(t) - c.textWidth(label) / 2, gy + gh + 4, label, dim);
    }
    float step = rpm ? (yMax > 2500 ? 1000.0f : 500.0f) : 20.0f;
    for (float v = 0; v <= yMax + 0.1f; v += step) {
        c.fillRect(gx, py(v), gw, 1, blendColor(field, border, 160));
        snprintf(label, sizeof label, rpm ? "%.0f" : "%.0f%%", v);
        c.text(gx - 4 - c.textWidth(label), py(v) - 4, label, dim);
    }
    c.strokeRect(gx, gy, gw, gh, border);

    // PSU: below the auto marker the PSU runs its own fan logic.
    const bool psu = psuDriven(curve);
    if (psu && curve.autoBelow > tMin) {
        int ax = px(std::min(curve.autoBelow, tMax));
        c.fillRect(gx + 1, gy + 1, ax - gx - 1, gh - 2, blendColor(field, output, 40));
        c.text(gx + 6, gy + gh - 14, "PSU auto", output);
    }

    // Curve line, flat beyond the ends.
    int lastX = gx, lastY = py(curve.points.front().value);
    for (const CurvePoint& p : curve.points) {
        c.line(lastX, lastY, px(p.temp), py(p.value), good);
        lastX = px(p.temp);
        lastY = py(p.value);
    }
    c.line(lastX, lastY, gx + gw, lastY, good);

    // Live temperature and resulting value.
    if (Reading* r = engine.reading(curve.sensor))
        if (r->present && r->value >= tMin && r->value <= tMax) {
            c.fillRect(px(r->value), gy + 1, 1, gh - 2, warn);
            float v = curve.evaluate(r->value);
            c.disc(px(r->value), py(std::min(v, yMax)), 3, warn);
            snprintf(label, sizeof label, rpm ? "%.0f rpm" : "%.0f %%", v);
            c.text(std::min(px(r->value) + 6, gx + gw - 50), gy + 18, label, warn);
        }

    // Points.
    bool dragging = false;
    int removeIndex = -1;
    const UiInput& in = ui_.input;
    for (size_t i = 0; i < curve.points.size(); i++) {
        CurvePoint& p = curve.points[i];
        int cx = px(p.temp), cy = py(p.value);
        uintptr_t id = widgetId("point", (int)i);
        bool held = ui_.drag(id, cx - 6, cy - 6, 13, 13);
        bool hover = ui_.hovered(cx - 6, cy - 6, 13, 13);
        if (held) {
            dragging = true;
            float lo = i > 0 ? curve.points[i - 1].temp + 0.5f : tMin;
            float hi = i + 1 < curve.points.size() ? curve.points[i + 1].temp - 0.5f : tMax;
            float t = std::min(hi, std::max(lo, toTemp(in.mouseX)));
            float v = std::min(yMax, std::max(0.0f, toValue(in.mouseY)));
            float tSnap = in.shiftDown ? 1.0f : 0.5f, vSnap = rpm ? (in.shiftDown ? 100.0f : 10.0f) : (in.shiftDown ? 5.0f : 1.0f);
            p.temp = std::min(hi, std::max(lo, roundf(t / tSnap) * tSnap));
            p.value = roundf(v / vSnap) * vSnap;
            markDirty();
        }
        if (hover && in.wheel) {
            p.value = std::min(yMax, std::max(0.0f, p.value + in.wheel * (rpm ? 50.0f : 1.0f)));
            markDirty();
        }
        if (hover && in.rightPressed && curve.points.size() > 2) removeIndex = (int)i;
        c.disc(cx, cy, held || hover ? 5 : 4, held || hover ? warn : good);
        if (held || hover) {
            snprintf(label, sizeof label, rpm ? "%.1fC %.0f rpm" : "%.1fC %.0f%%", p.temp, p.value);
            c.text(std::min(cx + 8, gx + gw - 80), std::max(cy - 14, gy + 2), label, theme::text);
        }
    }
    // The auto marker: drag sideways, right-click or drag to the left edge to turn it off.
    if (psu) {
        float t = curve.autoBelow > tMin ? std::min(curve.autoBelow, tMax) : tMin;
        int ax = px(t);
        uintptr_t id = widgetId("autoBelow");
        bool held = ui_.drag(id, ax - 6, gy, 13, 14);
        bool hover = ui_.hovered(ax - 6, gy, 13, 14);
        if (held) {
            dragging = true;
            float nt = roundf(std::min(tMax, std::max(tMin, toTemp(in.mouseX))));
            curve.autoBelow = nt <= tMin ? 0 : nt;
            markDirty();
        }
        if (hover && in.rightPressed) {
            curve.autoBelow = 0;
            markDirty();
        }
        uint32_t color = held || hover ? warn : output;
        if (curve.autoBelow > tMin) c.fillRect(ax, gy + 1, 1, gh - 2, color);
        c.fillRect(ax - 5, gy + 1, 11, 11, color);
        c.text(ax - 2, gy + 2, "A", background, true);
        if (held || hover) {
            snprintf(label, sizeof label, curve.autoBelow > 0 ? "PSU auto below %.0fC" : "PSU auto: off", curve.autoBelow);
            c.text(std::min(ax + 9, gx + gw - 110), gy + 16, label, theme::text);
        }
        ui_.hint("Below this the PSU runs its own fan logic (zero RPM). Drag; right-click: off");
    }
    graphDragging_ = dragging;
    if (removeIndex >= 0) {
        curve.points.erase(curve.points.begin() + removeIndex);
        markDirty();
    } else if (in.rightPressed && ui_.hovered(gx, gy, gw, gh) && curve.points.size() < 12) {
        float t = roundf(toTemp(in.mouseX) * 2) / 2, v = roundf(toValue(in.mouseY) / (rpm ? 10 : 1)) * (rpm ? 10 : 1);
        bool clash = false;
        for (const CurvePoint& p : curve.points) clash = clash || fabsf(p.temp - t) < 0.5f;
        if (!clash) {
            curve.points.push_back({t, v});
            curve.sortPoints();
            markDirty();
        }
    }
}
