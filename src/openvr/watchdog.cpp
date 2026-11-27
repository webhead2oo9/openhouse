// SteamVR's watchdog (IVRWatchdogProvider): loaded while SteamVR is not running, it starts
// SteamVR when a device is turned on, as stock's CWatchdogDriver_Lighthouse does ("new
// wireless connection"). Off when Steam's own setting is: "power" /
// "autoLaunchSteamVROnButtonPress" ("Start SteamVR when a headset or controller is turned
// on" in Steam's settings).
//
// What wakes it:
// - a receiver (dongle, 28de:2101/2102) that had no wireless traffic starts receiving input
//   reports (0x23/0x24): a controller or tracker was turned on. Detected passively via Interrupt
//   IN reads, as stock's CWatchdogDriver_Lighthouse does, without polling EP0 feature report ff 83;
// - a wired device (28de:2000/2012/2022/2300) is plugged in, unless SteamVR knows it as a
//   controller or tracker (WiredPlugInWakes): those show up on a charging cable while off.
//   A Vive tracker and the Bigscreen Beyond are both 28de:2300, so it goes by the serial.
// Devices already there (or already streaming within 0.5 s of opening) when the watchdog starts
// don't wake it: SteamVR was just closed with them, or Steam is starting.
#include "watchdog.hpp"

#include "../platform/platform.hpp"
#include "../transport/usb/hid_list.hpp"
#include "../transport/usb/usb_transport.hpp"

#include <hidapi/hidapi.h>

#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <thread>

using namespace vr;

namespace lh::openvr {
namespace {

constexpr uint16_t kValve = 0x28de;

bool IsReceiver(uint16_t pid) { return pid == 0x2101 || pid == 0x2102; }
bool IsWired(uint16_t pid) { return pid == 0x2000 || pid == 0x2012 || pid == 0x2022 || pid == 0x2300; }

std::string Narrow(const std::wstring& w) {
  std::string out;
  for (wchar_t c : w) out += c < 0x80 ? char(c) : '?';
  return out;
}

class Watchdog final : public IVRWatchdogProvider {
 public:
  EVRInitError Init(IVRDriverContext* context) override {
    VR_INIT_WATCHDOG_DRIVER_CONTEXT(context);
    // Steam's switch. Missing (an old Steam): on, as SteamVR's default.
    if (VRSettings()) {
      EVRSettingsError e = VRSettingsError_None;
      const bool on = VRSettings()->GetBool("power", "autoLaunchSteamVROnButtonPress", &e);
      if (e == VRSettingsError_None && !on) return VRInitError_Init_WatchdogDisabledInSettings;
    }
    if (hid_init() != 0) return VRInitError_Init_InitCanceledByUser;
    stop_ = false;
    thread_ = std::thread([this] { Main(); });
    return VRInitError_None;
  }

  void Cleanup() override {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    LetGo();
    CleanupDriverContext();
  }

 private:
  struct Receiver {
    hid_device* dev = nullptr;
    std::chrono::steady_clock::time_point openedAt{};
    std::chrono::steady_clock::time_point lastData{};
    bool connected = false;
  };

  // While SteamVR runs, its driver has the dongles: handles here are closed as soon as vrserver is
  // seen. Steam keeps the watchdog loaded through SteamVR sessions without calling Cleanup().
  void LetGo() {
    for (auto& [path, r] : receivers_)
      if (r.dev) hid_close(r.dev);
    receivers_.clear();
    wired_.clear();
  }

  void Main() {
    bool first = true;
    int ticksUntilScan = 0;
    int ticksUntilProcCheck = 0;
    bool vrserver = false;
    while (!stop_) {
      if (ticksUntilProcCheck <= 0) {
        vrserver = lh::platform::VrserverRunning();
        ticksUntilProcCheck = 5;  // check process list twice a second, not 20-30x/s
      } else {
        --ticksUntilProcCheck;
      }
      if (vrserver) {
        LetGo();
        first = true;   // what's there once SteamVR closes was there with it: not a wake-up
        ticksUntilScan = 0;
      } else {
        if (ticksUntilScan <= 0) {
          lh::platform::DongleQueryLock lock;
          if (lock.Acquire(std::chrono::milliseconds(0))) {
            Scan(first);
            first = false;
            ticksUntilScan = 10;  // re-list HID paths once a second
          }
        } else {
          --ticksUntilScan;
        }
        ReadReceivers();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  void Scan(bool first) {
    const auto now = std::chrono::steady_clock::now();
    std::set<std::string> seenReceivers, seenWired;
    for (const lh::usb::HidEntry& entry : lh::usb::ListHid(kValve)) {
      const lh::usb::HidEntry* d = &entry;
      const std::string& path = d->path;
      if (IsReceiver(d->pid) && d->iface == 0) {
        seenReceivers.insert(path);
        if (!receivers_.count(path)) {
          Receiver r;
          r.dev = hid_open_path(path.c_str());
          if (!r.dev) continue;
          hid_set_nonblocking(r.dev, 1);
          r.openedAt = now;
          receivers_[path] = r;
        }
      } else if (IsWired(d->pid) && d->iface == 0) {
        seenWired.insert(path);
        if (!first && !wired_.count(path)) {
          const std::string serial = Narrow(d->serial);
          const std::string config =
              serial.empty() ? std::string() : lh::usb::UsbTransport::LoadSteamConfig(lh::platform::SteamConfigDirectory(), serial);
          if (WiredPlugInWakes(config)) {
            Wake(d->pid == 0x2000 ? TrackedDeviceClass_HMD : TrackedDeviceClass_Controller);
          } else if (VRDriverLog()) {
            VRDriverLog()->Log(("lh: [INFO] watchdog: " + serial + " plugged in, a controller or tracker charging: not starting SteamVR\n").c_str());
          }
        }
      }
    }
    wired_ = seenWired;
    for (auto it = receivers_.begin(); it != receivers_.end();) {
      if (!seenReceivers.count(it->first)) {
        hid_close(it->second.dev);
        it = receivers_.erase(it);
        continue;
      }
      ++it;
    }
  }

  void ReadReceivers() {
    const auto now = std::chrono::steady_clock::now();
    unsigned char buf[64];
    for (auto it = receivers_.begin(); it != receivers_.end();) {
      Receiver& r = it->second;
      bool failed = false;
      for (int k = 0; k < 16; ++k) {
        const int n = hid_read(r.dev, buf, sizeof buf);
        if (n < 0) { failed = true; break; }
        if (n == 0) break;
        if (buf[0] == 0x26) {
          r.connected = false;
        } else {
          r.lastData = now;
          if (!r.connected) {
            r.connected = true;
            // A device already transmitting when the receiver was opened (Steam starting, SteamVR
            // just closed with powerOffOnExit=false, or dongle plugged in with device on) doesn't wake.
            if (now - r.openedAt >= std::chrono::milliseconds(500)) Wake(TrackedDeviceClass_Controller);
          }
        }
      }
      if (failed) {
        hid_close(r.dev);
        it = receivers_.erase(it);
        continue;
      }
      // Require a long silence (15 s) before assuming power-off without a 0x26 disconnect report,
      // so brief radio dropouts on a device left on after SteamVR exits don't relaunch SteamVR.
      if (r.connected && now - r.lastData > std::chrono::seconds(15)) r.connected = false;
      ++it;
    }
  }

  void Wake(ETrackedDeviceClass cls) {
    // One wake-up per 10 s: SteamVR is starting, and closes the watchdog when it has.
    const auto now = std::chrono::steady_clock::now();
    if (now - lastWake_ < std::chrono::seconds(10)) return;
    lastWake_ = now;
    if (VRWatchdogHost()) VRWatchdogHost()->WatchdogWakeUp(cls);
  }

  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::map<std::string, Receiver> receivers_;  // by HID path
  std::set<std::string> wired_;
  std::chrono::steady_clock::time_point lastWake_{};
};

Watchdog g_watchdog;

}  // namespace

IVRWatchdogProvider* WatchdogProvider() { return &g_watchdog; }

}  // namespace lh::openvr
