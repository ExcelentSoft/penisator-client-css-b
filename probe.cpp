// CS:S v34 probe: logs which Source interfaces this build exposes. Writes css_probe.txt next to the DLL.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

static HMODULE g_self;
static FILE* g_log;

typedef void* (__cdecl* CreateInterfaceFn)(const char*, int*);

static void ProbeModule(const char* mod) {
    HMODULE m = GetModuleHandleA(mod);
    if (!m) { fprintf(g_log, "[%s] not loaded\n", mod); return; }
    CreateInterfaceFn ci = (CreateInterfaceFn)GetProcAddress(m, "CreateInterface");
    fprintf(g_log, "[%s] base=%p CreateInterface=%p\n", mod, (void*)m, (void*)ci);
    if (!ci) return;
    static const char* prefixes[] = {
        "VEngineClient", "VClient", "VClientEntityList", "VEngineTrace", "EngineTraceClient", "EngineTraceServer",
        "VModelInfoClient", "VModelInfoServer", "VEngineModel", "VEngineCvar", "VDebugOverlay", "VGUI_Surface",
        "VGUI_Panel", "GameMovement", "VClientPrediction", "ServerGameDLL", "ServerGameClients", "ServerGameEnts",
        "PlayerInfoManager", "VEngineServer", "BotManager", "VEngineRandom", "VEngineEffects", "GAMEEVENTSMANAGER",
        "GAMEEVENTSMANAGER002", "VSERVERTOOLS", "VCLIENTTOOLS", "VStudioRender", "VEngineHud", "IEngineSoundClient",
        "VClientDllPrediction", "VRenderView", "VModelRender", "VPhysicsCollision", "ISERVERPLUGINHELPERS"
    };
    for (auto pr : prefixes) {
        for (int n = 0; n <= 40; ++n) {
            char name[96];
            if (n == 0) sprintf_s(name, "%s", pr); else sprintf_s(name, "%s%03d", pr, n);
            int ret = 0;
            void* p = nullptr;
            __try { p = ci(name, &ret); } __except (EXCEPTION_EXECUTE_HANDLER) { p = nullptr; }
            if (p) fprintf(g_log, "   FOUND %s -> %p  (vtable[0..3]: %p %p %p %p)\n", name, p,
                           (*(void***)p)[0], (*(void***)p)[1], (*(void***)p)[2], (*(void***)p)[3]);
        }
    }
}

static DWORD WINAPI Run(void*) {
    char path[MAX_PATH]; GetModuleFileNameA(g_self, path, MAX_PATH);
    char* sl = strrchr(path, '\\'); if (sl) *(sl + 1) = 0;
    strcat_s(path, "css_probe.txt");
    fopen_s(&g_log, path, "w");
    if (!g_log) return 1;
    Sleep(3000);
    fprintf(g_log, "CS:S probe. pid=%lu\n", GetCurrentProcessId());
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 me; me.dwSize = sizeof(me);
        if (Module32First(snap, &me)) do {
            fprintf(g_log, "MODULE %-28s base=%p size=%lu  %s\n", me.szModule, me.modBaseAddr, me.modBaseSize, me.szExePath);
        } while (Module32Next(snap, &me));
        CloseHandle(snap);
    }
    static const char* mods[] = { "engine.dll", "client.dll", "server.dll", "vstdlib.dll", "materialsystem.dll",
                                  "vguimatsurface.dll", "vgui2.dll", "studiorender.dll", "datacache.dll", "inputsystem.dll" };
    for (auto m : mods) ProbeModule(m);
    fprintf(g_log, "DONE\n");
    fclose(g_log);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst; DisableThreadLibraryCalls(inst);
        HANDLE t = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
