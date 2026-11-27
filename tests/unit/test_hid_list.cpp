// Windows HID listing without device I/O (hid_list.cpp): what it reads from device paths and
// USB instance IDs. Pure string work, so it runs on every platform.
#include "hid_list.hpp"

#include <gtest/gtest.h>

using lh::usb::IsUsbDeviceInstanceId;
using lh::usb::ParseHidPath;
using lh::usb::SerialFromInstanceId;

TEST(HidList, ParsesVendorProductAndInterfaceFromWindowsPaths) {
  uint16_t vid = 0, pid = 0;
  int iface = 9;
  ASSERT_TRUE(ParseHidPath(R"(\\?\hid#vid_28de&pid_2101&mi_00#8&2c6e7e1c&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030})", &vid, &pid, &iface));
  EXPECT_EQ(vid, 0x28de);
  EXPECT_EQ(pid, 0x2101);
  EXPECT_EQ(iface, 0);
  // Single-interface Watchman dongle: no "&mi_" in the HID path; ListHid promotes iface -1 -> 0
  // when its parent DevInst is a whole USB device node.
  ASSERT_TRUE(ParseHidPath(R"(\\?\hid#vid_28de&pid_2101#8&2c6e7e1c&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030})", &vid, &pid, &iface));
  EXPECT_EQ(vid, 0x28de);
  EXPECT_EQ(pid, 0x2101);
  EXPECT_EQ(iface, -1);
  EXPECT_TRUE(IsUsbDeviceInstanceId(L"USB\\VID_28DE&PID_2101\\7B001B0FB0"));
  EXPECT_FALSE(IsUsbDeviceInstanceId(L"USB\\VID_28DE&PID_2300&MI_00\\7&3A4B5C6D&0&0000"));
  EXPECT_FALSE(IsUsbDeviceInstanceId(L"BTHLEDevice\\{00001812-0000-1000-8000-00805f9b34fb}\\1"));
  // Upper case, a later interface, and a collection suffix.
  ASSERT_TRUE(ParseHidPath(R"(\\?\HID#VID_28DE&PID_2300&MI_02&Col01#9&1a&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030})", &vid, &pid, &iface));
  EXPECT_EQ(pid, 0x2300);
  EXPECT_EQ(iface, 2);
  // Not a composite device: no interface number.
  ASSERT_TRUE(ParseHidPath(R"(\\?\hid#vid_0bb4&pid_0306#7&1&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030})", &vid, &pid, &iface));
  EXPECT_EQ(vid, 0x0bb4);
  EXPECT_EQ(iface, -1);
  // Bluetooth HID paths and junk.
  EXPECT_FALSE(ParseHidPath(R"(\\?\hid#{00001124-0000-1000-8000-00805f9b34fb}_dev_vid&02045e_pid&0b13)", &vid, &pid, &iface));
  EXPECT_FALSE(ParseHidPath("", &vid, &pid, &iface));
  EXPECT_FALSE(ParseHidPath("vid_zz12&pid_2101", &vid, &pid, &iface));
}

TEST(HidList, TakesTheSerialFromTheUsbDeviceInstanceId) {
  EXPECT_EQ(SerialFromInstanceId(L"USB\\VID_28DE&PID_2101\\7B001B0FB0"), L"7B001B0FB0");
  EXPECT_EQ(SerialFromInstanceId(L"USB\\VID_28DE&PID_2000\\LHR-B132FC21"), L"LHR-B132FC21");
  // A USB interface, not the device.
  EXPECT_EQ(SerialFromInstanceId(L"USB\\VID_28DE&PID_2101&MI_00\\7&3A4B5C6D&0&0000"), L"");
  // Made up by Windows: the device has no serial number.
  EXPECT_EQ(SerialFromInstanceId(L"USB\\VID_28DE&PID_2101\\6&2F1A3B4C&0&2"), L"");
  // Not USB at all.
  EXPECT_EQ(SerialFromInstanceId(L"HID\\VID_28DE&PID_2101&MI_00\\8&2C6E7E1C&0&0000"), L"");
  EXPECT_EQ(SerialFromInstanceId(L"USB\\VID_28DE&PID_2101\\"), L"");
}
