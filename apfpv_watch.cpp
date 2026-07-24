// apfpv_watch.cpp — one-click launcher: starts lqfeedback_cli.exe in the
// background, then apfpv_player.exe (our custom hardware-decode viewer with
// the FPS/resolution overlay and REC button) on the APFPV RTP stream. When
// the player window is closed, lqfeedback_cli is killed too, so nothing is
// left running.
//
// Build: g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ apfpv_watch.cpp -o apfpv_watch.exe
#include <windows.h>
#include <string>
#include <cstdio>

static PROCESS_INFORMATION launch(std::wstring cmdline, const std::wstring& cwd, bool showConsole)
{
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    DWORD flags = showConsole ? 0 : CREATE_NO_WINDOW;
    if (!CreateProcessW(NULL, &cmdline[0], NULL, NULL, FALSE, flags, NULL, cwd.c_str(), &si, &pi)) {
        pi.hProcess = NULL;
    }
    return pi;
}

int main()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));

    std::wstring lqCmd = L"\"" + dir + L"\\lqfeedback_cli.exe\"";
    std::wstring playerCmd = L"\"" + dir + L"\\apfpv_player.exe\"";

    printf("Starting LQ feedback...\n");
    PROCESS_INFORMATION lqPi = launch(lqCmd, dir, false);
    if (!lqPi.hProcess) printf("  WARNING: lqfeedback_cli.exe failed to start.\n");

    printf("Starting video (apfpv_player, fullscreen)...\n");
    PROCESS_INFORMATION ffPi = launch(playerCmd, dir, true);
    if (!ffPi.hProcess) {
        printf("  FATAL: apfpv_player.exe failed to start.\n");
        if (lqPi.hProcess) { TerminateProcess(lqPi.hProcess, 0); CloseHandle(lqPi.hProcess); }
        return 1;
    }

    WaitForSingleObject(ffPi.hProcess, INFINITE);
    printf("Video window closed -- stopping LQ feedback...\n");

    if (lqPi.hProcess) {
        TerminateProcess(lqPi.hProcess, 0);
        CloseHandle(lqPi.hProcess);
        CloseHandle(lqPi.hThread);
    }
    CloseHandle(ffPi.hProcess);
    CloseHandle(ffPi.hThread);
    printf("Done.\n");
    return 0;
}
