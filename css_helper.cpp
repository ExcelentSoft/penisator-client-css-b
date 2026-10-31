// CS:Source v34 (engine build 4044, client.dll 4689920 bytes) helper — OFFLINE / BOTS ONLY.
// Menu (RShift): triggerbot, enemy ESP, FOV aim assist. Locks itself if any real (non-bot) player is present.
// All offsets/indices below come from the probe logs of this exact build.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <MinHook.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {
    // ---------------- build-specific constants (from probes) ----------------
    constexpr DWORD kClientImageSize = 4689920;
    constexpr uintptr_t kCreateMoveRva = 0xF83F0;                 // CInput::CreateMove
    constexpr unsigned char kCreateMoveSig[9] = { 0x83, 0xEC, 0x10, 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x1C };
    constexpr int kCmdArrayOffset = 0xC4;                         // CInput::m_pCommands
    constexpr int kMultiplayerBackup = 90;
    constexpr int kCmdSize = 64;

    // netvar offsets (C_BasePlayer / C_CSPlayer)
    constexpr int OFF_TEAM = 144, OFF_LIFE = 135, OFF_HEALTH = 3492, OFF_ORIGIN = 668, OFF_FLAGS = 692,
                  OFF_VIEWOFS = 212, OFF_VEL = 224, OFF_PUNCH = 2992, OFF_FOV = 3356, OFF_DEFFOV = 3368,
                  OFF_ACTWEAPON = 2872, OFF_SIMTIME = 104;
    constexpr int FL_ONGROUND = 1 << 0, FL_DUCKING = 1 << 1;
    constexpr int IN_ATTACK = 1;
    constexpr int kPlayerInfoFakeOffset = 108;                    // player_info_t::fakeplayer

    // IVEngineClient012 vtable indices
    constexpr int E_GETSCREEN = 5, E_GETPLAYERINFO = 8, E_GETLOCAL = 12, E_GETVIEWANG = 19, E_SETVIEWANG = 20,
                  E_GETMAXCLIENTS = 21, E_ISINGAME = 26;
    // IClientEntityList003: GetClientEntity = 3
    constexpr int L_GETENT = 3;
    // IEngineTrace (EngineTraceClient003): TraceRay = 4
    constexpr int T_TRACERAY = 4;

    template <class T> T Rd(const void* base, int off) { return *(const T*)((const unsigned char*)base + off); }

    struct UserCmd {
        void* vtbl; int command_number; int tick_count; float viewangles[3];
        float forwardmove, sidemove, upmove; int buttons; unsigned char impulse;
        int weaponselect; int weaponsubtype; int random_seed; short mousedx, mousedy; bool hasbeenpredicted;
    };
    struct Ray { float start[3], delta[3], startOffset[3], extents[3]; bool isRay, isSwept; };

    // ---------------- options ----------------
    struct Opts {
        volatile LONG teamCheck = 1;
        volatile LONG ground = 1;
        volatile LONG speedCheck = 1;
        volatile LONG scopeCheck = 1;
        volatile LONG intervalPct = 100;
        volatile LONG esp = 0;
        volatile LONG wideFov = 1;
        volatile LONG aimOn = 0;
        volatile LONG aimHead = 1;
        volatile LONG aimFov = 10;
    } g_opt;

    HMODULE g_self = nullptr;
    volatile LONG g_enabled = 0;      // triggerbot
    volatile LONG g_menuOpen = 0;
    volatile LONG g_unloading = 0;
    volatile LONG g_activeHooks = 0;
    volatile LONG g_hooksReady = 0;
    volatile LONG g_status = 0;       // 0 ok, 1 interfaces missing, 2 unsupported client.dll / hook failed
    volatile LONG g_botsOnly = 0;     // 1 = verified: only bots present
    char g_lockWho[48] = "";
    HWND g_overlay = nullptr;
    HANDLE g_overlayThread = nullptr;
    LONGLONG g_qpcFreq = 0, g_nextShot = 0;

    // ---------------- engine interfaces ----------------
    typedef void* (__cdecl* CreateInterfaceFn)(const char*, int*);
    void** g_engine = nullptr; void** g_entlist = nullptr; void** g_trace = nullptr;
    void** EVT() { return *(void***)g_engine; }
    void** LVT() { return *(void***)g_entlist; }
    void** TVT() { return *(void***)g_trace; }
    volatile LONG g_ifaceReady = 0;

    typedef int (__thiscall* IntFn)(void*);
    typedef bool (__thiscall* BoolFn)(void*);
    typedef void (__thiscall* VecFn)(void*, float*);
    typedef void* (__thiscall* EntFn)(void*, int);
    typedef bool (__thiscall* PInfoFn)(void*, int, void*);
    typedef void (__thiscall* TraceRayFn)(void*, const Ray&, unsigned int, void*, void*);

    void* GetEnt(int i) { return ((EntFn)LVT()[L_GETENT])(g_entlist, i); }

    LONGLONG Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

    // ---------------- trace (wall check) ----------------
    void* g_playerPtrs[40]; int g_playerPtrCount = 0;
    bool __fastcall F_ShouldHit(void*, void*, void* ent, int) {
        for (int i = 0; i < g_playerPtrCount; ++i) if (g_playerPtrs[i] == ent) return false;
        return true;
    }
    int __fastcall F_TraceType(void*, void*) { return 0; }
    void* g_filterVtbl[2] = { (void*)&F_ShouldHit, (void*)&F_TraceType };
    struct Filter { void** vtbl; };

    // true if nothing solid (except players) lies between a and b
    bool ClearLine(const float* a, const float* b) {
        Ray r; memset(&r, 0, sizeof(r));
        for (int i = 0; i < 3; ++i) { r.start[i] = a[i]; r.delta[i] = b[i] - a[i]; }
        r.isRay = true; r.isSwept = true;
        Filter f = { g_filterVtbl };
        __declspec(align(16)) unsigned int buf[64];
        memset(buf, 0, sizeof(buf));
        ((TraceRayFn)TVT()[T_TRACERAY])(g_trace, r, 0x4600400B, &f, buf);
        const float fraction = *(float*)&buf[11];
        return fraction >= 0.97f;
    }

    // ---------------- math ----------------
    const float kPi = 3.14159265f;
    void AngleForward(const float* a, float* f) {
        const float d2r = kPi / 180.0f;
        const float p = a[0] * d2r, y = a[1] * d2r;
        f[0] = cosf(p) * cosf(y); f[1] = cosf(p) * sinf(y); f[2] = -sinf(p);
    }

    // ray p + t*d (t>=0) vs vertical cylinder; returns t or -1
    float RayCylinder(const float* p, const float* d, float cx, float cy, float z0, float z1, float r) {
        const float a = d[0] * d[0] + d[1] * d[1];
        if (a < 1e-8f) return -1.0f;
        const float ox = p[0] - cx, oy = p[1] - cy;
        const float b = 2.0f * (ox * d[0] + oy * d[1]);
        const float c = ox * ox + oy * oy - r * r;
        const float disc = b * b - 4.0f * a * c;
        if (disc < 0.0f) return -1.0f;
        const float sq = sqrtf(disc);
        float t = (-b - sq) / (2.0f * a);
        if (t < 0.0f) t = (-b + sq) / (2.0f * a);
        if (t < 0.0f) return -1.0f;
        const float z = p[2] + d[2] * t;
        return (z >= z0 && z <= z1) ? t : -1.0f;
    }
    float RaySphere(const float* p, const float* d, const float* c, float r) {
        const float ox = p[0] - c[0], oy = p[1] - c[1], oz = p[2] - c[2];
        const float b = ox * d[0] + oy * d[1] + oz * d[2];
        const float cc = ox * ox + oy * oy + oz * oz - r * r;
        const float disc = b * b - cc;
        if (disc < 0.0f) return -1.0f;
        float t = -b - sqrtf(disc);
        if (t < 0.0f) t = -b + sqrtf(disc);
        return t >= 0.0f ? t : -1.0f;
    }

    // ---------------- weapons ----------------
    enum WpnClass { W_NONE, W_PISTOL, W_SMG, W_RIFLE, W_SNIPER, W_SHOTGUN };
    struct WInfo { const char* name; WpnClass c; float interval; };
    const WInfo kWeapons[] = {
        { "CWeaponAWP", W_SNIPER, 1.50f }, { "CWeaponScout", W_SNIPER, 1.30f },
        { "CWeaponG3SG1", W_SNIPER, 0.30f }, { "CWeaponSG550", W_SNIPER, 0.30f },
        { "CDEagle", W_PISTOL, 0.30f }, { "CWeaponUSP", W_PISTOL, 0.23f }, { "CWeaponGlock", W_PISTOL, 0.23f },
        { "CWeaponP228", W_PISTOL, 0.23f }, { "CWeaponFiveSeven", W_PISTOL, 0.23f }, { "CWeaponElite", W_PISTOL, 0.23f },
        { "CWeaponM3", W_SHOTGUN, 0.95f }, { "CWeaponXM1014", W_SHOTGUN, 0.28f },
        { "CAK47", W_RIFLE, 0.20f }, { "CWeaponM4A1", W_RIFLE, 0.18f }, { "CWeaponGalil", W_RIFLE, 0.19f },
        { "CWeaponFamas", W_RIFLE, 0.17f }, { "CWeaponAug", W_RIFLE, 0.17f }, { "CWeaponSG552", W_RIFLE, 0.17f },
        { "CWeaponMP5Navy", W_SMG, 0.11f }, { "CWeaponUMP45", W_SMG, 0.13f }, { "CWeaponP90", W_SMG, 0.10f },
        { "CWeaponMAC10", W_SMG, 0.10f }, { "CWeaponTMP", W_SMG, 0.10f }, { "CWeaponM249", W_SMG, 0.12f },
    };
    char g_wpnName[40] = "?";
    float MaxSpeedFor(WpnClass c) {
        switch (c) { case W_SNIPER: return 135.0f; case W_RIFLE: return 110.0f; case W_SMG: return 130.0f;
                     case W_PISTOL: return 150.0f; default: return 9999.0f; }
    }

    // ClientClass* is read WITHOUT calling virtuals: find the tiny "mov eax, imm32; ret" thunk in the
    // entity's IClientNetworkable vtable (entity+8) and validate the struct it points at.
    const char* ClassNameOf(void* ent) {
        if (!ent || IsBadReadPtr((char*)ent + 8, 4)) return nullptr;
        void** nvt = *(void***)((char*)ent + 8);
        if (!nvt || IsBadReadPtr(nvt, 4 * 6)) return nullptr;
        for (int k = 0; k < 6; ++k) {
            const unsigned char* f = (const unsigned char*)nvt[k];
            if (!f || IsBadReadPtr(f, 6)) continue;
            if (f[0] == 0xB8 && f[5] == 0xC3) {
                const uint32_t ccAddr = *(const uint32_t*)(f + 1);
                const unsigned char* cc = (const unsigned char*)(uintptr_t)ccAddr;
                if (IsBadReadPtr(cc, 24)) continue;
                const char* name = *(const char* const*)(cc + 8);
                if (name && !IsBadStringPtrA(name, 40) && name[0] == 'C') return name;
            }
        }
        return nullptr;
    }

    bool WeaponOf(void* local, WpnClass& cls, float& interval) {
        cls = W_NONE; interval = 0.2f;
        const unsigned h = Rd<unsigned>(local, OFF_ACTWEAPON);
        if (h == 0xFFFFFFFFu) return false;
        void* w = GetEnt((int)(h & 0xFFF));
        const char* n = ClassNameOf(w);
        if (!n) { strcpy_s(g_wpnName, "?"); return false; }
        strncpy_s(g_wpnName, n, _TRUNCATE);
        for (const auto& wi : kWeapons) if (strcmp(n, wi.name) == 0) { cls = wi.c; interval = wi.interval; return true; }
        return true; // known entity, not a gun we handle (knife/grenade/c4)
    }

    // ---------------- ESP snapshot ----------------
    struct EspSnap {
        float eye[3]; float ang[3]; float fov; DWORD t; int n;
        float feet[32][3]; unsigned char duck[32];
    };
    EspSnap g_espBuf = {};
    volatile LONG g_espSeq = 0;

    bool ReadEsp(EspSnap& out) {
        for (int t = 0; t < 4; ++t) {
            const LONG a = g_espSeq;
            if (a & 1) continue;
            out = g_espBuf;
            MemoryBarrier();
            if (a == g_espSeq) return true;
        }
        return false;
    }

    bool WorldToScreen(const EspSnap& s, const float* p, float W, float H, float& x, float& y) {
        const float d2r = kPi / 180.0f;
        const float sp = sinf(s.ang[0] * d2r), cp = cosf(s.ang[0] * d2r);
        const float sy = sinf(s.ang[1] * d2r), cy = cosf(s.ang[1] * d2r);
        const float fwd[3] = { cp * cy, cp * sy, -sp };
        const float right[3] = { sy, -cy, 0.0f };
        const float up[3] = { sp * cy, sp * sy, cp };
        const float d[3] = { p[0] - s.eye[0], p[1] - s.eye[1], p[2] - s.eye[2] };
        const float fz = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        if (fz < 1.0f) return false;
        const float tanH = tanf(s.fov * 0.5f * d2r);
        float tanV, tanX;
        if (g_opt.wideFov) { tanV = tanH * 0.75f; tanX = tanV * W / H; }   // Hor+ (4:3 baseline)
        else { tanX = tanH; tanV = tanH * H / W; }                          // fixed horizontal FOV
        const float rx = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
        const float uy = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
        x = W * 0.5f + (rx / fz) / tanX * W * 0.5f;
        y = H * 0.5f - (uy / fz) / tanV * H * 0.5f;
        return true;
    }

    // ---------------- per-frame logic (runs inside CInput::CreateMove, game thread) ----------------
    struct Target { int idx; float feet[3]; bool duck; };

    void CheckBotsOnly(int local, int maxc, bool ingame) {
        bool humans = false; char who[48] = "";
        if (!ingame) { InterlockedExchange(&g_botsOnly, 0); strcpy_s(g_lockWho, "not in game"); return; }
        for (int i = 1; i <= maxc && i <= 64; ++i) {
            if (i == local) continue;
            char pi[400]; memset(pi, 0, sizeof(pi));
            const bool ok = ((PInfoFn)EVT()[E_GETPLAYERINFO])(g_engine, i, pi);
            if (ok && pi[0] && !pi[kPlayerInfoFakeOffset]) { humans = true; strncpy_s(who, pi, 32); break; }
        }
        if (humans) { strcpy_s(g_lockWho, who); InterlockedExchange(&g_botsOnly, 0); }
        else { g_lockWho[0] = 0; InterlockedExchange(&g_botsOnly, 1); }
    }

    void OnCreateMove(UserCmd* cmd) {
        static unsigned frame = 0; ++frame;
        static bool frozen = false; static float frozenAng[3];

        const int local = ((IntFn)EVT()[E_GETLOCAL])(g_engine);
        const int maxc = ((IntFn)EVT()[E_GETMAXCLIENTS])(g_engine);
        const bool ingame = ((BoolFn)EVT()[E_ISINGAME])(g_engine);
        if (frame % 20 == 1) CheckBotsOnly(local, maxc, ingame);

        // menu open: freeze camera / movement / buttons
        if (InterlockedCompareExchange(&g_menuOpen, 0, 0)) {
            if (!frozen) { ((VecFn)EVT()[E_GETVIEWANG])(g_engine, frozenAng); frozen = true; }
            ((VecFn)EVT()[E_SETVIEWANG])(g_engine, frozenAng);
            cmd->viewangles[0] = frozenAng[0]; cmd->viewangles[1] = frozenAng[1]; cmd->viewangles[2] = frozenAng[2];
            cmd->buttons = 0; cmd->forwardmove = 0; cmd->sidemove = 0; cmd->upmove = 0;
            return;
        }
        frozen = false;

        if (!ingame || !InterlockedCompareExchange(&g_botsOnly, 0, 0)) return;
        if (local < 1 || local > 64) return;
        void* me = GetEnt(local);
        if (!me) return;
        if (Rd<int>(me, OFF_HEALTH) <= 0 || Rd<unsigned char>(me, OFF_LIFE) != 0) return;

        const int myTeam = Rd<int>(me, OFF_TEAM);
        const float eye[3] = { Rd<float>(me, OFF_ORIGIN) + Rd<float>(me, OFF_VIEWOFS),
                               Rd<float>(me, OFF_ORIGIN + 4) + Rd<float>(me, OFF_VIEWOFS + 4),
                               Rd<float>(me, OFF_ORIGIN + 8) + Rd<float>(me, OFF_VIEWOFS + 8) };
        const float punch[3] = { Rd<float>(me, OFF_PUNCH), Rd<float>(me, OFF_PUNCH + 4), Rd<float>(me, OFF_PUNCH + 8) };
        const float vel[2] = { Rd<float>(me, OFF_VEL), Rd<float>(me, OFF_VEL + 4) };
        const int myFlags = Rd<int>(me, OFF_FLAGS);

        // ---- collect enemy targets (and player pointers for the wall-check filter)
        Target targets[32]; int nt = 0;
        g_playerPtrCount = 0;
        static DWORD lastSim[65]; static DWORD lastSimChange[65];
        const DWORD nowMs = GetTickCount();
        for (int i = 1; i <= maxc && i <= 32; ++i) {
            void* e = GetEnt(i);
            if (!e) continue;
            if (g_playerPtrCount < 40) g_playerPtrs[g_playerPtrCount++] = e;
            if (i == local) continue;
            if (Rd<int>(e, OFF_HEALTH) <= 0 || Rd<unsigned char>(e, OFF_LIFE) != 0) continue;
            // stale/dormant filter: simulation time must have changed recently
            const DWORD sim = Rd<DWORD>(e, OFF_SIMTIME);
            if (sim != lastSim[i]) { lastSim[i] = sim; lastSimChange[i] = nowMs; }
            if (nowMs - lastSimChange[i] > 400) continue;
            const int team = Rd<int>(e, OFF_TEAM);
            if (g_opt.teamCheck && (team == myTeam || (team != 2 && team != 3))) continue;
            Target& t = targets[nt++];
            t.idx = i;
            t.feet[0] = Rd<float>(e, OFF_ORIGIN); t.feet[1] = Rd<float>(e, OFF_ORIGIN + 4); t.feet[2] = Rd<float>(e, OFF_ORIGIN + 8);
            t.duck = (Rd<int>(e, OFF_FLAGS) & FL_DUCKING) != 0;
        }

        // view direction (this command) incl. recoil punch
        const float va[3] = { cmd->viewangles[0] + punch[0], cmd->viewangles[1] + punch[1], 0.0f };
        float fwd[3]; AngleForward(va, fwd);

        // ---- ESP publish
        if (g_opt.esp) {
            EspSnap s; memset(&s, 0, sizeof(s));
            for (int i = 0; i < 3; ++i) { s.eye[i] = eye[i]; s.ang[i] = va[i]; }
            int fov = Rd<int>(me, OFF_FOV); const int def = Rd<int>(me, OFF_DEFFOV);
            s.fov = fov > 0 ? (float)fov : (def > 0 ? (float)def : 90.0f);
            s.t = nowMs;
            for (int k = 0; k < nt && s.n < 32; ++k) {
                for (int c = 0; c < 3; ++c) s.feet[s.n][c] = targets[k].feet[c];
                s.duck[s.n] = targets[k].duck ? 1 : 0; ++s.n;
            }
            InterlockedIncrement(&g_espSeq); g_espBuf = s; InterlockedIncrement(&g_espSeq);
        }

        // ---- weapon
        WpnClass wc; float interval;
        const bool haveW = WeaponOf(me, wc, interval);
        if (!haveW || wc == W_NONE) return;

        const float speed = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]);
        int fovNow = Rd<int>(me, OFF_FOV); int defFov = Rd<int>(me, OFF_DEFFOV); if (defFov <= 0) defFov = 90;
        const bool scoped = fovNow > 0 && fovNow < defFov - 1;

        // ---- triggerbot
        if (InterlockedCompareExchange(&g_enabled, 0, 0) && nt > 0) {
            do {
                const LONGLONG now = Now();
                if (now < g_nextShot) break;
                if (g_opt.ground && !(myFlags & FL_ONGROUND)) break;
                if (g_opt.speedCheck && speed > MaxSpeedFor(wc)) break;
                float bestT = 1e9f; int best = -1;
                for (int k = 0; k < nt; ++k) {
                    const float h = targets[k].duck ? 54.0f : 72.0f;
                    float tmin = -1.0f;
                    float tc = RayCylinder(eye, fwd, targets[k].feet[0], targets[k].feet[1], targets[k].feet[2], targets[k].feet[2] + h - 10.0f, 11.0f);
                    const float hc[3] = { targets[k].feet[0], targets[k].feet[1], targets[k].feet[2] + h - 6.0f };
                    float ts = RaySphere(eye, fwd, hc, 7.5f);
                    if (tc >= 0.0f) tmin = tc;
                    if (ts >= 0.0f && (tmin < 0.0f || ts < tmin)) tmin = ts;
                    if (tmin >= 0.0f && tmin < bestT) { bestT = tmin; best = k; }
                }
                if (best < 0) break;
                const float hitp[3] = { eye[0] + fwd[0] * bestT, eye[1] + fwd[1] * bestT, eye[2] + fwd[2] * bestT };
                if (!ClearLine(eye, hitp)) break;
                if (g_opt.scopeCheck && wc == W_SNIPER) {
                    if (!scoped && (bestT > 128.0f || speed > 5.0f)) break;
                }
                cmd->buttons |= IN_ATTACK;
                g_nextShot = now + (LONGLONG)(interval * (float)g_opt.intervalPct * 0.01f * (float)g_qpcFreq);
            } while (0);
        }

        // ---- FOV aim assist (silent): only while firing
        if (g_opt.aimOn && (cmd->buttons & IN_ATTACK) && nt > 0) {
            float bestAng = (float)g_opt.aimFov; float bp[3]; bool found = false;
            for (int k = 0; k < nt; ++k) {
                const bool d = targets[k].duck;
                const float headZ = d ? 49.0f : 67.0f, chestZ = d ? 32.0f : 46.0f;
                const float zs[2] = { g_opt.aimHead ? headZ : chestZ, g_opt.aimHead ? chestZ : headZ };
                for (int j = 0; j < 2; ++j) {
                    const float pt[3] = { targets[k].feet[0], targets[k].feet[1], targets[k].feet[2] + zs[j] };
                    float dv[3] = { pt[0] - eye[0], pt[1] - eye[1], pt[2] - eye[2] };
                    const float len = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
                    if (len < 1.0f) continue;
                    float dot = (dv[0] * fwd[0] + dv[1] * fwd[1] + dv[2] * fwd[2]) / len;
                    if (dot > 1.0f) dot = 1.0f; if (dot < -1.0f) dot = -1.0f;
                    const float ang = acosf(dot) * 180.0f / kPi;
                    if (ang >= bestAng) break;
                    if (!ClearLine(eye, pt)) continue;
                    bestAng = ang; bp[0] = pt[0]; bp[1] = pt[1]; bp[2] = pt[2]; found = true; break;
                }
            }
            if (found) {
                const float dx = bp[0] - eye[0], dy = bp[1] - eye[1], dz = bp[2] - eye[2];
                float pitch = -atan2f(dz, sqrtf(dx * dx + dy * dy)) * 180.0f / kPi - punch[0];
                float yaw = atan2f(dy, dx) * 180.0f / kPi - punch[1];
                while (yaw > 180.0f) yaw -= 360.0f; while (yaw < -180.0f) yaw += 360.0f;
                if (pitch > 89.0f) pitch = 89.0f; if (pitch < -89.0f) pitch = -89.0f;
                const float dd = (yaw - cmd->viewangles[1]) * kPi / 180.0f;
                const float f = cmd->forwardmove, sd = cmd->sidemove;
                cmd->forwardmove = f * cosf(dd) - sd * sinf(dd);
                cmd->sidemove = f * sinf(dd) + sd * cosf(dd);
                cmd->viewangles[0] = pitch; cmd->viewangles[1] = yaw;
            }
        }
    }

    // ---------------- hook ----------------
    typedef void (__thiscall* CreateMoveFn)(void*, int, float, bool);
    CreateMoveFn g_origCreateMove = nullptr;

    void __fastcall HookCreateMove(void* self, void* edx, int seq, float frametime, bool active) {
        InterlockedIncrement(&g_activeHooks);
        g_origCreateMove(self, seq, frametime, active);
        if (active && InterlockedCompareExchange(&g_ifaceReady, 0, 0)) {
            unsigned char* arr = *(unsigned char**)((unsigned char*)self + kCmdArrayOffset);
            if (arr && !IsBadReadPtr(arr, kCmdSize * kMultiplayerBackup)) {
                UserCmd* cmd = (UserCmd*)(arr + kCmdSize * (seq % kMultiplayerBackup));
                if (cmd->command_number == seq) {
                    __try { OnCreateMove(cmd); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                }
            }
        }
        InterlockedDecrement(&g_activeHooks);
    }

    bool InitInterfaces() {
        HMODULE cl = GetModuleHandleA("client.dll"), en = GetModuleHandleA("engine.dll");
        if (!cl || !en) return false;
        CreateInterfaceFn ciC = (CreateInterfaceFn)GetProcAddress(cl, "CreateInterface");
        CreateInterfaceFn ciE = (CreateInterfaceFn)GetProcAddress(en, "CreateInterface");
        if (!ciC || !ciE) return false;
        g_engine = (void**)ciE("VEngineClient012", nullptr);
        g_trace = (void**)ciE("EngineTraceClient003", nullptr);
        g_entlist = (void**)ciC("VClientEntityList003", nullptr);
        return g_engine && g_trace && g_entlist;
    }

    bool InstallHook() {
        HMODULE cl = GetModuleHandleA("client.dll");
        if (!cl) return false;
        auto dos = (IMAGE_DOS_HEADER*)cl; auto nt = (IMAGE_NT_HEADERS*)((unsigned char*)cl + dos->e_lfanew);
        if (nt->OptionalHeader.SizeOfImage != kClientImageSize) return false;           // different build
        unsigned char* fn = (unsigned char*)cl + kCreateMoveRva;
        if (memcmp(fn, kCreateMoveSig, sizeof(kCreateMoveSig)) != 0) return false;      // different code
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return false;
        if (MH_CreateHook(fn, (void*)&HookCreateMove, (void**)&g_origCreateMove) != MH_OK) return false;
        if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) return false;
        InterlockedExchange(&g_hooksReady, 1);
        return true;
    }

    void UninstallHook() {
        if (InterlockedCompareExchange(&g_hooksReady, 0, 0)) {
            MH_DisableHook(MH_ALL_HOOKS);
            for (int i = 0; i < 300 && InterlockedCompareExchange(&g_activeHooks, 0, 0) != 0; ++i) Sleep(1);
            Sleep(30);
            MH_Uninitialize();
            InterlockedExchange(&g_hooksReady, 0);
        }
    }

    bool GameForeground() {
        HWND fg = GetForegroundWindow(); if (!fg) return false;
        DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }

    // ============================ overlay + menu ============================
    float g_vx = 0, g_vy = 0;
    bool g_lmbHeld = false;
    volatile LONG g_clickPending = 0;
    bool g_prevMenuOpen = false;

    COLORREF kBg = RGB(22, 24, 30), kPanel = RGB(32, 35, 44), kAccent = RGB(0, 220, 90),
             kText = RGB(225, 228, 235), kDim = RGB(130, 136, 150), kOff = RGB(58, 62, 74);

    struct Ui { HDC dc; bool click; bool held; int cx, cy; };
    bool Hit(const Ui& u, int x, int y, int w, int h) { return u.cx >= x && u.cx < x + w && u.cy >= y && u.cy < y + h; }
    void Text(HDC dc, int x, int y, const char* t, COLORREF c) { SetTextColor(dc, c); TextOutA(dc, x, y, t, (int)strlen(t)); }

    void Checkbox(Ui& u, int x, int y, int w, const char* label, volatile LONG* v) {
        const bool hot = Hit(u, x, y, w, 24);
        if (hot && u.click) InterlockedExchange(v, *v ? 0 : 1);
        RECT r = { x, y, x + w, y + 24 };
        HBRUSH hb = CreateSolidBrush(hot ? RGB(42, 46, 58) : kPanel); FillRect(u.dc, &r, hb); DeleteObject(hb);
        RECT box = { x + 6, y + 4, x + 22, y + 20 };
        HBRUSH bb = CreateSolidBrush(*v ? kAccent : kOff); FillRect(u.dc, &box, bb); DeleteObject(bb);
        if (*v) {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(15, 20, 15)); HGDIOBJ op = SelectObject(u.dc, pen);
            MoveToEx(u.dc, x + 9, y + 12, nullptr); LineTo(u.dc, x + 13, y + 16); LineTo(u.dc, x + 19, y + 7);
            SelectObject(u.dc, op); DeleteObject(pen);
        }
        Text(u.dc, x + 32, y + 4, label, kText);
    }

    void Slider(Ui& u, int x, int y, int w, const char* label, volatile LONG* v, int lo, int hi, const char* suffix = "%") {
        static bool drag = false;
        char buf[96]; sprintf_s(buf, "%s: %ld%s", label, (long)*v, suffix);
        Text(u.dc, x + 6, y, buf, kText);
        const int tx = x + 6, ty = y + 26, tw = w - 12;
        if (u.click && Hit(u, tx - 4, ty - 8, tw + 8, 22)) drag = true;
        if (!u.held) drag = false;
        if (drag) {
            float f = (float)(u.cx - tx) / (float)tw; if (f < 0) f = 0; if (f > 1) f = 1;
            InterlockedExchange(v, lo + (LONG)(f * (hi - lo) + 0.5f));
        }
        RECT tr = { tx, ty, tx + tw, ty + 4 };
        HBRUSH t = CreateSolidBrush(kOff); FillRect(u.dc, &tr, t); DeleteObject(t);
        const int px = tx + (int)((float)(*v - lo) / (float)(hi - lo) * tw);
        RECT fr = { tx, ty, px, ty + 4 };
        HBRUSH f = CreateSolidBrush(kAccent); FillRect(u.dc, &fr, f); DeleteObject(f);
        RECT kn = { px - 5, ty - 6, px + 5, ty + 10 };
        HBRUSH k = CreateSolidBrush(kText); FillRect(u.dc, &kn, k); DeleteObject(k);
    }

    void DrawMenu(Ui& u, const RECT& rc) {
        const int W = 380, H = 380;
        const int x0 = (rc.right - W) / 2, y0 = (rc.bottom - H) / 2;
        RECT panel = { x0, y0, x0 + W, y0 + H };
        HBRUSH pb = CreateSolidBrush(kBg); FillRect(u.dc, &panel, pb); DeleteObject(pb);
        HBRUSH bd = CreateSolidBrush(kAccent); FrameRect(u.dc, &panel, bd); DeleteObject(bd);
        RECT title = { x0, y0, x0 + W, y0 + 34 };
        HBRUSH tb = CreateSolidBrush(kPanel); FillRect(u.dc, &title, tb); DeleteObject(tb);
        Text(u.dc, x0 + 14, y0 + 8, "CS:S v34  |  Menu", kAccent);

        static int tab = 0;
        const char* names[3] = { "Triggerbot", "Visuals", "Aim" };
        for (int t = 0; t < 3; ++t) {
            const int tx = x0 + 10 + t * 112;
            if (u.click && Hit(u, tx, y0 + 44, 106, 26)) tab = t;
            RECT tr = { tx, y0 + 44, tx + 106, y0 + 70 };
            HBRUSH tbr = CreateSolidBrush(tab == t ? kAccent : kPanel); FillRect(u.dc, &tr, tbr); DeleteObject(tbr);
            Text(u.dc, tx + 12, y0 + 49, names[t], tab == t ? RGB(15, 20, 15) : kText);
        }

        const int x = x0 + 12, w = W - 24; int y = y0 + 84;
        if (tab == 0) {
            Checkbox(u, x, y, w, "Enabled", &g_enabled);                                   y += 30;
            Text(u.dc, x + 6, y, "Conditions", kDim);                                      y += 22;
            Checkbox(u, x, y, w, "Team check (don't shoot teammates)", &g_opt.teamCheck);  y += 28;
            Checkbox(u, x, y, w, "Only on ground", &g_opt.ground);                         y += 28;
            Checkbox(u, x, y, w, "Speed limit (accuracy)", &g_opt.speedCheck);             y += 28;
            Checkbox(u, x, y, w, "Snipers: scope required", &g_opt.scopeCheck);            y += 34;
            Text(u.dc, x + 6, y, "Tuning", kDim);                                          y += 22;
            Slider(u, x, y, w, "Shot interval (lower = faster)", &g_opt.intervalPct, 50, 200);
        } else if (tab == 2) {
            Checkbox(u, x, y, w, "FOV aim assist", &g_opt.aimOn);                          y += 30;
            Checkbox(u, x, y, w, "Aim at head (off = chest)", &g_opt.aimHead);             y += 34;
            Slider(u, x, y, w, "FOV radius", &g_opt.aimFov, 1, 40, " deg");                y += 50;
            Text(u.dc, x + 6, y, "Works while you fire, enemy team only,", kDim);         y += 20;
            Text(u.dc, x + 6, y, "target must be visible. Bots-only game.", kDim);
        } else {
            Checkbox(u, x, y, w, "Enemy ESP (green boxes, through walls)", &g_opt.esp);   y += 30;
            Checkbox(u, x, y, w, "Widescreen FOV (Hor+) - flip if boxes drift", &g_opt.wideFov); y += 34;
            Text(u.dc, x + 6, y, "Draws enemy-team players only.", kDim);                 y += 20;
            Text(u.dc, x + 6, y, "Use windowed mode so the overlay is visible.", kDim);
        }
        Text(u.dc, x0 + 14, y0 + H - 26, "RShift - close menu   |   END - unload", kDim);
    }

    void DrawCursor(HDC dc, int x, int y) {
        POINT p[3] = { { x, y }, { x, y + 17 }, { x + 12, y + 12 } };
        HBRUSH b = CreateSolidBrush(RGB(255, 255, 255)); HGDIOBJ ob = SelectObject(dc, b);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(10, 10, 10)); HGDIOBJ op = SelectObject(dc, pen);
        Polygon(dc, p, 3);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(pen);
    }

    void PaintOverlay(HWND hwnd) {
        PAINTSTRUCT ps; HDC wdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(dc, bmp);
        HBRUSH black = CreateSolidBrush(RGB(0, 0, 0)); FillRect(dc, &rc, black); DeleteObject(black);
        SetBkMode(dc, TRANSPARENT);

        const bool locked = !InterlockedCompareExchange(&g_botsOnly, 0, 0);
        if (g_opt.esp && !locked) {
            EspSnap es;
            if (ReadEsp(es) && GetTickCount() - es.t < 200) {
                HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 255, 70));
                HGDIOBJ op = SelectObject(dc, pen);
                HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
                for (int i = 0; i < es.n; ++i) {
                    const float h = es.duck[i] ? 54.0f : 72.0f;
                    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f; bool ok = true;
                    for (int c = 0; c < 8 && ok; ++c) {
                        const float q[3] = { es.feet[i][0] + ((c & 1) ? 16.0f : -16.0f),
                                             es.feet[i][1] + ((c & 2) ? 16.0f : -16.0f),
                                             es.feet[i][2] + ((c & 4) ? h : 0.0f) };
                        float x, y;
                        if (!WorldToScreen(es, q, (float)rc.right, (float)rc.bottom, x, y)) { ok = false; break; }
                        if (x < minx) minx = x; if (x > maxx) maxx = x;
                        if (y < miny) miny = y; if (y > maxy) maxy = y;
                    }
                    if (!ok || maxx - minx > rc.right * 2 || maxy - miny > rc.bottom * 2) continue;
                    Rectangle(dc, (int)minx, (int)miny, (int)maxx, (int)maxy);
                }
                SelectObject(dc, obr); SelectObject(dc, op); DeleteObject(pen);
            }
        }

        const bool open = InterlockedCompareExchange(&g_menuOpen, 0, 0) != 0;
        const bool on = InterlockedCompareExchange(&g_enabled, 0, 0) != 0;
        if (open) {
            HFONT font = CreateFontA(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            HGDIOBJ of = SelectObject(dc, font);
            if (!g_prevMenuOpen) { g_vx = rc.right / 2.0f; g_vy = rc.bottom / 2.0f; g_clickPending = 0; }
            Ui u = { dc, InterlockedExchange(&g_clickPending, 0) != 0, g_lmbHeld, (int)g_vx, (int)g_vy };
            DrawMenu(u, rc);
            DrawCursor(dc, u.cx, u.cy);
            SelectObject(dc, of); DeleteObject(font);
        }
        g_prevMenuOpen = open;

        HFONT hf = CreateFontA(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
        HGDIOBJ oh = SelectObject(dc, hf);
        SetTextColor(dc, RGB(0, 255, 70));
        RECT r1 = { rc.right - 300, 4, rc.right - 8, 28 };
        DrawTextA(dc, on ? "Trigger - on" : "Trigger - off", -1, &r1, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        const LONG st = InterlockedCompareExchange(&g_status, 0, 0);
        const char* line = nullptr; char buf[96];
        if (st == 1) line = "interfaces not found";
        else if (st == 2) line = "unsupported client.dll / hook failed";
        else if (locked) {
            if (g_lockWho[0] && strcmp(g_lockWho, "not in game") != 0) { sprintf_s(buf, "online - disabled (%s)", g_lockWho); line = buf; }
            else line = g_lockWho[0] ? "waiting: not in game" : "waiting...";
        }
        if (line) {
            SetTextColor(dc, RGB(255, 80, 80));
            RECT r2 = { rc.right - 300, 28, rc.right - 8, 50 };
            DrawTextA(dc, line, -1, &r2, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        }
        if ((g_enabled || g_opt.aimOn) && !locked) {
            char wb[64]; sprintf_s(wb, "wpn: %s", g_wpnName);
            SetTextColor(dc, RGB(120, 200, 140));
            RECT r4 = { rc.right - 300, 50, rc.right - 8, 72 };
            DrawTextA(dc, wb, -1, &r4, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(dc, oh); DeleteObject(hf);

        BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
        EndPaint(hwnd, &ps);
    }

    LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_TIMER: InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_INPUT: {
            if (!InterlockedCompareExchange(&g_menuOpen, 0, 0)) return 0;
            RAWINPUT ri; UINT sz = sizeof(ri);
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) == (UINT)-1) return 0;
            if (ri.header.dwType != RIM_TYPEMOUSE) return 0;
            const RAWMOUSE& m = ri.data.mouse;
            if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) { g_vx += (float)m.lLastX; g_vy += (float)m.lLastY; }
            RECT rc; GetClientRect(hwnd, &rc);
            if (g_vx < 0) g_vx = 0; if (g_vy < 0) g_vy = 0;
            if (g_vx > rc.right - 1) g_vx = (float)(rc.right - 1);
            if (g_vy > rc.bottom - 1) g_vy = (float)(rc.bottom - 1);
            if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) { g_lmbHeld = true; InterlockedExchange(&g_clickPending, 1); }
            if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) g_lmbHeld = false;
            return 0;
        }
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: PaintOverlay(hwnd); return 0;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: KillTimer(hwnd, 1); PostQuitMessage(0); return 0;
        }
        return DefWindowProcA(hwnd, msg, wp, lp);
    }

    DWORD WINAPI OverlayThread(void*) {
        const char* cls = "CSSHelperOverlayV1";
        WNDCLASSEXA wc = {}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = OverlayProc;
        wc.hInstance = g_self; wc.lpszClassName = cls; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;
        HWND h = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                 cls, "status", WS_POPUP, 0, 0, 300, 80, nullptr, nullptr, g_self, nullptr);
        if (!h) return 1;
        g_overlay = h;
        SetLayeredWindowAttributes(h, RGB(0, 0, 0), 0, LWA_COLORKEY);
        RAWINPUTDEVICE rid = { 0x01, 0x02, RIDEV_INPUTSINK, h };
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        SetTimer(h, 1, 10, nullptr);
        ShowWindow(h, SW_SHOWNOACTIVATE);
        MSG m;
        while (GetMessageA(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageA(&m); }
        RAWINPUTDEVICE off = { 0x01, 0x02, RIDEV_REMOVE, nullptr };
        RegisterRawInputDevices(&off, 1, sizeof(off));
        g_overlay = nullptr;
        UnregisterClassA(cls, g_self);
        return 0;
    }

    void PlaceOverlay(bool fg) {
        HWND o = g_overlay;
        if (!o || !IsWindow(o)) return;
        if (!fg) { ShowWindow(o, SW_HIDE); return; }
        HWND game = GetForegroundWindow();
        RECT rc; if (!game || !GetClientRect(game, &rc)) return;
        POINT pt = { rc.left, rc.top }; ClientToScreen(game, &pt);
        const int gw = rc.right - rc.left, gh = rc.bottom - rc.top;
        if (InterlockedCompareExchange(&g_menuOpen, 0, 0) || g_opt.esp)
            SetWindowPos(o, HWND_TOPMOST, pt.x, pt.y, gw, gh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        else
            SetWindowPos(o, HWND_TOPMOST, pt.x + gw - 300, pt.y, 300, 80, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    // ============================ worker ============================
    DWORD WINAPI Worker(void*) {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcFreq = f.QuadPart;
        g_overlayThread = CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr);

        bool ok = false;
        for (int i = 0; i < 50 && !ok; ++i) { ok = InitInterfaces(); if (!ok) Sleep(200); }
        if (!ok) InterlockedExchange(&g_status, 1);
        else {
            if (!InstallHook()) InterlockedExchange(&g_status, 2);
            else InterlockedExchange(&g_ifaceReady, 1);
        }

        bool prevToggle = false, prevEnd = false; DWORD lastOv = 0;
        while (!InterlockedCompareExchange(&g_unloading, 0, 0)) {
            const DWORD now = GetTickCount();
            const bool fg = GameForeground();
            const bool tog = fg && (GetAsyncKeyState(VK_RSHIFT) & 0x8000);
            if (tog && !prevToggle) {
                InterlockedExchange(&g_menuOpen, InterlockedCompareExchange(&g_menuOpen, 0, 0) ? 0 : 1);
                PlaceOverlay(fg);
            }
            prevToggle = tog;
            const bool end = fg && (GetAsyncKeyState(VK_END) & 0x8000);
            if (end && !prevEnd) { InterlockedExchange(&g_unloading, 1); break; }
            prevEnd = end;
            if (now - lastOv >= 100) { PlaceOverlay(fg); lastOv = now; }
            Sleep(5);
        }

        InterlockedExchange(&g_enabled, 0);
        InterlockedExchange(&g_menuOpen, 0);
        InterlockedExchange(&g_ifaceReady, 0);
        UninstallHook();
        if (g_overlay) PostMessageA(g_overlay, WM_CLOSE, 0, 0);
        if (g_overlayThread) { WaitForSingleObject(g_overlayThread, 1500); CloseHandle(g_overlayThread); }
        FreeLibraryAndExitThread(g_self, 0);
        return 0;
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
