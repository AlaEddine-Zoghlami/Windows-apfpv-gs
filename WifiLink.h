#pragma once
// ---------------------------------------------------------------------------
// WifiLink — platform abstraction for reading the local Wi-Fi radio's link
// quality (RSSI, and the PHY rate where the OS exposes it).
//
// WHY THIS EXISTS. This is the only genuinely non-portable thing the ground
// station needs: every OS exposes it through a different API, and none of them
// resemble each other.
//     Windows   wlanapi          WlanQueryInterface(rssi / current_connection)
//     Linux     /proc/net/wireless   (same numbers iwconfig prints)
//     macOS     CoreWLAN         CWInterface.rssiValue (Objective-C)
// It used to be an #ifdef block inside LinkStats, which meant the non-Windows
// paths silently reported "unknown" and the player carried Win32 types in its
// main translation unit. One interface + one file per platform keeps the OS
// specifics out of the player entirely.
//
// CONTRACT. open() may fail (no radio, no permission, unsupported OS) and that
// is NOT an error the caller should treat as fatal -- the video path does not
// depend on any of this. A failed open() simply means poll() reports nothing,
// and the UI shows "--" rather than a fabricated value.
//
// Values are "as the driver reports them"; no smoothing or scaling happens
// here, so callers (LinkStats, LqFeedback) remain the single place that decides
// how a dBm becomes a percentage.
// ---------------------------------------------------------------------------

#include <memory>
#include <string>

namespace apfpv {

struct WifiLinkSample {
    int      rssiDbm    = 0;   // 0 = unknown. Negative dBm when known.
    unsigned rxRateKbps = 0;   // 0 = unknown/not exposed by this platform.
};

class WifiLink {
public:
    virtual ~WifiLink() = default;

    /// Acquire the radio handle / locate the interface. False = unavailable;
    /// the caller should carry on without link stats.
    virtual bool open() = 0;

    /// Read current values. Returns false if nothing could be read this time
    /// (transient failures are normal on a link that comes and goes).
    /// Leaves `out` untouched on failure, so the caller keeps its last value.
    virtual bool poll(WifiLinkSample& out) = 0;

    virtual void close() = 0;

    /// Human-readable name of the interface actually being read (for logs), or
    /// empty if unknown. Helps when a machine has several radios.
    virtual std::string ifaceName() const { return {}; }

    /// True when this platform can report a PHY rate at all, so the UI can
    /// distinguish "not supported here" from "supported but currently unknown".
    virtual bool hasRxRate() const { return false; }
};

/// Returns the implementation for the current platform. Never null: on an
/// unsupported OS it returns a null-object whose open() fails cleanly, so
/// callers never need a platform #ifdef of their own.
///
/// `preferUsb`: when several radios are present, prefer a USB one -- on this
/// project that is the FPV dongle rather than the laptop's built-in card.
std::unique_ptr<WifiLink> makeWifiLink(bool preferUsb = true);

} // namespace apfpv
