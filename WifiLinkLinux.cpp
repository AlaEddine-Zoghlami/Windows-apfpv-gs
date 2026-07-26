// WifiLink — Linux implementation.
//
// Reads /proc/net/wireless, which is the same data iwconfig prints. Chosen over
// nl80211 deliberately: it needs no libnl dependency, no root, and no netlink
// socket, and it is present on every kernel with CONFIG_WIRELESS_EXT-compatible
// reporting (which includes cfg80211 drivers -- the file is emulated for them).
//
// Format (two header rows, then one row per interface):
//   Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE
//    face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon | 22
//    wlan0: 0000   70.  -40.  -256        0      0      0      0     0        0
//
// "level" is the RSSI in dBm. The trailing '.' is part of the field, so the
// scanf format consumes it explicitly rather than relying on %lf stopping there.
//
// LIMITATION, stated rather than papered over: this file exposes no PHY rate, so
// rxRateKbps stays 0 and hasRxRate() is false -- the UI then shows "not
// supported" rather than a fabricated number. Getting the rate would mean an
// nl80211 NL80211_CMD_GET_STATION query and a libnl dependency, which is not
// worth it for a cosmetic field.

#include "WifiLink.h"

#if defined(__linux__)

#include <unistd.h>      // readlink

#include <cstdio>
#include <cstring>
#include <vector>

namespace apfpv {
namespace {

/// True if `iface` is a USB device, by checking whether its sysfs device path
/// passes through a USB bus. On this project that identifies the FPV dongle as
/// opposed to a laptop's built-in card.
bool isUsbIface(const std::string& iface) {
    // /sys/class/net/<if> is a symlink into the device tree; USB devices have
    // "/usb" in the resolved path. readlink is enough -- no need to realpath.
    std::string link = "/sys/class/net/" + iface;
    char buf[512] = {0};
    ssize_t n = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0) return false;
    return std::strstr(buf, "/usb") != nullptr;
}

class WifiLinkLinux final : public WifiLink {
public:
    explicit WifiLinkLinux(bool preferUsb) : _preferUsb(preferUsb) {}

    bool open() override {
        // Nothing to acquire -- just confirm the file exists and that at least
        // one wireless interface is listed, so a machine with no radio fails
        // here rather than polling a missing file every second.
        std::vector<std::string> ifaces;
        if (!listIfaces(ifaces) || ifaces.empty()) return false;
        _iface = ifaces.front();
        if (_preferUsb) {
            for (const auto& i : ifaces)
                if (isUsbIface(i)) { _iface = i; break; }
        }
        return true;
    }

    bool poll(WifiLinkSample& out) override {
        FILE* f = std::fopen("/proc/net/wireless", "r");
        if (!f) return false;
        char line[256];
        int lineNo = 0;
        bool got = false;
        while (std::fgets(line, sizeof(line), f)) {
            if (++lineNo <= 2) continue;                       // two header rows
            char iface[64] = {0};
            double qual = 0, level = 0;
            if (std::sscanf(line, " %63[^:]: %*x %lf. %lf.", iface, &qual, &level) != 3) continue;
            if (_iface != iface) continue;                      // only our chosen radio
            int dbm = (int) level;
            // Sanity-bound it: an unassociated radio can report 0 or nonsense,
            // and passing that on would look like a real (very strong) reading.
            if (dbm < 0 && dbm > -110) { out.rssiDbm = dbm; got = true; }
            break;
        }
        std::fclose(f);
        return got;
    }

    void close() override {}
    std::string ifaceName() const override { return _iface; }
    bool hasRxRate() const override { return false; }

private:
    static bool listIfaces(std::vector<std::string>& out) {
        FILE* f = std::fopen("/proc/net/wireless", "r");
        if (!f) return false;
        char line[256];
        int lineNo = 0;
        while (std::fgets(line, sizeof(line), f)) {
            if (++lineNo <= 2) continue;
            char iface[64] = {0};
            if (std::sscanf(line, " %63[^:]:", iface) == 1) out.emplace_back(iface);
        }
        std::fclose(f);
        return true;
    }

    std::string _iface;
    bool        _preferUsb = true;
};

} // namespace

std::unique_ptr<WifiLink> makeWifiLink(bool preferUsb) {
    return std::make_unique<WifiLinkLinux>(preferUsb);
}

} // namespace apfpv

#endif // __linux__
