#include "win32/debug_tools.h"
#include <windows.h>
#include <cmath>

int writeScreenshot(App& app, const wchar_t* bmpPath, const wchar_t* configPath, wchar_t** clicks, int clickCount) {
    AppHooks hooks;
    char path[1024];
    WideCharToMultiByte(CP_UTF8, 0, configPath, -1, path, sizeof path, nullptr, nullptr);
    app.init(path, hooks);
    float demo[] = {31.4f, 29.8f, 24.1f, 33.0f, 46.5f, 44.0f, 41.2f, 38.0f, 35.2f, 33.1f, 42, 118, 233.5f};
    size_t i = 0;
    for (Reading& r : app.engine.readings) {
        if (i < sizeof demo / sizeof demo[0]) {
            r.value = demo[i++];
            r.present = true;
        }
    }
    for (int k = 0; k < 900; k++) {  // 15 min of fake history
        app.engine.historyTimes[k] = k;
        int ri = 0;
        for (Reading& r : app.engine.readings)
            r.history.values[k] = r.present ? r.value + 2.0f * sinf(k / (40.0f + 7 * ri++)) : NAN;
        for (Output& o : app.engine.outputs) o.history.values[k] = 600 + 150 * sinf(k / 60.0f + o.channel);
    }
    app.engine.historyHead = 900;
    app.engine.lastTick = 899;
    float rpms[] = {612, 655, 410, 380, 598, 2510, 0};
    for (size_t k = 0; k < app.engine.outputs.size(); k++) {
        Output& o = app.engine.outputs[k];
        o.present = true;
        o.rpm = rpms[k];
        int unit;
        bool device;
        o.command = app.engine.target(o, unit, device, o.state);
        o.commandUnit = unit;
        o.deviceMode = device;
    }
    UiInput input;
    for (int i = 0; i + 1 < clickCount; i += 2) {  // "x y" pairs: press and release there
        input.mouseX = _wtoi(clicks[i]);
        input.mouseY = _wtoi(clicks[i + 1]);
        app.drawFrame(input, 1);
        input.leftDown = input.leftPressed = true;
        app.drawFrame(input, 1);
        input.clearEdges();
        input.leftDown = false;
        input.leftReleased = true;
        app.drawFrame(input, 1);
        input.clearEdges();
    }
    input.mouseX = 760;
    input.mouseY = 330;
    app.drawFrame(input, 1);
    app.drawFrame(input, 1);
    const Canvas& c = app.canvas();
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof ih;
    ih.biWidth = c.width;
    ih.biHeight = -c.height;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + c.width * c.height * 4;
    HANDLE f = CreateFileW(bmpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w;
    WriteFile(f, &fh, sizeof fh, &w, nullptr);
    WriteFile(f, &ih, sizeof ih, &w, nullptr);
    WriteFile(f, c.pixels, c.width * c.height * 4, &w, nullptr);
    CloseHandle(f);
    return 0;
}
