// Windows host: tray icon, 1 Hz poll timer, settings window, alerts, power events.
// Nothing runs between polls: no redraw timer, no timer-resolution requests.
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <cstdio>
#include <set>
#include <string>
#include <vector>
#include "app.h"
#include "win32/debug_tools.h"

#ifndef WM_GETDPISCALEDSIZE
#define WM_GETDPISCALEDSIZE 0x02E4
#endif

namespace {

const wchar_t* kAppName = L"CorsairCommanderCommander";
const wchar_t* kHostClass = L"CorsairCommanderCommander_Host";
const wchar_t* kWindowClass = L"CorsairCommanderCommander_Window";
const UINT WM_TRAY_ICON = WM_APP + 1;
const UINT WM_SHOW_SETTINGS = WM_APP + 2;
const UINT WM_ALERT_CLOSED = WM_APP + 3;
enum TimerId { TIMER_POLL = 1 };
enum MenuCommand { COMMAND_OPEN_WINDOW = 1, COMMAND_QUIT, COMMAND_PROFILE_BASE = 100 };
const DWORD kWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
const COLORREF kBackgroundColor = RGB(0x1B, 0x1D, 0x21);

App g_app;
HINSTANCE g_instance;
HWND g_host = nullptr, g_window = nullptr;
UINT g_taskbarCreatedMessage = 0;
HICON g_iconSmall = nullptr, g_iconLarge = nullptr;
bool g_trayAdded = false;
UINT g_dpi = 96;
int g_scale = 1;
int g_pollSeconds = 0;
UiInput g_input;
bool g_trackingMouseLeave = false;
double g_secondsPerTick = 0;
std::set<std::string> g_firedAlerts;  // shown and still active; re-armed when the condition clears
int g_openAlertBoxes = 0;

double now() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * g_secondsPerTick;
}

std::wstring toWide(const std::string& text) {
    int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 1) return std::wstring();
    std::wstring result(length - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &result[0], length);
    return result;
}

std::string toUtf8(const std::wstring& text) {
    int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return std::string();
    std::string result(length - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &result[0], length, nullptr, nullptr);
    return result;
}

std::wstring appDataPath(const wchar_t* sub) {
    wchar_t appData[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    return ((length && length < MAX_PATH) ? std::wstring(appData) : std::wstring(L".")) + sub;
}

// Hooks --------------------------------------------------------------------------------------

void showMessage(const char* title, const char* text) {
    MessageBoxW(g_window, toWide(text).c_str(), toWide(title).c_str(), MB_OK | MB_ICONINFORMATION);
}

// Elevated logon task: PawnIO needs administrator rights. No 72 h limit, runs on battery.
void setStartWithWindows(bool enabled) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command;
    std::wstring xmlPath;
    if (enabled) {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        xmlPath = std::wstring(temp) + L"CorsairCommanderCommander_task.xml";
        std::wstring xml =
            L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
            L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
            L"<Triggers><LogonTrigger><Enabled>true</Enabled><Delay>PT5S</Delay></LogonTrigger></Triggers>\r\n"
            L"<Principals><Principal id=\"Author\"><LogonType>InteractiveToken</LogonType>"
            L"<RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
            L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
            L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
            L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
            L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>5</Priority></Settings>\r\n"
            L"<Actions Context=\"Author\"><Exec><Command>" +
            std::wstring(exe) + L"</Command></Exec></Actions>\r\n</Task>\r\n";
        HANDLE file = CreateFileW(xmlPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written;
        const wchar_t bom = 0xFEFF;
        WriteFile(file, &bom, 2, &written, nullptr);
        WriteFile(file, xml.c_str(), (DWORD)(xml.size() * 2), &written, nullptr);
        CloseHandle(file);
        command = L"schtasks.exe /Create /F /TN \"CorsairCommanderCommander\" /XML \"" + xmlPath + L"\"";
    } else {
        command = L"schtasks.exe /Delete /F /TN \"CorsairCommanderCommander\"";
    }
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (!xmlPath.empty()) DeleteFileW(xmlPath.c_str());
}

// Alerts: one message box per condition, each on its own thread so polling continues.
DWORD WINAPI alertThread(LPVOID parameter) {
    std::wstring* text = (std::wstring*)parameter;
    MessageBoxW(nullptr, text->c_str(), L"CorsairCommanderCommander alert",
                MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST | MB_SYSTEMMODAL);
    delete text;
    PostMessageW(g_host, WM_ALERT_CLOSED, 0, 0);
    return 0;
}

void handleAlerts() {
    std::set<std::string> active;
    for (auto& alert : g_app.engine.alerts) {
        active.insert(alert.first);
        if (g_firedAlerts.count(alert.first) || g_openAlertBoxes >= 4) continue;
        g_firedAlerts.insert(alert.first);
        g_openAlertBoxes++;
        HANDLE thread = CreateThread(nullptr, 0, alertThread, new std::wstring(toWide(alert.second)), 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    for (auto it = g_firedAlerts.begin(); it != g_firedAlerts.end();)
        it = active.count(*it) ? std::next(it) : g_firedAlerts.erase(it);
}

// DPI ----------------------------------------------------------------------------------------

UINT dpiForWindow(HWND window) {
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    static auto getDpi = (GetDpiForWindowFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (getDpi) return getDpi(window);
    HDC dc = GetDC(nullptr);
    UINT dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    return dpi;
}

int scaleForDpi(UINT dpi) {
    if (g_app.config.settings.uiScale > 0) return g_app.config.settings.uiScale;
    int scale = (int)(dpi + 48) / 96;
    return scale < 1 ? 1 : scale;
}

SIZE windowSizeForDpi(UINT dpi) {
    typedef BOOL(WINAPI * AdjustForDpiFn)(RECT*, DWORD, BOOL, DWORD, UINT);
    static auto adjustForDpi =
        (AdjustForDpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    int scale = scaleForDpi(dpi);
    RECT r = {0, 0, App::kWidth * scale, App::kHeight * scale};
    if (adjustForDpi)
        adjustForDpi(&r, kWindowStyle, FALSE, 0, dpi);
    else
        AdjustWindowRectEx(&r, kWindowStyle, FALSE, 0);
    return {r.right - r.left, r.bottom - r.top};
}

void enablePerMonitorDpiAwareness() {
    typedef BOOL(WINAPI * SetContextFn)(HANDLE);
    auto setContext = (SetContextFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (setContext && setContext((HANDLE)-4)) return;  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    SetProcessDPIAware();
}

// Tray ---------------------------------------------------------------------------------------

void updateTray(bool add) {
    NOTIFYICONDATAW data = {sizeof data};
    data.hWnd = g_host;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = WM_TRAY_ICON;
    data.hIcon = g_iconSmall;
    std::string tip = "CorsairCommanderCommander - " + g_app.config.activeProfile;
    for (const Reading& r : g_app.engine.readings) {
        if (!r.isTemp || !r.present || tip.size() > 100) continue;
        if (r.id.rfind("cp.t", 0) != 0 && r.id != "cpu.tctl" && r.id != "gpu.temp") continue;
        char line[64];
        snprintf(line, sizeof line, "\n%s %.1f C", g_app.engine.nameOf(r.id).c_str(), r.value);
        tip += line;
    }
    static std::string shownTip;
    if (!add && tip == shownTip) return;  // don't wake Explorer every second for nothing
    shownTip = tip;
    wcsncpy_s(data.szTip, toWide(tip).c_str(), _TRUNCATE);
    if (add)
        g_trayAdded = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    else if (g_trayAdded)
        Shell_NotifyIconW(NIM_MODIFY, &data);
}

void removeTray() {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW data = {sizeof data};
    data.hWnd = g_host;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
    g_trayAdded = false;
}

void showTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, COMMAND_OPEN_WINDOW, L"Open window...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < g_app.config.profiles.size(); i++) {
        const std::string& name = g_app.config.profiles[i].name;
        AppendMenuW(menu, MF_STRING | (name == g_app.config.activeProfile ? MF_CHECKED : 0),
                    COMMAND_PROFILE_BASE + (UINT)i, toWide(name).c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, COMMAND_QUIT, L"Quit (fans to failsafe, PSU to auto)");
    SetMenuDefaultItem(menu, COMMAND_OPEN_WINDOW, FALSE);
    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(g_host);
    int command =
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, g_host, nullptr);
    PostMessageW(g_host, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (command) PostMessageW(g_host, WM_COMMAND, command, 0);
}

// Polling ------------------------------------------------------------------------------------

void poll() {
    g_app.engine.detailed = g_window != nullptr;
    g_app.engine.tick(now());
    handleAlerts();
    updateTray(false);
    if (g_window) InvalidateRect(g_window, nullptr, FALSE);
    if (g_pollSeconds != g_app.config.settings.pollSeconds) {
        g_pollSeconds = g_app.config.settings.pollSeconds;
        SetTimer(g_host, TIMER_POLL, (UINT)g_pollSeconds * 1000, nullptr);
    }
}

// Window -------------------------------------------------------------------------------------

void openWindow() {
    if (g_window) {
        ShowWindow(g_window, SW_RESTORE);
        SetForegroundWindow(g_window);
        return;
    }
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO monitor = {sizeof monitor};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT& area = monitor.rcWork;
    g_window = CreateWindowExW(0, kWindowClass, kAppName, kWindowStyle, area.left, area.top, 100, 100, nullptr, nullptr,
                               g_instance, nullptr);
    if (!g_window) return;
    g_dpi = dpiForWindow(g_window);
    g_scale = scaleForDpi(g_dpi);
    SIZE size = windowSizeForDpi(g_dpi);
    int x = area.left + ((area.right - area.left) - size.cx) / 2;
    int y = area.top + ((area.bottom - area.top) - size.cy) / 2;
    SetWindowPos(g_window, nullptr, x, y > area.top ? y : area.top, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    g_input = UiInput();
    ShowWindow(g_window, SW_SHOWNORMAL);
    SetForegroundWindow(g_window);
    poll();  // read the rails right away
}

void quit() {
    if (g_window) DestroyWindow(g_window);
    removeTray();
    g_app.engine.stop();
    if (g_app.configDirty) g_app.save();
    DestroyWindow(g_host);
}

LRESULT CALLBACK hostProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_TIMER:
        if (wParam == TIMER_POLL) poll();
        return 0;
    case WM_TRAY_ICON:
        if (LOWORD(lParam) == WM_LBUTTONUP)
            openWindow();
        else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
            showTrayMenu();
        return 0;
    case WM_SHOW_SETTINGS: openWindow(); return 0;
    case WM_ALERT_CLOSED:
        if (g_openAlertBoxes > 0) g_openAlertBoxes--;
        return 0;
    case WM_COMMAND: {
        UINT command = LOWORD(wParam);
        if (command == COMMAND_OPEN_WINDOW) openWindow();
        if (command == COMMAND_QUIT) {
            quit();
            return 0;
        }
        if (command >= COMMAND_PROFILE_BASE && command < COMMAND_PROFILE_BASE + g_app.config.profiles.size()) {
            g_app.setActiveProfile(g_app.config.profiles[command - COMMAND_PROFILE_BASE].name);
            g_app.save();
            poll();
        }
        return 0;
    }
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMSUSPEND) g_app.engine.suspend();
        if (wParam == PBT_APMRESUMEAUTOMATIC) g_app.engine.resume(now());
        return TRUE;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        if (wParam) {
            g_app.engine.stop();
            if (g_app.configDirty) g_app.save();
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default:
        if (message == g_taskbarCreatedMessage && g_taskbarCreatedMessage) {
            updateTray(true);
            return 0;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void repaintNow(HWND window) {
    InvalidateRect(window, nullptr, FALSE);
    UpdateWindow(window);
}

void setMousePosition(LPARAM lParam) {
    g_input.mouseX = GET_X_LPARAM(lParam) / g_scale;
    g_input.mouseY = GET_Y_LPARAM(lParam) / g_scale;
}

void paintWindow(HWND window) {
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(window, &paint);
    g_input.shiftDown = GetKeyState(VK_SHIFT) < 0;
    bool again = g_app.drawFrame(g_input, g_scale);
    g_input.clearEdges();
    const Canvas& canvas = g_app.canvas();
    if (canvas.pixels) {
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = canvas.width;
        info.bmiHeader.biHeight = -canvas.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(dc, 0, 0, canvas.width, canvas.height, 0, 0, 0, canvas.height, canvas.pixels, &info,
                          DIB_RGB_COLORS);
    }
    RECT client;
    GetClientRect(window, &client);
    HBRUSH brush = CreateSolidBrush(kBackgroundColor);
    RECT right = {canvas.width, 0, client.right, client.bottom},
         bottom = {0, canvas.height, canvas.width, client.bottom};
    if (right.left < right.right) FillRect(dc, &right, brush);
    if (bottom.top < bottom.bottom) FillRect(dc, &bottom, brush);
    DeleteObject(brush);
    EndPaint(window, &paint);
    if (again) InvalidateRect(window, nullptr, FALSE);
    if (g_scale != scaleForDpi(g_dpi)) {
        g_scale = scaleForDpi(g_dpi);
        SIZE size = windowSizeForDpi(g_dpi);
        SetWindowPos(window, nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: paintWindow(window); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE:
        setMousePosition(lParam);
        if (!g_trackingMouseLeave) {
            TRACKMOUSEEVENT track = {sizeof track, TME_LEAVE, window, 0};
            g_trackingMouseLeave = TrackMouseEvent(&track) != FALSE;
        }
        repaintNow(window);
        return 0;
    case WM_MOUSELEAVE:
        g_trackingMouseLeave = false;
        if (!g_input.leftDown) g_input.mouseX = g_input.mouseY = -1;
        repaintNow(window);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        setMousePosition(lParam);
        SetCapture(window);
        g_input.leftDown = g_input.leftPressed = true;
        g_input.leftDouble = message == WM_LBUTTONDBLCLK;
        repaintNow(window);
        return 0;
    case WM_LBUTTONUP:
        setMousePosition(lParam);
        ReleaseCapture();
        g_input.leftDown = false;
        g_input.leftReleased = true;
        repaintNow(window);
        return 0;
    case WM_RBUTTONDOWN:
        setMousePosition(lParam);
        g_input.rightPressed = true;
        repaintNow(window);
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &p);
        g_input.mouseX = p.x / g_scale;
        g_input.mouseY = p.y / g_scale;
        g_input.wheel += GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        repaintNow(window);
        return 0;
    }
    case WM_KEYDOWN:
        g_input.key = (int)wParam;
        repaintNow(window);
        return 0;
    case WM_CHAR:
        if (wParam >= 0x20 && wParam < 0x7F) {
            g_input.text += (char)wParam;
            repaintNow(window);
        }
        return 0;
    case WM_GETDPISCALEDSIZE: {
        SIZE* proposed = (SIZE*)lParam;
        *proposed = windowSizeForDpi(LOWORD(wParam));
        return TRUE;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wParam);
        g_scale = scaleForDpi(g_dpi);
        const RECT* suggested = (const RECT*)lParam;
        SIZE size = windowSizeForDpi(g_dpi);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        g_window = nullptr;
        g_app.releaseCanvas();
        if (g_app.configDirty) g_app.save();
        g_app.engine.detailed = false;
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    bool showWindow = wcsstr(GetCommandLineW(), L"--show") != nullptr;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 4 && !wcscmp(argv[1], L"--screenshot")) return writeScreenshot(g_app, argv[2], argv[3], argv + 4, argc - 4);
    if (argc >= 2 && !wcscmp(argv[1], L"--quit")) {  // clean exit of the running instance (fans to failsafe)
        HWND other = FindWindowW(kHostClass, nullptr);
        if (!other) return 0;
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        PostMessageW(other, WM_COMMAND, COMMAND_QUIT, 0);
        DWORD wait = process ? WaitForSingleObject(process, 10000) : WAIT_FAILED;
        if (process) CloseHandle(process);
        return wait == WAIT_OBJECT_0 ? 0 : 1;
    }
    LocalFree(argv);
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\CorsairCommanderCommander_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kHostClass, nullptr)) PostMessageW(other, WM_SHOW_SETTINGS, 0, 0);
        return 0;
    }
    g_instance = instance;
    enablePerMonitorDpiAwareness();
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_secondsPerTick = 1.0 / (double)frequency.QuadPart;

    g_iconSmall = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), 0);
    g_iconLarge = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                    GetSystemMetrics(SM_CYICON), 0);
    if (!g_iconSmall) g_iconSmall = LoadIconW(nullptr, IDI_APPLICATION);
    if (!g_iconLarge) g_iconLarge = g_iconSmall;

    WNDCLASSEXW windowClass = {sizeof windowClass};
    windowClass.lpfnWndProc = hostProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kHostClass;
    RegisterClassExW(&windowClass);
    windowClass.lpfnWndProc = windowProc;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.style = CS_DBLCLKS;
    windowClass.hIcon = g_iconLarge;
    windowClass.hIconSm = g_iconSmall;
    RegisterClassExW(&windowClass);
    g_host = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance,
                             nullptr);
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    // We usually run elevated; let a normal-rights second launch and Explorer reach the host window.
    ChangeWindowMessageFilterEx(g_host, WM_SHOW_SETTINGS, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_host, g_taskbarCreatedMessage, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_host, WM_TRAY_ICON, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_host, WM_COMMAND, MSGFLT_ALLOW, nullptr);  // --quit from a non-elevated shell

    std::wstring directory = appDataPath(L"\\CorsairCommanderCommander");
    CreateDirectoryW(directory.c_str(), nullptr);
    AppHooks hooks;
    hooks.message = showMessage;
    hooks.setStartWithWindows = setStartWithWindows;
    hooks.icueFolder = toUtf8(appDataPath(L"\\Corsair\\CUE5"));
    g_app.init(toUtf8(directory + L"\\config.ini"), hooks);

    updateTray(true);
    poll();
    if (showWindow || g_app.config.settings.openWindowAtStart || !g_trayAdded) openWindow();
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    ReleaseMutex(instanceMutex);
    CloseHandle(instanceMutex);
    return 0;
}
