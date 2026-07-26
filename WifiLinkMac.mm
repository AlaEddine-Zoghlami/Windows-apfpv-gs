// WifiLink — macOS implementation (CoreWLAN).
//
// Objective-C++ (.mm) because CoreWLAN has no C API: CWWiFiClient/CWInterface are
// Objective-C classes. This is the reason this file exists separately rather than
// being another branch inside a .cpp -- the language itself differs.
//
// CoreWLAN gives us both values the interface asks for:
//   rssiValue    -> RSSI in dBm
//   transmitRate -> the negotiated rate in Mbit/s (a double), converted to kbps
//
// Two macOS realities worth knowing:
//  - On recent macOS, reading RSSI can require Location Services permission for
//    the calling app. When it is not granted, rssiValue returns 0 and there is no
//    error -- so poll() reports failure rather than passing 0 off as a reading.
//  - USB Wi-Fi adapters are rare on Apple silicon (few vendor drivers), so
//    preferUsb has no reliable signal to act on here; CoreWLAN does not expose a
//    bus type. We take the default interface, which is the built-in radio. That
//    is honest behaviour for the platform rather than a guess.

#include "WifiLink.h"

#if defined(__APPLE__)

#import <Foundation/Foundation.h>
#import <CoreWLAN/CoreWLAN.h>

namespace apfpv {
namespace {

class WifiLinkMac final : public WifiLink {
public:
    explicit WifiLinkMac(bool /*preferUsb*/) {}
    ~WifiLinkMac() override { close(); }

    bool open() override {
        @autoreleasepool {
            CWWiFiClient* client = [CWWiFiClient sharedWiFiClient];
            if (!client) return false;
            CWInterface* iface = [client interface];
            if (!iface) return false;
            _iface = [iface retain];
            NSString* n = [iface interfaceName];
            if (n) _name = std::string([n UTF8String]);
            return true;
        }
    }

    bool poll(WifiLinkSample& out) override {
        if (!_iface) return false;
        @autoreleasepool {
            CWInterface* iface = (CWInterface*) _iface;
            NSInteger rssi = [iface rssiValue];
            double rateMbps = [iface transmitRate];
            bool any = false;
            // rssiValue is 0 both when unassociated and when Location Services
            // permission is missing; either way it is not a reading.
            if (rssi < 0 && rssi > -110) { out.rssiDbm = (int) rssi; any = true; }
            if (rateMbps > 0.0) { out.rxRateKbps = (unsigned) (rateMbps * 1000.0); any = true; }
            return any;
        }
    }

    void close() override {
        if (_iface) { [(CWInterface*) _iface release]; _iface = nullptr; }
    }

    std::string ifaceName() const override { return _name; }
    bool hasRxRate() const override { return true; }

private:
    void*       _iface = nullptr;   // CWInterface*, kept opaque out of the header
    std::string _name;
};

} // namespace

std::unique_ptr<WifiLink> makeWifiLink(bool preferUsb) {
    return std::make_unique<WifiLinkMac>(preferUsb);
}

} // namespace apfpv

#endif // __APPLE__
