#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

static DWORD FindProcessId(const char* imageName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 entry;
    ZeroMemory(&entry, sizeof(entry));
    entry.dwSize = sizeof(entry);

    DWORD found = 0;
    if (Process32First(snapshot, &entry)) {
        do {
            if (_stricmp(entry.szExeFile, imageName) == 0) {
                found = entry.th32ProcessID;
                break;
            }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

int main(int argc, char** argv) {
    char input[MAX_PATH] = {0};
    const char* requestedPath = nullptr;

    if (argc >= 2) {
        requestedPath = argv[1];
    } else {
        printf("Path to triggerbot.dll (drag the DLL into this window or paste full path):\n> ");
        if (!fgets(input, sizeof(input), stdin)) return 1;
        input[strcspn(input, "\r\n")] = 0;
        // Accept paths pasted with surrounding quotes (common with drag-and-drop).
        size_t inputLength = strlen(input);
        if (inputLength >= 2 && input[0] == '\"' && input[inputLength - 1] == '\"') {
            memmove(input, input + 1, inputLength - 2);
            input[inputLength - 2] = 0;
        }
        requestedPath = input;
    }

    if (!requestedPath || !requestedPath[0]) {
        fprintf(stderr, "No DLL path supplied.\n");
        return 1;
    }

    char fullPath[MAX_PATH] = {0};
    DWORD pathLength = GetFullPathNameA(requestedPath, MAX_PATH, fullPath, nullptr);
    if (pathLength == 0 || pathLength >= MAX_PATH) {
        fprintf(stderr, "Could not resolve DLL path (error %lu).\n", GetLastError());
        return 1;
    }

    if (GetFileAttributesA(fullPath) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "DLL file does not exist: %s\n", fullPath);
        return 1;
    }

    DWORD pid = FindProcessId("hl.exe");
    if (!pid) pid = FindProcessId("cs.exe");
    if (!pid) pid = FindProcessId("hl2.exe");
    if (!pid) {
        fprintf(stderr, "CS 1.6 process hl.exe, cs.exe or hl2.exe was not found. Start the game first.\n");
        return 1;
    }

    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!process) {
        fprintf(stderr, "OpenProcess failed (error %lu). Run the injector with the same privilege level as CS 1.6.\n", GetLastError());
        return 1;
    }

    const SIZE_T bytes = (SIZE_T)strlen(fullPath) + 1;
    LPVOID remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        fprintf(stderr, "VirtualAllocEx failed (error %lu).\n", GetLastError());
        CloseHandle(process);
        return 1;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, fullPath, bytes, &written) || written != bytes) {
        fprintf(stderr, "WriteProcessMemory failed (error %lu).\n", GetLastError());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }

    HMODULE kernel = GetModuleHandleA("kernel32.dll");
    FARPROC loadLibrary = kernel ? GetProcAddress(kernel, "LoadLibraryA") : nullptr;
    if (!loadLibrary) {
        fprintf(stderr, "Could not find LoadLibraryA.\n");
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }

    HANDLE remoteThread = CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary),
        remotePath, 0, nullptr);
    if (!remoteThread) {
        fprintf(stderr, "CreateRemoteThread failed (error %lu).\n", GetLastError());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }

    WaitForSingleObject(remoteThread, 5000);
    DWORD threadExitCode = 0;
    GetExitCodeThread(remoteThread, &threadExitCode);
    CloseHandle(remoteThread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);

    if (threadExitCode == 0) {
        fprintf(stderr, "LoadLibraryA returned NULL; the DLL probably failed to load.\n");
        return 1;
    }

    printf("DLL load request completed for PID %lu.\n", pid);
    printf("In the game, press Right Shift to toggle the triggerbot; press END to unload it.\n");
    printf("Test only in an offline game with bots.\n");
    return 0;
}
