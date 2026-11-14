// The settings window: sensors, outputs with their curves, and the curve editor.
#pragma once
#include <map>
#include <string>
#include "core/model.h"
#include "ui/widgets.h"
#include "win32/engine.h"

struct AppHooks {
    void (*message)(const char* title, const char* text) = nullptr;
    void (*setStartWithWindows)(bool enabled) = nullptr;
    std::string icueFolder;  // %APPDATA%\Corsair\CUE5
};

class App {
public:
    static const int kWidth = 1200, kHeight = 760;

    Config config;
    Engine engine;
    AppHooks hooks;
    bool configDirty = false;
    bool redrawNeeded = true;

    // Loads the config; on first run imports iCUE's setup. Starts the engine.
    void init(const std::string& configPath, const AppHooks& appHooks);
    void save();
    void revert();
    void importFromIcue(bool quiet);
    void setActiveProfile(const std::string& name);

    bool drawFrame(const UiInput& input, int scale);  // true: draw again
    const Canvas& canvas() const { return canvas_; }
    void releaseCanvas() { canvas_.release(); }

    std::vector<std::string> tempSensors;  // ids of the temperature readings, for the sensor dropdown

private:
    std::string configPath_;
    Canvas canvas_;
    Ui ui_;
    int curveIndex_ = 0;
    std::string graphId_;      // reading or output shown in the large graph (clicked)
    std::string hoverId_;      // row under the mouse this frame: shown in the large graph meanwhile
    std::string editingId_;    // name being edited inline (double click)
    std::string nameBuffer_, curveNameBuffer_, profileNameBuffer_;
    std::string shownCurve_, shownProfile_;
    float graphTMax_ = 60, graphYMax_ = 100;
    bool graphDragging_ = false;

    // Dropdowns write their pick in Ui::end(), after the frame's code ran, so the target must outlive the
    // frame: each dropdown keeps a slot, and a pick shows up as value != shown on the next frame.
    struct DropSlot {
        int shown = 0, value = 0;
    };
    std::map<uintptr_t, DropSlot> drops_;
    int dropdown(uintptr_t id, int x, int y, int w, int h, int current, int lo, int count, ItemNameFn name,
                 void* context);

    struct HistoryStats {
        float min = INFINITY, max = -INFINITY, avg = NAN;
        float scaleLo = 0, scaleHi = 1;  // the vertical range drawn
        int count = 0;
    };
    // A name that turns into a text field on double click.
    void drawName(const std::string& id, int x, int y, int w, int h, uint32_t color, bool bold);
    void drawLargeGraph(int x, int y, int w, int h);
    bool psuDriven(const Curve& curve);
    HistoryStats drawHistory(const History& history, int x, int y, int w, int h, uint32_t color, float minSpan);

    void markDirty() { configDirty = true; }
    void drawHeader();
    void drawSensors(int x, int y, int w, int h);
    void drawOutputs(int x, int y, int w, int h);
    int drawSettings(int x, int y, int w);  // returns the y below
    void drawCurveEditor(int x, int y, int w, int h);
    void drawGraph(Curve& curve, int x, int y, int w, int h);
    std::string valueText(const Reading& r) const;
};
