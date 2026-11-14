#include "win32/hid.h"
extern "C" {
#include <hidsdi.h>
}
#include <setupapi.h>
#include <cstring>
#include <vector>

bool HidDevice::open(uint16_t vid, uint16_t pid, size_t minOutput) {
    close();
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return false;
    SP_DEVICE_INTERFACE_DATA iface = {sizeof iface};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &iface); i++) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
        std::vector<BYTE> buffer(size);
        auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buffer.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attributes = {sizeof attributes};
        bool match = HidD_GetAttributes(h, &attributes) && attributes.VendorID == vid && attributes.ProductID == pid;
        HIDP_CAPS caps = {};
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (match && HidD_GetPreparsedData(h, &preparsed)) {
            HidP_GetCaps(preparsed, &caps);
            HidD_FreePreparsedData(preparsed);
        }
        if (match && caps.OutputReportByteLength >= minOutput + 1 && caps.InputReportByteLength > 1) {
            handle_ = h;
            inputLength_ = caps.InputReportByteLength;
            outputLength_ = caps.OutputReportByteLength;
            break;
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!isOpen()) return false;
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return true;
}

void HidDevice::close() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    if (event_) CloseHandle(event_);
    handle_ = INVALID_HANDLE_VALUE;
    event_ = nullptr;
}

bool HidDevice::transfer(bool isWrite, uint8_t* buffer, DWORD length, DWORD timeoutMs, DWORD* done) {
    OVERLAPPED overlapped = {};
    overlapped.hEvent = event_;
    ResetEvent(event_);
    BOOL ok = isWrite ? WriteFile(handle_, buffer, length, nullptr, &overlapped)
                      : ReadFile(handle_, buffer, length, nullptr, &overlapped);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        if (GetLastError() == ERROR_DEVICE_NOT_CONNECTED || GetLastError() == ERROR_BAD_COMMAND) close();
        return false;
    }
    if (WaitForSingleObject(event_, timeoutMs) != WAIT_OBJECT_0) {
        CancelIo(handle_);
        GetOverlappedResult(handle_, &overlapped, done, TRUE);
        return false;
    }
    return GetOverlappedResult(handle_, &overlapped, done, FALSE) != FALSE;
}

bool HidDevice::write(const uint8_t* data, size_t length, DWORD timeoutMs) {
    if (!isOpen()) return false;
    std::vector<uint8_t> report(outputLength_, 0);
    memcpy(report.data() + 1, data, length < outputLength_ - 1 ? length : outputLength_ - 1);
    DWORD done = 0;
    return transfer(true, report.data(), (DWORD)report.size(), timeoutMs, &done);
}

int HidDevice::read(uint8_t* data, size_t capacity, DWORD timeoutMs) {
    if (!isOpen()) return -1;
    std::vector<uint8_t> report(inputLength_, 0);
    DWORD done = 0;
    if (!transfer(false, report.data(), (DWORD)report.size(), timeoutMs, &done) || done < 1) return -1;
    size_t n = done - 1;
    if (n > capacity) n = capacity;
    memcpy(data, report.data() + 1, n);
    return (int)n;
}

void HidDevice::flushInput() {
    if (isOpen()) HidD_FlushQueue(handle_);
}
