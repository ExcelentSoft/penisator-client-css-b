// CS:S v34 probe #4: self-test of layouts + TraceRay vtable scan -> css_probe4.txt
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static HMODULE g_self;
static FILE* g_log;
typedef void* (__cdecl* CreateInterfaceFn)(const char*, int*);

// netvar offsets found by probe #3
enum { OFF_TEAM = 144, OFF_LIFE = 135, OFF_HEALTH = 3492, OFF_ORIGIN = 668, OFF_FLAGS = 692,
       OFF_VIEWOFS = 212, OFF_VEL = 224, OFF_PUNCH = 2992, OFF_FOV = 3356, OFF_ACTWEAPON = 2872 };

template <class T> static T Rd(void* base, int off) { return *(T*)((unsigned char*)base + off); }

struct __declspec(align(16)) VecA { float x, y, z, w; };
struct __declspec(align(16)) Ray_t { VecA start, delta, startOffset, extents; bool isRay; bool isSwept; };

static void* g_localEnt = nullptr;
static bool __fastcall F_ShouldHit(void*, void*, void* ent, int) { return ent != g_localEnt; }
static int  __fastcall F_TraceType(void*, void*) { return 0; }
static void* g_filterVtbl[2] = { (void*)&F_ShouldHit, (void*)&F_TraceType };
struct Filter { void** vtbl; };

typedef void (__thiscall* TraceRayFn)(void*, const Ray_t&, unsigned, void*, void*);

static DWORD WINAPI Run(void*) {
    char path[MAX_PATH]; GetModuleFileNameA(g_self, path, MAX_PATH);
    char* sl = strrchr(path, '\\'); if (sl) *(sl + 1) = 0;
    strcat_s(path, "css_probe4.txt");
    fopen_s(&g_log, path, "w");
    if (!g_log) return 1;
    Sleep(3000);
    HMODULE cl = GetModuleHandleA("client.dll"), en = GetModuleHandleA("engine.dll");
    CreateInterfaceFn ciC = (CreateInterfaceFn)GetProcAddress(cl, "CreateInterface");
    CreateInterfaceFn ciE = (CreateInterfaceFn)GetProcAddress(en, "CreateInterface");
    void** engine = (void**)ciE("VEngineClient012", nullptr);
    void** entlist = (void**)ciC("VClientEntityList003", nullptr);
    void** trace = (void**)ciE("EngineTraceClient003", nullptr);
    void** evt = *(void***)engine; void** lvt = *(void***)entlist;
    fprintf(g_log, "client.dll size check: see probe1 (4689920)\n");

    __try {
        typedef int (__thiscall* IntFn)(void*);
        typedef bool (__thiscall* BoolFn)(void*);
        typedef void (__thiscall* VecFn)(void*, float*);
        typedef void* (__thiscall* EntFn)(void*, int);
        typedef bool (__thiscall* PInfoFn)(void*, int, void*);
        int local = ((IntFn)evt[12])(engine);
        int maxc = ((IntFn)evt[21])(engine);
        bool ingame = ((BoolFn)evt[26])(engine);
        float va[3]; ((VecFn)evt[19])(engine, va);
        fprintf(g_log, "IsInGame=%d localIdx=%d maxClients=%d viewangles=(%.2f %.2f %.2f)\n", ingame, local, maxc, va[0], va[1], va[2]);
        int highest = ((IntFn)lvt[6])(entlist);
        fprintf(g_log, "HighestEntityIndex=%d\n", highest);
        EntFn getEnt = (EntFn)lvt[3];
        for (int i = 1; i <= maxc && i <= 64; ++i) {
            void* e = getEnt(entlist, i);
            if (!e) continue;
            char pi[400]; memset(pi, 0, sizeof(pi));
            bool okp = ((PInfoFn)evt[8])(engine, i, pi);
            fprintf(g_log, "ENT %d ptr=%p name='%.32s' (pinfo=%d) team=%d life=%d hp=%d flags=%08X origin=(%.1f %.1f %.1f) viewofs=(%.1f %.1f %.1f) vel=(%.1f %.1f %.1f) fov=%d%s\n",
                    i, e, pi, okp, Rd<int>(e, OFF_TEAM), (int)Rd<unsigned char>(e, OFF_LIFE), Rd<int>(e, OFF_HEALTH), Rd<int>(e, OFF_FLAGS),
                    Rd<float>(e, OFF_ORIGIN), Rd<float>(e, OFF_ORIGIN + 4), Rd<float>(e, OFF_ORIGIN + 8),
                    Rd<float>(e, OFF_VIEWOFS), Rd<float>(e, OFF_VIEWOFS + 4), Rd<float>(e, OFF_VIEWOFS + 8),
                    Rd<float>(e, OFF_VEL), Rd<float>(e, OFF_VEL + 4), Rd<float>(e, OFF_VEL + 8), Rd<int>(e, OFF_FOV),
                    i == local ? "  <-- LOCAL" : "");
            if (i == local) fprintf(g_log, "      punch=(%.2f %.2f %.2f)\n", Rd<float>(e, OFF_PUNCH), Rd<float>(e, OFF_PUNCH + 4), Rd<float>(e, OFF_PUNCH + 8));
        }
        void* me = getEnt(entlist, local);
        g_localEnt = me;
        if (me) {
            float eye[3] = { Rd<float>(me, OFF_ORIGIN) + Rd<float>(me, OFF_VIEWOFS), Rd<float>(me, OFF_ORIGIN + 4) + Rd<float>(me, OFF_VIEWOFS + 4),
                             Rd<float>(me, OFF_ORIGIN + 8) + Rd<float>(me, OFF_VIEWOFS + 8) };
            const float d2r = 3.14159265f / 180.0f;
            float fwd[3] = { cosf(va[0] * d2r) * cosf(va[1] * d2r), cosf(va[0] * d2r) * sinf(va[1] * d2r), -sinf(va[0] * d2r) };
            fprintf(g_log, "EYE=(%.1f %.1f %.1f) fwd=(%.3f %.3f %.3f)\n", eye[0], eye[1], eye[2], fwd[0], fwd[1], fwd[2]);
            Ray_t ray; memset(&ray, 0, sizeof(ray));
            ray.start.x = eye[0]; ray.start.y = eye[1]; ray.start.z = eye[2];
            ray.delta.x = fwd[0] * 8192; ray.delta.y = fwd[1] * 8192; ray.delta.z = fwd[2] * 8192;
            ray.isRay = true; ray.isSwept = true;
            Filter filt = { g_filterVtbl };
            void** tvt = *(void***)trace;
            fprintf(g_log, "TRACE iface=%p vtbl=%p sizeof(Ray_t)=%d\n", (void*)trace, (void*)tvt, (int)sizeof(Ray_t));
            for (int idx = 0; idx < 8; ++idx) {
                __declspec(align(16)) unsigned int buf[64];
                for (int k = 0; k < 64; ++k) buf[k] = 0xCDCDCDCD;
                bool crashed = false;
                __try { ((TraceRayFn)tvt[idx])(trace, ray, 0x4600400B, &filt, buf); } __except (EXCEPTION_EXECUTE_HANDLER) { crashed = true; }
                fprintf(g_log, "  TRY vt[%d]%s:", idx, crashed ? " (exception)" : "");
                for (int k = 0; k < 24; ++k) fprintf(g_log, " %08X", buf[k]);
                float fr = *(float*)&buf[11];
                fprintf(g_log, "  | fraction@11=%.4f endpos=(%.1f %.1f %.1f)\n", fr, *(float*)&buf[3], *(float*)&buf[4], *(float*)&buf[5]);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(g_log, "EXCEPTION in self-test\n");
    }
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
