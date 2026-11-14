// Minimal HID access: open by VID/PID, overlapped report I/O with timeouts.
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>

class HidDevice {
public:
    ~HidDevice() { close(); }
    // Opens the first interface of vid:pid whose output report holds at least minOutput bytes (excluding the
    // report id).
    bool open(uint16_t vid, uint16_t pid, size_t minOutput);
    void close();
    bool isOpen() const { return handle_ != INVALID_HANDLE_VALUE; }
    // `data` excludes the report id; it is zero-padded to the device's report size behind report id 0.
    bool write(const uint8_t* data, size_t length, DWORD timeoutMs = 500);
    // Reads one input report into `data` (without the report id). Returns bytes stored, or -1.
    int read(uint8_t* data, size_t capacity, DWORD timeoutMs = 500);
    void flushInput();

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
    size_t inputLength_ = 0, outputLength_ = 0;  // including the report id byte
    bool transfer(bool isWrite, uint8_t* buffer, DWORD length, DWORD timeoutMs, DWORD* done);
};
