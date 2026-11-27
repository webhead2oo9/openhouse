// Lists HID interfaces without talking to the devices. On Windows hidapi's hid_enumerate reads
// every device's manufacturer, product and serial strings from the device itself, with no
// timeout: one wedged dongle blocked the whole list, and SteamVR then aborted the driver while it
// loaded. Here, Windows reads them from the device path and the USB instance ID instead. On
// Linux hid_enumerate only reads sysfs, so it stays.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lh::usb {

struct HidEntry {
  std::string path;     // as hid_open_path takes it
  uint16_t vid = 0, pid = 0;
  int iface = -1;       // USB interface number (0 for single-interface USB HID), -1 when non-USB without mi_
  std::wstring serial;  // the USB serial number, empty when unknown
};

// HID interfaces of one vendor (all with vid 0).
std::vector<HidEntry> ListHid(uint16_t vid);

// Windows device path pieces ("\\?\hid#vid_28de&pid_2101&mi_00#..."); false when there's no
// vid/pid. iface: -1 without "mi_".
bool ParseHidPath(const std::string& path, uint16_t* vid, uint16_t* pid, int* iface);
// The serial from a USB device's instance ID ("USB\VID_28DE&PID_2101\7B001B0FB0"), or empty:
// a per-interface ID ("...&MI_00\...") or one Windows made up ("6&2f1a3b4c&0&2", a device
// without a serial number).
std::wstring SerialFromInstanceId(const std::wstring& id);
// Whether a Windows device instance ID names a whole USB device ("USB\...", without "&MI_").
bool IsUsbDeviceInstanceId(const std::wstring& id);

}  // namespace lh::usb
