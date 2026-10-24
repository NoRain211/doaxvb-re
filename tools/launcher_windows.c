/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Windows entry point for the PowerShell launcher, so the file carries the game icon. */
#include <windows.h>
#include <strsafe.h>

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show) {
    wchar_t directory[1024], script[2048], line[4096];
    DWORD length = GetModuleFileNameW(NULL, directory, ARRAYSIZE(directory));
    (void)instance; (void)previous; (void)command; (void)show;
    if (length == 0 || length >= ARRAYSIZE(directory)) {
        MessageBoxW(NULL, L"Could not locate the launcher.", L"DOAXBV", MB_ICONERROR);
        return 1;
    }
    while (length && directory[length - 1] != L'\\') --length;
    directory[length] = L'\0';
    StringCchPrintfW(script, ARRAYSIZE(script), L"%stools\\launcher.ps1", directory);
    if (GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"tools\\launcher.ps1 is missing. Run BuildGame.cmd to set up this copy.",
                    L"DOAXBV", MB_ICONERROR);
        return 1;
    }
    StringCchPrintfW(line, ARRAYSIZE(line),
        L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%s\"", script);
    STARTUPINFOW start = {0};
    PROCESS_INFORMATION process;
    start.cb = sizeof start;
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, directory,
                        &start, &process)) {
        MessageBoxW(NULL, L"Could not start the launcher.", L"DOAXBV", MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
