// WifiLink — fallback for platforms with no implementation (BSD, etc).
//
// Compiled ONLY when none of the platform files apply, so the ground station
// still links and runs there. It fails open() cleanly, which the contract in
// WifiLink.h defines as "carry on without link stats" -- video does not depend
// on any of this. A null object is preferable to leaving makeWifiLink()
// undefined, which would turn an unsupported OS into a link error.

#include "WifiLink.h"

#if !defined(_WIN32) && !defined(__linux__) && !defined(__APPLE__)

namespace apfpv {
namespace {
class WifiLinkNull final : public WifiLink {
public:
    bool open() override { return false; }
    bool poll(WifiLinkSample&) override { return false; }
    void close() override {}
};
} // namespace

std::unique_ptr<WifiLink> makeWifiLink(bool) { return std::make_unique<WifiLinkNull>(); }

} // namespace apfpv

#endif
