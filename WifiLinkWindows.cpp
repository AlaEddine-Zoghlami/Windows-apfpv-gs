// WifiLink — Windows implementation (wlanapi).
//
// Lifted from the LinkStats #ifdef block that used to live in apfpv_player.cpp,
// so the behaviour is unchanged: prefer a USB adapter (the FPV dongle) and fall
// back to the first interface, then read RSSI and the association's RX rate.
//
// Note wlanapi reports a PHY RATE, not an MCS index. Back-deriving an index
// would need the bandwidth + guard-interval combination and would be a guess,
// so the real rate is surfaced and the UI shows that instead.

#include "WifiLink.h"

#if defined(_WIN32)

#include <winsock2.h>      // must precede windows.h
#include <windows.h>
#include <wlanapi.h>
#include <cstring>

namespace apfpv {
namespace {

class WifiLinkWindows final : public WifiLink {
public:
    explicit WifiLinkWindows(bool preferUsb) : _preferUsb(preferUsb) {}
    ~WifiLinkWindows() override { close(); }

    bool open() override {
        DWORD neg = 0;
        if (WlanOpenHandle(2, nullptr, &neg, &_h) != ERROR_SUCCESS) { _h = nullptr; return false; }
        WLAN_INTERFACE_INFO_LIST* list = nullptr;
        if (WlanEnumInterfaces(_h, nullptr, &list) == ERROR_SUCCESS && list) {
            if (_preferUsb) {
                for (DWORD i = 0; i < list->dwNumberOfItems; i++) {
                    char desc[256] = {0};
                    WideCharToMultiByte(CP_UTF8, 0, list->InterfaceInfo[i].strInterfaceDescription,
                                        -1, desc, sizeof(desc), nullptr, nullptr);
                    if (std::strstr(desc, "USB")) {
                        _guid = list->InterfaceInfo[i].InterfaceGuid;
                        _name = desc;
                        _have = true;
                        break;
                    }
                }
            }
            if (!_have && list->dwNumberOfItems > 0) {
                char desc[256] = {0};
                WideCharToMultiByte(CP_UTF8, 0, list->InterfaceInfo[0].strInterfaceDescription,
                                    -1, desc, sizeof(desc), nullptr, nullptr);
                _guid = list->InterfaceInfo[0].InterfaceGuid;
                _name = desc;
                _have = true;
            }
            WlanFreeMemory(list);
        }
        if (!_have) { close(); return false; }
        return true;
    }

    bool poll(WifiLinkSample& out) override {
        if (!_h || !_have) return false;
        bool any = false;
        DWORD sz = 0; PVOID data = nullptr;
        if (WlanQueryInterface(_h, &_guid, wlan_intf_opcode_rssi, nullptr, &sz, &data, nullptr)
                == ERROR_SUCCESS && data) {
            out.rssiDbm = (int) *(LONG*) data;
            WlanFreeMemory(data);
            any = true;
        }
        sz = 0; data = nullptr;
        if (WlanQueryInterface(_h, &_guid, wlan_intf_opcode_current_connection, nullptr, &sz,
                               &data, nullptr) == ERROR_SUCCESS && data) {
            auto* ca = (WLAN_CONNECTION_ATTRIBUTES*) data;
            out.rxRateKbps = ca->wlanAssociationAttributes.ulRxRate;
            WlanFreeMemory(data);
            any = true;
        }
        return any;
    }

    void close() override {
        if (_h) { WlanCloseHandle(_h, nullptr); _h = nullptr; }
        _have = false;
    }

    std::string ifaceName() const override { return _name; }
    bool hasRxRate() const override { return true; }

private:
    HANDLE      _h = nullptr;
    GUID        _guid{};
    bool        _have = false;
    bool        _preferUsb = true;
    std::string _name;
};

} // namespace

std::unique_ptr<WifiLink> makeWifiLink(bool preferUsb) {
    return std::make_unique<WifiLinkWindows>(preferUsb);
}

} // namespace apfpv

#endif // _WIN32
