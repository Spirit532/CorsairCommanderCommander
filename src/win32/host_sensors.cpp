#include "win32/host_sensors.h"
#include <intrin.h>
#include <cmath>
#include <vector>

// CPU ----------------------------------------------------------------------------------------

namespace {
typedef HRESULT(WINAPI* PawnOpenFn)(PHANDLE);
typedef HRESULT(WINAPI* PawnLoadFn)(HANDLE, const UCHAR*, SIZE_T);
typedef HRESULT(WINAPI* PawnExecuteFn)(HANDLE, PCSTR, const ULONG64*, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
typedef HRESULT(WINAPI* PawnCloseFn)(HANDLE);

const unsigned kThmTconCurTmp = 0x00059800;  // Zen: Tctl

// CCD temperature registers, 4 bytes apart: Zen 2 (family 17h) and Zen 3+ (family 19h+), per LibreHardwareMonitor.
unsigned ccdTempAddress() {
    static unsigned cached = 0;
    if (cached) return cached;
    int r[4];
    __cpuid(r, 1);
    unsigned family = ((r[0] >> 8) & 0xF) + ((r[0] >> 20) & 0xFF);
    return cached = family >= 0x19 ? 0x00059B08 : 0x00059954;
}

// The signed AMDFamily17 module from PawnIO.Modules, embedded as RCDATA 2.
bool loadModule(std::vector<UCHAR>& data) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(2), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    const UCHAR* bytes = loaded ? (const UCHAR*)LockResource(loaded) : nullptr;
    if (!bytes) return false;
    data.assign(bytes, bytes + SizeofResource(nullptr, resource));
    return !data.empty();
}
}  // namespace

bool CpuSensor::open(std::string& error) {
    close();
    lib_ = LoadLibraryW(L"PawnIOLib.dll");
    if (!lib_) {
        wchar_t programFiles[MAX_PATH];
        if (GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH))
            lib_ = LoadLibraryW((std::wstring(programFiles) + L"\\PawnIO\\PawnIOLib.dll").c_str());
    }
    if (!lib_) {
        error = "PawnIO is not installed (pawnio.eu)";
        return false;
    }
    auto open = (PawnOpenFn)GetProcAddress(lib_, "pawnio_open");
    auto load = (PawnLoadFn)GetProcAddress(lib_, "pawnio_load");
    execute_ = (void*)GetProcAddress(lib_, "pawnio_execute");
    close_ = (void*)GetProcAddress(lib_, "pawnio_close");
    std::vector<UCHAR> blob;
    if (!open || !load || !execute_ || !close_) {
        error = "PawnIOLib.dll is incompatible";
    } else if (!loadModule(blob)) {
        error = "AMDFamily17 module missing from the exe";
    } else if (FAILED(open(&handle_)) || !handle_) {
        handle_ = nullptr;
        error = "PawnIO driver not accessible (run as administrator)";
    } else if (FAILED(load(handle_, blob.data(), blob.size()))) {
        error = "PawnIO refused the AMDFamily17 module";
    } else {
        pciMutex_ = CreateMutexW(nullptr, FALSE, L"Global\\Access_PCI");  // shared with other monitoring tools
        float t, ccd[2];
        if (read(t, ccd)) return true;
        error = "CPU temperature read failed";
    }
    close();
    return false;
}

void CpuSensor::close() {
    if (handle_ && close_) ((PawnCloseFn)close_)(handle_);
    if (pciMutex_) CloseHandle(pciMutex_);
    if (lib_) FreeLibrary(lib_);
    handle_ = pciMutex_ = nullptr;
    lib_ = nullptr;
}

bool CpuSensor::readSmn(unsigned address, unsigned& value) {
    ULONG64 in = address, out = 0;
    SIZE_T returned = 0;
    HRESULT hr = ((PawnExecuteFn)execute_)(handle_, "ioctl_read_smn", &in, 1, &out, 1, &returned);
    value = (unsigned)out;
    return SUCCEEDED(hr) && returned == 1;
}

bool CpuSensor::read(float& tctl, float ccd[2]) {
    tctl = ccd[0] = ccd[1] = NAN;
    if (!handle_) return false;
    if (pciMutex_ && WaitForSingleObject(pciMutex_, 100) == WAIT_TIMEOUT) return false;
    unsigned v = 0;
    bool ok = readSmn(kThmTconCurTmp, v);
    if (ok) {
        tctl = ((v >> 21) & 0x7FF) * 0.125f;
        if (v & (1u << 19)) tctl -= 49.0f;  // CUR_TEMP_RANGE_SEL
    }
    for (int i = 0; i < 2 && ok; i++) {
        unsigned c = 0;
        if (readSmn(ccdTempAddress() + i * 4, c) && (c & 0x800)) ccd[i] = (c & 0x7FF) * 0.125f - 49.0f;
    }
    if (pciMutex_) ReleaseMutex(pciMutex_);
    return ok && tctl > -20 && tctl < 130;
}

// GPU ----------------------------------------------------------------------------------------

namespace {
typedef int(__cdecl* NvmlInitFn)();
typedef int(__cdecl* NvmlHandleFn)(unsigned, void**);
typedef int(__cdecl* NvmlTempFn)(void*, int, unsigned*);
typedef int(__cdecl* NvmlPowerFn)(void*, unsigned*);
typedef int(__cdecl* NvmlShutdownFn)();
}  // namespace

bool GpuSensor::open(std::string& error) {
    close();
    lib_ = LoadLibraryW(L"nvml.dll");
    if (!lib_) {
        wchar_t programFiles[MAX_PATH];
        if (GetEnvironmentVariableW(L"ProgramW6432", programFiles, MAX_PATH))
            lib_ = LoadLibraryW((std::wstring(programFiles) + L"\\NVIDIA Corporation\\NVSMI\\nvml.dll").c_str());
    }
    if (!lib_) {
        error = "NVIDIA NVML not found";
        return false;
    }
    auto init = (NvmlInitFn)GetProcAddress(lib_, "nvmlInit_v2");
    auto handle = (NvmlHandleFn)GetProcAddress(lib_, "nvmlDeviceGetHandleByIndex_v2");
    getTemperature_ = (void*)GetProcAddress(lib_, "nvmlDeviceGetTemperature");
    getPower_ = (void*)GetProcAddress(lib_, "nvmlDeviceGetPowerUsage");
    shutdown_ = (void*)GetProcAddress(lib_, "nvmlShutdown");
    if (!init || !handle || !getTemperature_ || init() != 0) {
        error = "NVML init failed";
        FreeLibrary(lib_);
        lib_ = nullptr;
        return false;
    }
    if (handle(0, &device_) != 0) {
        device_ = nullptr;
        error = "no NVIDIA GPU";
        close();
        return false;
    }
    return true;
}

void GpuSensor::close() {
    if (lib_ && shutdown_) ((NvmlShutdownFn)shutdown_)();
    if (lib_) FreeLibrary(lib_);
    lib_ = nullptr;
    device_ = nullptr;
}

bool GpuSensor::read(float& celsius, float& watts) {
    celsius = watts = NAN;
    if (!device_) return false;
    unsigned t = 0, mw = 0;
    if (((NvmlTempFn)getTemperature_)(device_, 0, &t) != 0) return false;  // NVML_TEMPERATURE_GPU
    celsius = (float)t;
    if (getPower_ && ((NvmlPowerFn)getPower_)(device_, &mw) == 0) watts = mw / 1000.0f;
    return true;
}
