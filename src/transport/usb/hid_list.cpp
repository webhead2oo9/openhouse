#include "hid_list.hpp"

#include <hidapi/hidapi.h>

#include <cctype>
#include <cstdlib>
#include <cwctype>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
}
#endif

namespace lh::usb {

namespace {
// The 4 hex digits after `key` ("vid_"), case-insensitive.
bool HexAfter(const std::string& lower, const char* key, uint16_t* out) {
  const size_t at = lower.find(key);
  if (at == std::string::npos) return false;
  const std::string digits = lower.substr(at + std::char_traits<char>::length(key), 4);
  if (digits.size() != 4) return false;
  for (char c : digits)
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  *out = static_cast<uint16_t>(std::strtoul(digits.c_str(), nullptr, 16));
  return true;
}
}  // namespace

bool ParseHidPath(const std::string& path, uint16_t* vid, uint16_t* pid, int* iface) {
  std::string lower = path;
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (!HexAfter(lower, "vid_", vid) || !HexAfter(lower, "pid_", pid)) return false;
  *iface = -1;
  const size_t mi = lower.find("&mi_");
  if (mi != std::string::npos && mi + 6 <= lower.size() && std::isxdigit(static_cast<unsigned char>(lower[mi + 4])) &&
      std::isxdigit(static_cast<unsigned char>(lower[mi + 5])))
    *iface = static_cast<int>(std::strtoul(lower.substr(mi + 4, 2).c_str(), nullptr, 16));
  return true;
}

bool IsUsbDeviceInstanceId(const std::wstring& id) {
  std::wstring upper = id;
  for (wchar_t& c : upper) c = static_cast<wchar_t>(std::towupper(c));
  return upper.rfind(L"USB\\", 0) == 0 && upper.find(L"&MI_") == std::wstring::npos;
}

std::wstring SerialFromInstanceId(const std::wstring& id) {
  if (!IsUsbDeviceInstanceId(id)) return {};
  const size_t slash = id.rfind(L'\\');
  if (slash == std::wstring::npos || slash + 1 >= id.size()) return {};
  std::wstring serial = id.substr(slash + 1);
  if (serial.find(L'&') != std::wstring::npos) return {};   // made up by Windows: no serial
  return serial;
}

#ifdef _WIN32
namespace {
std::wstring InstanceId(DEVINST inst) {
  wchar_t id[MAX_DEVICE_ID_LEN + 1] = {};
  if (CM_Get_Device_IDW(inst, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return {};
  return id;
}

// The USB serial: the instance ID of the first ancestor that's a whole USB device (the HID
// interface's parent is the USB interface on a composite device, and its parent the device).
// Also sets *usbDevice when a whole USB device ancestor was found: single-interface USB HID
// devices (such as external Watchman dongles, 28de:2101) have no "&mi_" in their HID path and
// sit on interface 0, as hidapi reports them.
std::wstring UsbSerial(DEVINST inst, bool* usbDevice) {
  if (usbDevice) *usbDevice = false;
  for (int depth = 0; depth < 4; ++depth) {
    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, inst, 0) != CR_SUCCESS) return {};
    inst = parent;
    const std::wstring id = InstanceId(inst);
    if (IsUsbDeviceInstanceId(id)) {
      if (usbDevice) *usbDevice = true;
      return SerialFromInstanceId(id);
    }
  }
  return {};
}

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}
}  // namespace

std::vector<HidEntry> ListHid(uint16_t vid) {
  std::vector<HidEntry> out;
  GUID hidGuid;
  HidD_GetHidGuid(&hidGuid);
  HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) return out;
  SP_DEVICE_INTERFACE_DATA iface{};
  iface.cbSize = sizeof iface;
  for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &iface); ++i) {
    DWORD size = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
    if (size == 0) continue;
    std::vector<unsigned char> buffer(size);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof dev;
    if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, nullptr, &dev)) continue;
    HidEntry e;
    e.path = Narrow(detail->DevicePath);
    if (!ParseHidPath(e.path, &e.vid, &e.pid, &e.iface)) continue;
    if (vid != 0 && e.vid != vid) continue;
    bool usbDevice = false;
    e.serial = UsbSerial(dev.DevInst, &usbDevice);
    if (e.iface < 0 && usbDevice) e.iface = 0;
    out.push_back(std::move(e));
  }
  SetupDiDestroyDeviceInfoList(set);
  return out;
}
#else
std::vector<HidEntry> ListHid(uint16_t vid) {
  std::vector<HidEntry> out;
  hid_device_info* list = hid_enumerate(vid, 0);
  for (hid_device_info* d = list; d; d = d->next) {
    if (!d->path) continue;
    HidEntry e;
    e.path = d->path;
    e.vid = d->vendor_id;
    e.pid = d->product_id;
    e.iface = d->interface_number;
    if (d->serial_number) e.serial = d->serial_number;
    out.push_back(std::move(e));
  }
  hid_free_enumeration(list);
  return out;
}
#endif

}  // namespace lh::usb
