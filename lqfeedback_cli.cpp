// lqfeedback_cli.cpp — standalone LQ-feedback sender, no dongle/libusb station
// link required. Reads the dongle's live RSSI via Windows' own WLAN API (same
// stock-driver infrastructure-mode connection PixelPilot_rk's Windows port
// uses) and feeds it into the real apfpv::LqFeedback sender (src/LqFeedback.*),
// which talks the exact UDP protocol aalink on the VTX expects
// (192.168.0.1:12345, "gs_string=gs rssi_a = NN(%)").
//
// Build (MinGW64): g++ -std=c++17 -O2 lqfeedback_cli.cpp LqFeedback.cpp -lwlanapi -lws2_32 -o lqfeedback_cli.exe
// Run: lqfeedback_cli.exe [air-ip, default 192.168.0.1]
#include "LqFeedback.h"
#include <windows.h>
#include <wlanapi.h>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

static std::atomic<bool> g_run{true};
static void onSigint(int) { g_run = false; }

static bool findDongleGuid(HANDLE hClient, GUID* outGuid, char* descOut, size_t descSz)
{
    WLAN_INTERFACE_INFO_LIST* list = NULL;
    bool found = false;
    if (WlanEnumInterfaces(hClient, NULL, &list) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < list->dwNumberOfItems; i++) {
            char desc[256];
            WideCharToMultiByte(CP_UTF8, 0, list->InterfaceInfo[i].strInterfaceDescription, -1, desc, sizeof(desc), NULL, NULL);
            if (strstr(desc, "USB")) {   // the removable dongle, not built-in/PCIe Wi-Fi
                *outGuid = list->InterfaceInfo[i].InterfaceGuid;
                snprintf(descOut, descSz, "%s", desc);
                found = true;
                break;
            }
        }
        WlanFreeMemory(list);
    }
    return found;
}

static int queryRssiDbm(HANDLE hClient, const GUID* guid)
{
    DWORD dataSize = 0;
    PVOID data = NULL;
    int rssi = -80;   // safe fallback if the query fails (e.g. momentarily disconnected)
    if (WlanQueryInterface(hClient, guid, wlan_intf_opcode_rssi, NULL, &dataSize, &data, NULL) == ERROR_SUCCESS && data) {
        rssi = *(LONG*)data;
        WlanFreeMemory(data);
    }
    return rssi;
}

int main(int argc, char** argv)
{
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    signal(SIGINT, onSigint);

    const char* airIp = argc > 1 ? argv[1] : "192.168.0.1";

    HANDLE hClient = NULL; DWORD negotiated = 0;
    if (WlanOpenHandle(2, NULL, &negotiated, &hClient) != ERROR_SUCCESS) {
        fprintf(stderr, "WlanOpenHandle failed.\n");
        return 1;
    }

    GUID guid; char desc[256];
    if (!findDongleGuid(hClient, &guid, desc, sizeof(desc))) {
        fprintf(stderr, "No USB Wi-Fi adapter found.\n");
        WlanCloseHandle(hClient, NULL);
        return 1;
    }
    printf("Adapter: %s\n", desc);
    printf("Sending LQ feedback to %s:12345 (Ctrl+C to stop)...\n", airIp);

    apfpv::LqFeedback feedback;   // default Config: FixedTimer, 33ms interval
    if (!feedback.start(airIp, 12345)) {
        fprintf(stderr, "LqFeedback::start failed.\n");
        WlanCloseHandle(hClient, NULL);
        return 1;
    }

    while (g_run) {
        int rssi = queryRssiDbm(hClient, &guid);
        /* Windows' WLAN API only exposes one aggregate RSSI per connection, not
         * per-antenna chains -- but the dongle is a real 2x2 MIMO adapter, and
         * aalink's downlink% looks like a combined metric across rssi_a/rssi_b.
         * Leaving rssi_b unset (the single-arg call) may default it to 0 on the
         * VTX side and drag the combined value to 0% even though rssi_a parses
         * fine (matches the observed symptom: the line shows, but reads 0%).
         * Duplicate the one real reading into both slots instead of omitting it. */
        feedback.update(rssi, rssi);
        printf("\rRSSI: %4d dBm (%3d%%)   ", rssi, apfpv::LqFeedback::rssiPct(rssi));
        fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    feedback.stop();
    WlanCloseHandle(hClient, NULL);
    printf("\nStopped.\n");
    return 0;
}
