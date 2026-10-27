// CS:S v34 probe #2: dumps netvar tables (offsets) and client vtable code bytes -> css_probe2.txt
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static HMODULE g_self;
static FILE* g_log;
typedef void* (__cdecl* CreateInterfaceFn)(const char*, int*);

struct RecvTable;
struct RecvProp {
    const char* m_pVarName; int m_RecvType; int m_Flags; int m_StringBufferSize; bool m_bInsideArray;
    const void* m_pExtraData; RecvProp* m_pArrayProp; void* m_ArrayLengthProxy; void* m_ProxyFn;
    void* m_DataTableProxyFn; RecvTable* m_pDataTable; int m_Offset; int m_ElementStride; int m_nElements;
    const char* m_pParentArrayPropName;
};
struct RecvTable { RecvProp* m_pProps; int m_nProps; void* m_pDecoder; const char* m_pNetTableName; bool m_bInitialized; bool m_bInMainList; };
struct ClientClass { void* m_pCreateFn; void* m_pCreateEventFn; const char* m_pNetworkName; RecvTable* m_pRecvTable; ClientClass* m_pNext; int m_ClassID; };

static bool Wanted(const char* n) {
    static const char* w[] = { "CCSPlayer", "CBasePlayer", "CBaseEntity", "CBaseCombatCharacter", "CBaseAnimating",
                               "CBaseCombatWeapon", "CWeaponCSBase", "CCSPlayerResource", "CPlayerResource",
                               "CCSTeam", "CTeam", "CAK47", "CWeaponAWP" };
    for (auto s : w) if (strcmp(n, s) == 0) return true;
    return false;
}

static void DumpTable(RecvTable* t, int base, int depth, const char* path) {
    if (!t || depth > 5) return;
    for (int i = 0; i < t->m_nProps; ++i) {
        RecvProp* p = &t->m_pProps[i];
        if (!p->m_pVarName) continue;
        if (p->m_pDataTable) {
            char np[160]; sprintf_s(np, "%s.%s", path, p->m_pVarName);
            DumpTable(p->m_pDataTable, base + p->m_Offset, depth + 1, np);
        } else {
            fprintf(g_log, "   %s.%s type=%d off=%d (abs %d)\n", path, p->m_pVarName, p->m_RecvType, p->m_Offset, base + p->m_Offset);
        }
    }
}

static DWORD WINAPI Run(void*) {
    char path[MAX_PATH]; GetModuleFileNameA(g_self, path, MAX_PATH);
    char* sl = strrchr(path, '\\'); if (sl) *(sl + 1) = 0;
    strcat_s(path, "css_probe2.txt");
    fopen_s(&g_log, path, "w");
    if (!g_log) return 1;
    Sleep(3000);
    __try {
        HMODULE cl = GetModuleHandleA("client.dll");
        CreateInterfaceFn ci = (CreateInterfaceFn)GetProcAddress(cl, "CreateInterface");
        void** vc = (void**)ci("VClient013", nullptr);
        fprintf(g_log, "VClient013=%p client.dll base=%p\n", (void*)vc, (void*)cl);
        void** vt = *(void***)vc;
        for (int i = 0; i < 45; ++i) {
            unsigned char* f = (unsigned char*)vt[i];
            fprintf(g_log, "VCLIENT[%d] rva=%08lX :", i, (unsigned long)((unsigned char*)f - (unsigned char*)cl));
            for (int b = 0; b < 24; ++b) fprintf(g_log, " %02X", f[b]);
            fprintf(g_log, "\n");
        }
        typedef ClientClass* (__thiscall* GetAllClassesFn)(void*);
        ClientClass* cc = ((GetAllClassesFn)vt[6])(vc);
        fprintf(g_log, "GetAllClasses -> %p\n", (void*)cc);
        int n = 0;
        for (ClientClass* c = cc; c && n < 600; c = c->m_pNext, ++n)
            fprintf(g_log, "CLASS %d id=%d %s table=%s\n", n, c->m_ClassID, c->m_pNetworkName, c->m_pRecvTable ? c->m_pRecvTable->m_pNetTableName : "?");
        for (ClientClass* c = cc; c; c = c->m_pNext)
            if (Wanted(c->m_pNetworkName)) {
                fprintf(g_log, "=== %s ===\n", c->m_pNetworkName);
                DumpTable(c->m_pRecvTable, 0, 0, c->m_pRecvTable->m_pNetTableName);
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(g_log, "EXCEPTION during dump\n");
    }
    // engine vtable pointers
    __try {
        HMODULE en = GetModuleHandleA("engine.dll");
        CreateInterfaceFn ci = (CreateInterfaceFn)GetProcAddress(en, "CreateInterface");
        void** ve = (void**)ci("VEngineClient012", nullptr);
        void** vt = *(void***)ve;
        fprintf(g_log, "VEngineClient012=%p\n", (void*)ve);
        for (int i = 0; i < 90; ++i) {
            unsigned char* f = (unsigned char*)vt[i];
            fprintf(g_log, "VENGINE[%d] rva=%08lX :", i, (unsigned long)((unsigned char*)f - (unsigned char*)en));
            for (int b = 0; b < 12; ++b) fprintf(g_log, " %02X", f[b]);
            fprintf(g_log, "\n");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(g_log, "EXCEPTION during engine dump\n");
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
