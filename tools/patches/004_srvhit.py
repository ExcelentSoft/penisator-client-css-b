import sys
MARK = '[patch:004_srvhit]'
path = sys.argv[1]
s = open(path, encoding='utf-8', newline='').read().replace('\r\n', '\n')
if MARK in s:
    print('patch 004 already applied'); sys.exit(0)

def rep(a, b):
    global s
    if s.count(a) != 1:
        print('PATCH ERROR: pattern count %d for: %s' % (s.count(a), a[:90])); sys.exit(1)
    s = s.replace(a, b)

# 1) shared state
rep('    volatile LONG g_headMode = 0;', '''    // rage: shot target handed to the server-side hook
    DWORD g_rTick = 0; volatile LONG g_rValid = 0; float g_rEye[3] = { 0, 0, 0 }, g_rPt[3] = { 0, 0, 0 };
    volatile LONG g_fbHooked = 0, g_fbBad = 0, g_fbCalls = 0, g_fbRedirects = 0;
    volatile LONG g_headMode = 0;''')

# 2) remember the chosen point in the rage block; with the server hook the camera is left alone
rep('            float pitch, yaw;\n            if (found) {\n                const float dx = bp[0] - eyeR[0]',
    '''            if (found) { g_rEye[0] = eye[0]; g_rEye[1] = eye[1]; g_rEye[2] = eye[2]; g_rPt[0] = bp[0]; g_rPt[1] = bp[1]; g_rPt[2] = bp[2]; g_rTick = GetTickCount(); g_rValid = 1; }
            else g_rValid = 0;
            float pitch, yaw;
            if (found) {
                const float dx = bp[0] - eyeR[0]''')
rep('            if (pitch > 89.0f) pitch = 89.0f; if (pitch < -89.0f) pitch = -89.0f;\n            const float dd = (yaw - cmd->viewangles[1])',
    '            if (pitch > 89.0f) pitch = 89.0f; if (pitch < -89.0f) pitch = -89.0f;\n            if (g_fbHooked && found) { pitch = cmd->viewangles[0]; yaw = cmd->viewangles[1]; }   // server hook does the aiming\n            const float dd = (yaw - cmd->viewangles[1])')

# 3) server hook: CCSPlayer::FireBullet (one call per bullet)
hook = '''    void LogMsg(const char* fmt, ...);
    // ---------------- server side: CCSPlayer::FireBullet (server.dll, in-process listen server) ----------------
    typedef void (__fastcall* FireBulletFn)(void*, void*, float, float, float, const float*, float, float, int, int, int, float, void*, int, float, float);
    FireBulletFn g_origFireBullet = nullptr;
    constexpr DWORD kServerImageSize = 0x642000, kFireBulletRva = 0x2F4470;
    const unsigned char kFireBulletSig[] = { 0x81, 0xEC, 0xA0, 0x03, 0x00, 0x00, 0xDB, 0x84, 0x24, 0xC4, 0x03, 0x00, 0x00, 0x53, 0x55, 0x56, 0x57 };

    void __fastcall HookFireBullet(void* self, void* edx, float sx, float sy, float sz, const float* ang, float spread, float dist,
                                   int pen, int btype, int dmg, float rmod, void* attacker, int fx, float x, float y) {
        InterlockedIncrement(&g_activeHooks);
        float a2[3];
        InterlockedIncrement(&g_fbCalls);
        if (g_opt.rage && g_rValid && InterlockedCompareExchange(&g_botsOnly, 0, 0) && (GetTickCount() - g_rTick) < 250) {
            const float ex = sx - g_rEye[0], ey = sy - g_rEye[1], ez = sz - g_rEye[2];
            if (ex * ex + ey * ey + ez * ez < 48.0f * 48.0f) {                      // the shooter is the local player
                const float dx = g_rPt[0] - sx, dy = g_rPt[1] - sy, dz = g_rPt[2] - sz;
                a2[0] = -atan2f(dz, sqrtf(dx * dx + dy * dy)) * 180.0f / kPi;
                a2[1] = atan2f(dy, dx) * 180.0f / kPi;
                a2[2] = 0.0f;
                ang = a2; spread = 0.0f; x = 0.0f; y = 0.0f;
                InterlockedIncrement(&g_fbRedirects);
            }
        }
        g_origFireBullet(self, edx, sx, sy, sz, ang, spread, dist, pen, btype, dmg, rmod, attacker, fx, x, y);
        InterlockedDecrement(&g_activeHooks);
    }

    bool InstallServerHook() {
        if (InterlockedCompareExchange(&g_fbBad, 0, 0)) return false;
        HMODULE sv = GetModuleHandleA("server.dll");
        if (!sv) return false;
        auto dos = (IMAGE_DOS_HEADER*)sv; auto nt = (IMAGE_NT_HEADERS*)((unsigned char*)sv + dos->e_lfanew);
        unsigned char* fn = (unsigned char*)sv + kFireBulletRva;
        if (nt->OptionalHeader.SizeOfImage != kServerImageSize || memcmp(fn, kFireBulletSig, sizeof(kFireBulletSig)) != 0) {
            InterlockedExchange(&g_fbBad, 1); LogMsg("server.dll: different build, server hook skipped"); return false;
        }
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return false;
        if (MH_CreateHook(fn, (void*)&HookFireBullet, (void**)&g_origFireBullet) != MH_OK) { InterlockedExchange(&g_fbBad, 1); return false; }
        if (MH_EnableHook(fn) != MH_OK) { InterlockedExchange(&g_fbBad, 1); return false; }
        InterlockedExchange(&g_fbHooked, 1);
        return true;
    }

    // ---------------- hook ----------------
'''
rep('    // ---------------- hook ----------------\n', hook)

# 4) install from the worker loop (server.dll may load later than the client)
rep('        bool renderOk = false; DWORD lastTry = 0; int tries = 0;\n', '        bool renderOk = false; DWORD lastTry = 0; int tries = 0; DWORD lastSv = 0;\n')
rep('            const bool fg = GameForeground();\n            const bool tog = fg &&', '''            if (!InterlockedCompareExchange(&g_fbHooked, 0, 0) && now - lastSv >= 1000) {
                lastSv = now;
                if (InterlockedCompareExchange(&g_ifaceReady, 0, 0) && InstallServerHook()) LogMsg("server FireBullet hooked");
            }
            const bool fg = GameForeground();
            const bool tog = fg &&''')

# 5) menu: status of the server hook
rep('"Работает, пока стреляешь. FOV не нужен."', '"Пуля летит строго в цель, без разброса."')
rep('            Text(u.dc, x + 6, y, "Цель и голову берёт из вкладки Аим.", kDim);\n', '            Text(u.dc, x + 6, y, "Цель и голову берёт из вкладки Аим.", kDim);                 y += 24;\n            Text(u.dc, x + 6, y, InterlockedCompareExchange(&g_fbHooked, 0, 0) ? "Серверный хук: активен" : "Серверный хук: не найден", kDim);\n')

s = '// ' + MARK + ' applied\n' + s
open(path, 'w', encoding='utf-8', newline='').write(s)
print('patch 004 applied')
