// CPU and GPU temperatures read once per poll, nothing in the background.
// CPU: AMD Zen SMN registers through PawnIO (signed driver, https://pawnio.eu) and its AMDFamily17 module,
// which is embedded in the exe.
// GPU: NVIDIA NVML from the display driver.
#pragma once
#include <windows.h>
#include <string>

class CpuSensor {
public:
    ~CpuSensor() { close(); }
    bool open(std::string& error);  // error explains what is missing
    void close();
    bool isOpen() const { return handle_ != nullptr; }
    // Tctl/Tdie and per-CCD temperatures (NAN when a CCD is absent).
    bool read(float& tctl, float ccd[2]);

private:
    HMODULE lib_ = nullptr;
    HANDLE handle_ = nullptr, pciMutex_ = nullptr;
    void* execute_ = nullptr;
    void* close_ = nullptr;
    bool readSmn(unsigned address, unsigned& value);
};

class GpuSensor {
public:
    ~GpuSensor() { close(); }
    bool open(std::string& error);
    void close();
    bool isOpen() const { return device_ != nullptr; }
    bool read(float& celsius, float& watts);  // watts NAN if unsupported

private:
    HMODULE lib_ = nullptr;
    void* device_ = nullptr;
    void* getTemperature_ = nullptr;
    void* getPower_ = nullptr;
    void* shutdown_ = nullptr;
};
