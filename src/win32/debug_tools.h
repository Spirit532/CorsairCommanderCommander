// Development aid: renders the window with demo readings to a .bmp, optionally after simulated clicks.
//   CorsairCommanderCommander.exe --screenshot out.bmp config.ini [x y]...
#pragma once
#include "app.h"

int writeScreenshot(App& app, const wchar_t* bmpPath, const wchar_t* configPath, wchar_t** clicks, int clickCount);
