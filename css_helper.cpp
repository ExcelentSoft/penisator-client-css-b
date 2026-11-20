// [patch:006_srvaim] applied
// [patch:005_dmg] applied
// [patch:004_srvhit] applied
// [patch:003_head] applied
// [patch:002_rage] applied
// [patch:001_aim_ru] applied
// CS:Source v34 (engine build 4044, client.dll 4689920 bytes) helper — OFFLINE / BOTS ONLY.
// Menu (RShift): triggerbot, enemy ESP, FOV aim assist. Locks itself if any real (non-bot) player is present.
// All offsets/indices below come from the probe logs of this exact build.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <d3d9.h>
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
    constexpr int IN_ATTACK = 1, IN_ATTACK2 = 1 << 11, IN_JUMP = 1 << 1;
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
        volatile LONG ground = 0;
        volatile LONG speedCheck = 0;
        volatile LONG scopeCheck = 1;
        volatile LONG intervalPct = 100;
        volatile LONG esp = 0;
        volatile LONG wideFov = 1;
        volatile LONG aimOn = 0;
        volatile LONG aimHead = 1;
        volatile LONG aimFov = 10;
        volatile LONG espBox = 1, espHp = 1, espName = 1, espDist = 1, espAimed = 1, espColor = 0;
        volatile LONG aimSmooth = 0, aimSpeed = 30, aimPrio = 0;
        volatile LONG bhop = 0, autoStrafe = 0;
        volatile LONG noSpread = 1;
        volatile LONG rage = 0, recoilPct = 200;
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
    LONGLONG g_qpcFreq = 0, g_nextShot = 0, g_nextScope = 0;

    // ---------------- config (ini next to the dll) ----------------
    struct CfgEntry { const char* key; volatile LONG* v; LONG def, lo, hi; };
    CfgEntry g_cfg[] = {
        { "trigger", &g_enabled, 0, 0, 1 }, { "teamCheck", &g_opt.teamCheck, 1, 0, 1 }, { "ground", &g_opt.ground, 0, 0, 1 },
        { "standStill", &g_opt.speedCheck, 0, 0, 1 }, { "autoScope", &g_opt.scopeCheck, 1, 0, 1 },
        { "shotInterval", &g_opt.intervalPct, 100, 50, 200 },
        { "esp", &g_opt.esp, 0, 0, 1 }, { "espBox", &g_opt.espBox, 1, 0, 1 }, { "espHp", &g_opt.espHp, 1, 0, 1 },
        { "espName", &g_opt.espName, 1, 0, 1 }, { "espDist", &g_opt.espDist, 1, 0, 1 }, { "espAimed", &g_opt.espAimed, 1, 0, 1 },
        { "espColor", &g_opt.espColor, 0, 0, 5 }, { "wideFov", &g_opt.wideFov, 1, 0, 1 },
        { "aim", &g_opt.aimOn, 0, 0, 1 }, { "aimHead", &g_opt.aimHead, 1, 0, 1 }, { "aimFov", &g_opt.aimFov, 10, 1, 180 },
        { "aimSmooth", &g_opt.aimSmooth, 0, 0, 1 }, { "aimSpeed", &g_opt.aimSpeed, 30, 1, 100 }, { "aimPrio", &g_opt.aimPrio, 0, 0, 2 },
        { "bhop", &g_opt.bhop, 0, 0, 1 }, { "autoStrafe", &g_opt.autoStrafe, 0, 0, 1 },
        { "noSpread", &g_opt.noSpread, 1, 0, 1 },
        { "rage", &g_opt.rage, 0, 0, 1 }, { "recoilPct", &g_opt.recoilPct, 200, 0, 300 },
    };
    bool CfgPath(char* out, size_t n) {
        if (!GetModuleFileNameA(g_self, out, (DWORD)n)) return false;
        char* sl = strrchr(out, '\\'); if (!sl) return false;
        strcpy_s(sl + 1, n - (size_t)(sl + 1 - out), "css_helper.ini");
        return true;
    }
    void CfgSave() {
        char path[MAX_PATH]; if (!CfgPath(path, MAX_PATH)) return;
        for (auto& e : g_cfg) { char b[16]; sprintf_s(b, "%ld", (long)*e.v); WritePrivateProfileStringA("css", e.key, b, path); }
    }
    void CfgLoad() {
        char path[MAX_PATH]; if (!CfgPath(path, MAX_PATH)) return;
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
        for (auto& e : g_cfg) {
            LONG v = (LONG)GetPrivateProfileIntA("css", e.key, e.def, path);
            if (v < e.lo) v = e.lo; if (v > e.hi) v = e.hi;
            InterlockedExchange(e.v, v);
        }
    }
    void CfgReset() { for (auto& e : g_cfg) InterlockedExchange(e.v, e.def); }
    char g_names[65][32] = {};

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
        { "CWeaponAWP", W_SNIPER, 1.47f }, { "CWeaponScout", W_SNIPER, 1.27f },
        { "CWeaponG3SG1", W_SNIPER, 0.26f }, { "CWeaponSG550", W_SNIPER, 0.26f },
        { "CDEagle", W_PISTOL, 0.25f }, { "CWeaponUSP", W_PISTOL, 0.16f }, { "CWeaponGlock", W_PISTOL, 0.16f },
        { "CWeaponP228", W_PISTOL, 0.16f }, { "CWeaponFiveSeven", W_PISTOL, 0.16f }, { "CWeaponElite", W_PISTOL, 0.14f },
        { "CWeaponM3", W_SHOTGUN, 0.90f }, { "CWeaponXM1014", W_SHOTGUN, 0.26f },
        { "CAK47", W_RIFLE, 0.105f }, { "CWeaponM4A1", W_RIFLE, 0.095f }, { "CWeaponGalil", W_RIFLE, 0.095f },
        { "CWeaponFamas", W_RIFLE, 0.095f }, { "CWeaponAug", W_RIFLE, 0.095f }, { "CWeaponSG552", W_RIFLE, 0.095f },
        { "CWeaponMP5Navy", W_SMG, 0.08f }, { "CWeaponUMP45", W_SMG, 0.105f }, { "CWeaponP90", W_SMG, 0.075f },
        { "CWeaponMAC10", W_SMG, 0.08f }, { "CWeaponTMP", W_SMG, 0.075f }, { "CWeaponM249", W_SMG, 0.085f },
    };
    char g_wpnName[40] = "?";
    // rage: shot target handed to the server-side hook
    DWORD g_rTick = 0; volatile LONG g_rValid = 0; float g_rEye[3] = { 0, 0, 0 }, g_rPt[3] = { 0, 0, 0 };
    volatile LONG g_rHead = 0;
    volatile LONG g_fbHooked = 0, g_fbBad = 0, g_fbCalls = 0, g_fbRedirects = 0;
    volatile LONG g_headMode = 0;     // debug: 1 = head found by scan, 2 = fallback guess
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
        float feet[32][3]; unsigned char duck[32]; int idx[32]; int hp[32]; unsigned char aimed[32];
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
    struct Target { int idx; void* ent; float feet[3]; bool duck; int hp; };

    void CheckBotsOnly(int local, int maxc, bool ingame) {
        bool humans = false; char who[48] = "";
        if (!ingame) { InterlockedExchange(&g_botsOnly, 0); strcpy_s(g_lockWho, "not in game"); return; }
        for (int i = 1; i <= maxc && i <= 64; ++i) {
            if (i == local) continue;
            char pi[400]; memset(pi, 0, sizeof(pi));
            const bool ok = ((PInfoFn)EVT()[E_GETPLAYERINFO])(g_engine, i, pi);
            if (i < 65) { if (ok && pi[0]) strncpy_s(g_names[i], pi, 31); else g_names[i][0] = 0; }
            if (ok && pi[0] && !pi[kPlayerInfoFakeOffset]) { humans = true; strncpy_s(who, pi, 32); break; }
        }
        if (humans) { strcpy_s(g_lockWho, who); InterlockedExchange(&g_botsOnly, 0); }
        else { g_lockWho[0] = 0; InterlockedExchange(&g_botsOnly, 1); }
    }

    // ---- engine ray clipped against ONE entity (its real hitboxes in the current client pose)
    typedef void (__thiscall* ClipFn)(void*, const Ray&, unsigned int, void*, void*);
    bool ClipHit(void* ent, const float* eye, const float* pt) {
        Ray r; memset(&r, 0, sizeof(r));
        for (int i = 0; i < 3; ++i) { r.start[i] = eye[i]; r.delta[i] = (pt[i] - eye[i]) * 1.6f; }
        r.isRay = true; r.isSwept = true;
        __declspec(align(16)) unsigned int buf[64]; memset(buf, 0, sizeof(buf));
        const float one = 1.0f; memcpy(&buf[11], &one, 4);               // fraction = 1 (no hit) unless the engine overwrites it
        ((ClipFn)TVT()[2])(g_trace, r, 0x4600400B, ent, buf);
        return *(float*)&buf[11] < 0.999f;
    }

    // Head centre from the model itself: scan down for the top of the player, then centre sideways.
    bool HeadLocate(void* ent, const float* eye, const float* feet, bool duck, float* out) {
        // [patch:003_head] silhouette based: works for a turned / tilted head and for a back view
        const float fx = feet[0], fy = feet[1];
        const float zHi = feet[2] + (duck ? 64.0f : 82.0f), zLo = feet[2] + (duck ? 34.0f : 50.0f);
        const float dx = fx - eye[0], dy = fy - eye[1];
        const float dl = sqrtf(dx * dx + dy * dy);
        if (dl < 1.0f) return false;
        const float px = -dy / dl, py = dx / dl;
        // 1) highest visible point of the player over several columns (the head may be offset or tilted)
        float topZ = -1e9f;
        for (int o = -8; o <= 8; o += 2) {
            for (float z = zHi; z >= zLo; z -= 1.0f) {
                if (z <= topZ) break;
                const float pt[3] = { fx + px * o, fy + py * o, z };
                if (ClipHit(ent, eye, pt)) { if (z > topZ) topZ = z; break; }
            }
        }
        if (topZ < -1e8f) return false;
        // 2) centre of mass of the visible silhouette in the top band (= the head)
        float sx = 0, sz = 0; int n = 0;
        for (int o = -10; o <= 10; ++o) {
            for (int k = 0; k <= 8; ++k) {
                const float z = topZ - (float)k;
                const float pt[3] = { fx + px * o, fy + py * o, z };
                if (ClipHit(ent, eye, pt)) { sx += (float)o; sz += z; ++n; }
            }
        }
        if (n < 3) return false;
        const float mo = sx / (float)n;
        out[0] = fx + px * mo; out[1] = fy + py * mo; out[2] = sz / (float)n + 1.0f;
        return true;
    }

    // Valve's uniform random stream (vstdlib), used by the game for bullet spread.
    struct Rng {
        int idum, iy, iv[32];
        void Seed(int s) { idum = s < 0 ? s : -s; iy = 0; }
        int Gen() {
            const int IA = 16807, IM = 2147483647, IQ = 127773, IR = 2836, NTAB = 32, NDIV = 1 + (IM - 1) / NTAB;
            int j, k;
            if (idum <= 0 || !iy) {
                if (-idum < 1) idum = 1; else idum = -idum;
                for (j = NTAB + 7; j >= 0; --j) {
                    k = idum / IQ; idum = IA * (idum - k * IQ) - IR * k; if (idum < 0) idum += IM;
                    if (j < NTAB) iv[j] = idum;
                }
                iy = iv[0];
            }
            k = idum / IQ; idum = IA * (idum - k * IQ) - IR * k; if (idum < 0) idum += IM;
            j = iy / NDIV; iy = iv[j]; iv[j] = idum;
            return iy;
        }
        float Float(float lo, float hi) {
            float fl = (float)((double)Gen() * (1.0 / 2147483647.0));
            if (fl > 0.9999998f) fl = 0.9999998f;
            return fl * (hi - lo) + lo;
        }
    };
    // the seed (0..255) whose first spread sample is closest to the exact centre
    int BestSeed() {
        static int best = -1;
        if (best >= 0) return best;
        float bestMag = 1e9f; best = 0;
        for (int sd = 0; sd < 256; ++sd) {
            Rng r; memset(&r, 0, sizeof(r)); r.Seed(sd);
            const float x = r.Float(-0.5f, 0.5f) + r.Float(-0.5f, 0.5f);
            const float y = r.Float(-0.5f, 0.5f) + r.Float(-0.5f, 0.5f);
            const float m = x * x + y * y;
            if (m < bestMag) { bestMag = m; best = sd; }
        }
        return best;
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

        // ---- bunnyhop / autostrafe
        if (g_opt.bhop && (cmd->buttons & IN_JUMP) && !(myFlags & FL_ONGROUND)) {
            cmd->buttons &= ~IN_JUMP;
            if (g_opt.autoStrafe) {
                if (cmd->mousedx < 0) cmd->sidemove = -400.0f;
                else if (cmd->mousedx > 0) cmd->sidemove = 400.0f;
            }
        }

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
            t.idx = i; t.ent = e;
            t.feet[0] = Rd<float>(e, OFF_ORIGIN); t.feet[1] = Rd<float>(e, OFF_ORIGIN + 4); t.feet[2] = Rd<float>(e, OFF_ORIGIN + 8);
            t.duck = (Rd<int>(e, OFF_FLAGS) & FL_DUCKING) != 0;
            t.hp = Rd<int>(e, OFF_HEALTH);
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
                s.duck[s.n] = targets[k].duck ? 1 : 0; s.idx[s.n] = targets[k].idx; s.hp[s.n] = targets[k].hp;
                {
                    const float hh = targets[k].duck ? 54.0f : 72.0f;
                    const float tc = RayCylinder(eye, fwd, targets[k].feet[0], targets[k].feet[1], targets[k].feet[2], targets[k].feet[2] + hh - 10.0f, 11.0f);
                    const float hc[3] = { targets[k].feet[0], targets[k].feet[1], targets[k].feet[2] + hh - 6.0f };
                    s.aimed[s.n] = (tc >= 0.0f || RaySphere(eye, fwd, hc, 7.5f) >= 0.0f) ? 1 : 0;
                }
                ++s.n;
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
        int trigBest = -1;
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
                if (g_opt.scopeCheck && wc == W_SNIPER && !scoped) {      // auto-scope, then shoot on a later frame
                    if (now > g_nextScope) { cmd->buttons |= IN_ATTACK2; g_nextScope = now + g_qpcFreq; }
                    break;
                }
                cmd->buttons |= IN_ATTACK; trigBest = best;
                g_nextShot = now + (LONGLONG)(interval * (float)g_opt.intervalPct * 0.01f * (float)g_qpcFreq);
            } while (0);
        }

        // ---- FOV aim assist: only while firing
        if (!g_opt.rage && g_opt.aimOn && (cmd->buttons & IN_ATTACK)) {
            const float fovLimit = (float)g_opt.aimFov;
            const int onlyK = trigBest;                // the triggerbot already chose the target under the crosshair
            auto angTo = [&](const float* pt) -> float {
                const float dv[3] = { pt[0] - eye[0], pt[1] - eye[1], pt[2] - eye[2] };
                const float len = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
                if (len < 1.0f) return 999.0f;
                float dot = (dv[0] * fwd[0] + dv[1] * fwd[1] + dv[2] * fwd[2]) / len;
                if (dot > 1.0f) dot = 1.0f; if (dot < -1.0f) dot = -1.0f;
                return acosf(dot) * 180.0f / kPi;
            };
            struct Cand { int k; float score; float gh[3], gc[3]; };
            Cand cands[32]; int nc = 0;
            for (int k = 0; k < nt; ++k) {
                if (onlyK >= 0 && k != onlyK) continue;
                const bool d = targets[k].duck;
                Cand c; c.k = k;
                c.gh[0] = targets[k].feet[0]; c.gh[1] = targets[k].feet[1]; c.gh[2] = targets[k].feet[2] + (d ? 50.0f : 68.0f);
                c.gc[0] = targets[k].feet[0]; c.gc[1] = targets[k].feet[1]; c.gc[2] = targets[k].feet[2] + (d ? 32.0f : 46.0f);
                const float a = fminf(angTo(c.gh), angTo(c.gc));
                if (onlyK < 0 && a >= fovLimit + 4.0f) continue;
                const float dist = sqrtf((c.gc[0] - eye[0]) * (c.gc[0] - eye[0]) + (c.gc[1] - eye[1]) * (c.gc[1] - eye[1]) + (c.gc[2] - eye[2]) * (c.gc[2] - eye[2]));
                c.score = a;                                                        // 0: closest to crosshair
                if (g_opt.aimPrio == 1) c.score = dist;                             // 1: closest by distance
                else if (g_opt.aimPrio == 2) c.score = (float)targets[k].hp * 1000.0f + a;   // 2: lowest health
                cands[nc++] = c;
            }
            for (int i = 1; i < nc; ++i) { Cand t = cands[i]; int j = i - 1; while (j >= 0 && cands[j].score > t.score) { cands[j + 1] = cands[j]; --j; } cands[j + 1] = t; }
            float bp[3] = { 0, 0, 0 }, bestDist = 0.0f; bool found = false;
            for (int ci = 0; ci < nc && ci < 3 && !found; ++ci) {      // refine only the best few candidates (traces are not free)
                const Cand& c = cands[ci];
                for (int j = 0; j < 2 && !found; ++j) {
                    const bool wantHead = ((j == 0) == (g_opt.aimHead != 0));
                    float pt[3];
                    if (wantHead) {
                        pt[0] = c.gh[0]; pt[1] = c.gh[1]; pt[2] = c.gh[2];
                        if (HeadLocate(targets[c.k].ent, eye, targets[c.k].feet, targets[c.k].duck, pt)) InterlockedExchange(&g_headMode, 1);
                        else InterlockedExchange(&g_headMode, 2);
                    } else { pt[0] = c.gc[0]; pt[1] = c.gc[1]; pt[2] = c.gc[2]; }
                    if (onlyK < 0 && angTo(pt) >= fovLimit) continue;
                    if (!ClearLine(eye, pt)) continue;
                    bp[0] = pt[0]; bp[1] = pt[1]; bp[2] = pt[2]; found = true;
                    bestDist = sqrtf((pt[0] - eye[0]) * (pt[0] - eye[0]) + (pt[1] - eye[1]) * (pt[1] - eye[1]) + (pt[2] - eye[2]) * (pt[2] - eye[2]));
                }
            }
            float pitch, yaw;
            if (found) {
                const float dx = bp[0] - eye[0], dy = bp[1] - eye[1], dz = bp[2] - eye[2];
                pitch = -atan2f(dz, sqrtf(dx * dx + dy * dy)) * 180.0f / kPi - punch[0];
                yaw = atan2f(dy, dx) * 180.0f / kPi - punch[1];
            } else {                                   // no target: just cancel recoil
                pitch = cmd->viewangles[0] - punch[0]; yaw = cmd->viewangles[1] - punch[1];
            }
            while (yaw > 180.0f) yaw -= 360.0f; while (yaw < -180.0f) yaw += 360.0f;
            if (pitch > 89.0f) pitch = 89.0f; if (pitch < -89.0f) pitch = -89.0f;
            const float oldYaw = cmd->viewangles[1];
            if (found && g_opt.aimSmooth) {
                // visible smooth aim: move the real view a fraction of the way each tick, shoot only when aligned
                const float k = (float)g_opt.aimSpeed * 0.01f;
                float dp = pitch - cmd->viewangles[0], dyw = yaw - cmd->viewangles[1];
                while (dyw > 180.0f) dyw -= 360.0f; while (dyw < -180.0f) dyw += 360.0f;
                const float err = sqrtf(dp * dp + dyw * dyw);
                float np = cmd->viewangles[0] + dp * k, ny = cmd->viewangles[1] + dyw * k;
                if (err < 0.15f) { np = pitch; ny = yaw; }
                while (ny > 180.0f) ny -= 360.0f; while (ny < -180.0f) ny += 360.0f;
                const float remain = err * (1.0f - k);
                if (remain * kPi / 180.0f * bestDist > 5.0f) cmd->buttons &= ~IN_ATTACK;   // not aligned yet: hold fire
                pitch = np; yaw = ny;
                float nv[3] = { pitch, yaw, 0.0f };
                ((VecFn)EVT()[E_SETVIEWANG])(g_engine, nv);
            }
            const float dd = (yaw - oldYaw) * kPi / 180.0f;
            const float f = cmd->forwardmove, sd = cmd->sidemove;
            cmd->forwardmove = f * cosf(dd) - sd * sinf(dd);
            cmd->sidemove = f * sinf(dd) + sd * cosf(dd);
            cmd->viewangles[0] = pitch; cmd->viewangles[1] = yaw;
        }

        // ---- RAGE (beta): every shot is silently redirected to the best reachable enemy, recoil fully compensated
        if (g_opt.rage && (cmd->buttons & IN_ATTACK)) {
            const float sc = (float)g_opt.recoilPct * 0.01f;
            const float eyeR[3] = { eye[0] + vel[0] * 0.015f, eye[1] + vel[1] * 0.015f, eye[2] };   // eye after this tick's movement
            const float va2[3] = { cmd->viewangles[0] + punch[0] * sc, cmd->viewangles[1] + punch[1] * sc, 0.0f };
            float fw[3]; AngleForward(va2, fw);
            struct RC { int k; float score; };
            RC rc[32]; int nrc = 0;
            for (int k = 0; k < nt; ++k) {
                const float cz = targets[k].feet[2] + (targets[k].duck ? 32.0f : 46.0f);
                const float dv[3] = { targets[k].feet[0] - eyeR[0], targets[k].feet[1] - eyeR[1], cz - eyeR[2] };
                const float len = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
                float dot = len > 1.0f ? (dv[0] * fw[0] + dv[1] * fw[1] + dv[2] * fw[2]) / len : 1.0f;
                if (dot > 1.0f) dot = 1.0f; if (dot < -1.0f) dot = -1.0f;
                const float ang = acosf(dot) * 180.0f / kPi;
                float score = ang;
                if (g_opt.aimPrio == 1) score = len;
                else if (g_opt.aimPrio == 2) score = (float)targets[k].hp * 1000.0f + ang;
                rc[nrc].k = k; rc[nrc].score = score; ++nrc;
            }
            for (int i = 1; i < nrc; ++i) { RC t = rc[i]; int j = i - 1; while (j >= 0 && rc[j].score > t.score) { rc[j + 1] = rc[j]; --j; } rc[j + 1] = t; }
            bool found = false; float bp[3] = { 0, 0, 0 };
            for (int ci = 0; ci < nrc && ci < 4 && !found; ++ci) {
                const Target& tg = targets[rc[ci].k];
                const bool d = tg.duck;
                float hp3[3] = { tg.feet[0], tg.feet[1], tg.feet[2] + (d ? 50.0f : 68.0f) };
                if (HeadLocate(tg.ent, eyeR, tg.feet, d, hp3)) InterlockedExchange(&g_headMode, 1); else InterlockedExchange(&g_headMode, 2);
                const float zs[5] = { d ? 40.0f : 56.0f, d ? 32.0f : 46.0f, d ? 24.0f : 36.0f, d ? 14.0f : 22.0f, 6.0f };
                float pts[6][3]; int np = 0;
                if (g_opt.aimHead) { pts[np][0] = hp3[0]; pts[np][1] = hp3[1]; pts[np][2] = hp3[2]; ++np; }
                for (int z = 0; z < 5; ++z) { pts[np][0] = tg.feet[0]; pts[np][1] = tg.feet[1]; pts[np][2] = tg.feet[2] + zs[z]; ++np; }
                if (!g_opt.aimHead) { pts[np][0] = hp3[0]; pts[np][1] = hp3[1]; pts[np][2] = hp3[2]; ++np; }
                for (int q = 0; q < np && !found; ++q) {
                    if (!ClearLine(eyeR, pts[q])) continue;
                    bp[0] = pts[q][0]; bp[1] = pts[q][1]; bp[2] = pts[q][2]; found = true; g_rHead = (g_opt.aimHead && q == 0) ? 1 : 0;
                }
            }
            if (found) { g_rEye[0] = eye[0]; g_rEye[1] = eye[1]; g_rEye[2] = eye[2]; g_rPt[0] = bp[0]; g_rPt[1] = bp[1]; g_rPt[2] = bp[2]; g_rTick = GetTickCount(); g_rValid = 1; }
            else g_rValid = 0;
            float pitch, yaw;
            if (found) {
                const float dx = bp[0] - eyeR[0], dy = bp[1] - eyeR[1], dz = bp[2] - eyeR[2];
                pitch = -atan2f(dz, sqrtf(dx * dx + dy * dy)) * 180.0f / kPi - punch[0] * sc;
                yaw = atan2f(dy, dx) * 180.0f / kPi - punch[1] * sc;
            } else {
                pitch = cmd->viewangles[0] - punch[0] * sc; yaw = cmd->viewangles[1] - punch[1] * sc;
            }
            while (yaw > 180.0f) yaw -= 360.0f; while (yaw < -180.0f) yaw += 360.0f;
            if (pitch > 89.0f) pitch = 89.0f; if (pitch < -89.0f) pitch = -89.0f;
            if (g_fbHooked && found) { pitch = cmd->viewangles[0]; yaw = cmd->viewangles[1]; }   // server hook does the aiming
            const float dd = (yaw - cmd->viewangles[1]) * kPi / 180.0f;
            const float f = cmd->forwardmove, sd = cmd->sidemove;
            cmd->forwardmove = f * cosf(dd) - sd * sinf(dd);
            cmd->sidemove = f * sinf(dd) + sd * cosf(dd);
            cmd->viewangles[0] = pitch; cmd->viewangles[1] = yaw;
        }

        // ---- no spread: pick the random seed whose bullet spread is ~zero (server uses the seed we send)
        if (g_opt.noSpread && (cmd->buttons & IN_ATTACK) && wc != W_SHOTGUN) cmd->random_seed = BestSeed();
    }

    void LogMsg(const char* fmt, ...);
    // ---------------- server side: CCSPlayer::FireBullet (server.dll, in-process listen server) ----------------
    typedef void (__fastcall* FireBulletFn)(void*, void*, float, float, float, const float*, float, float, int, int, int, float, void*, int, float, float);
    FireBulletFn g_origFireBullet = nullptr;

    void** g_strace = nullptr; void* g_sfSelf = nullptr;
    bool __fastcall SF_ShouldHit(void*, void*, void* ent, int) { return ent != g_sfSelf; }
    void* g_sfVtbl[2] = { (void*)&SF_ShouldHit, (void*)&F_TraceType };

    // server trace a->b (a little beyond b); returns hitgroup of a player hit (1 = head), 0 = nothing
    int STrace(const float* a, const float* b) {
        Ray r; memset(&r, 0, sizeof(r));
        for (int i = 0; i < 3; ++i) { r.start[i] = a[i]; r.delta[i] = (b[i] - a[i]) * 1.5f; }
        r.isRay = true; r.isSwept = true;
        Filter f = { g_sfVtbl };
        __declspec(align(16)) unsigned int buf[64]; memset(buf, 0, sizeof(buf));
        const float one = 1.0f; memcpy(&buf[11], &one, 4);
        ((TraceRayFn)(*(void***)g_strace)[T_TRACERAY])(g_strace, r, 0x4600400B, &f, buf);
        const int hg = (int)buf[17];
        return (buf[19] != 0 && hg > 0 && hg <= 7) ? hg : 0;
    }

    // keep the aim point if the server already sees the wanted part there, otherwise search around it
    bool RefineAim(const float* src, const float* P, bool head, void* self, float* outAng) {
        if (!g_strace) return false;
        g_sfSelf = self;
        float d[3] = { P[0] - src[0], P[1] - src[1], P[2] - src[2] };
        const float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (L < 8.0f) return false;
        const float f[3] = { d[0] / L, d[1] / L, d[2] / L };
        float rx = f[1], ry = -f[0]; const float rl = sqrtf(rx * rx + ry * ry);
        if (rl < 1e-4f) return false;
        rx /= rl; ry /= rl;
        const float ux = ry * f[2], uy = -rx * f[2], uz = rx * f[1] - ry * f[0];
        const int hg0 = STrace(src, P);
        static int logn = 0;
        if (logn < 25) { ++logn; LogMsg("rage: wantHead=%d hitgroupAtPoint=%d dist=%.0f", head ? 1 : 0, hg0, L); }
        if (head ? (hg0 == 1) : (hg0 > 0)) return false;
        float ga[300], gb[300]; int n = 0; float sa = 0, sb = 0;
        for (int b = -18; b <= 18; b += 2)
            for (int a = -18; a <= 18; a += 2) {
                const float pt[3] = { P[0] + rx * a + ux * b, P[1] + ry * a + uy * b, P[2] + uz * b };
                const int hg = STrace(src, pt);
                if (head ? (hg == 1) : (hg > 0)) { if (n < 300) { ga[n] = (float)a; gb[n] = (float)b; ++n; sa += (float)a; sb += (float)b; } }
            }
        if (n < 1) return false;
        float ca = sa / n, cb = sb / n;
        float pt[3] = { P[0] + rx * ca + ux * cb, P[1] + ry * ca + uy * cb, P[2] + uz * cb };
        const int hgc = STrace(src, pt);
        if (!(head ? (hgc == 1) : (hgc > 0))) {                                  // centroid not on target: use the nearest good sample
            float bd = 1e9f; int bi = 0;
            for (int i = 0; i < n; ++i) { const float da = ga[i] - ca, db = gb[i] - cb, dd = da * da + db * db; if (dd < bd) { bd = dd; bi = i; } }
            pt[0] = P[0] + rx * ga[bi] + ux * gb[bi]; pt[1] = P[1] + ry * ga[bi] + uy * gb[bi]; pt[2] = P[2] + uz * gb[bi];
        }
        const float dx = pt[0] - src[0], dy = pt[1] - src[1], dz = pt[2] - src[2];
        outAng[0] = -atan2f(dz, sqrtf(dx * dx + dy * dy)) * 180.0f / kPi;
        outAng[1] = atan2f(dy, dx) * 180.0f / kPi;
        outAng[2] = 0.0f;
        return true;
    }
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
                { float ra[3]; const float src3[3] = { sx, sy, sz };
                  if (RefineAim(src3, g_rPt, g_rHead != 0, self, ra)) { a2[0] = ra[0]; a2[1] = ra[1]; a2[2] = ra[2]; } }
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
        { HMODULE en = GetModuleHandleA("engine.dll");
          CreateInterfaceFn ci = en ? (CreateInterfaceFn)GetProcAddress(en, "CreateInterface") : nullptr;
          g_strace = ci ? (void**)ci("EngineTraceServer003", nullptr) : nullptr;
          LogMsg("server trace interface: %p", (void*)g_strace); }
        if (MH_CreateHook(fn, (void*)&HookFireBullet, (void**)&g_origFireBullet) != MH_OK) { InterlockedExchange(&g_fbBad, 1); return false; }
        if (MH_EnableHook(fn) != MH_OK) { InterlockedExchange(&g_fbBad, 1); return false; }
        InterlockedExchange(&g_fbHooked, 1);
        return true;
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

    // ============================ menu UI ============================
    float g_vx = 0, g_vy = 0;
    bool g_lmbHeld = false;
    volatile LONG g_clickPending = 0;
    bool g_prevMenuOpen = false;

    COLORREF kBg = RGB(22, 24, 30), kPanel = RGB(32, 35, 44), kAccent = RGB(0, 220, 90),
             kText = RGB(225, 228, 235), kDim = RGB(130, 136, 150), kOff = RGB(58, 62, 74);

    struct Ui { HDC dc; bool click; bool held; int cx, cy; };
    bool Hit(const Ui& u, int x, int y, int w, int h) { return u.cx >= x && u.cx < x + w && u.cy >= y && u.cy < y + h; }
    void Text(HDC dc, int x, int y, const char* t, COLORREF c) {
        wchar_t w[256]; const int n = MultiByteToWideChar(CP_UTF8, 0, t, -1, w, 256);
        if (n <= 1) return;
        SetTextColor(dc, c); TextOutW(dc, x, y, w, n - 1);
    }
    int DrawTextU8(HDC dc, const char* t, int, RECT* r, UINT fmt) {
        wchar_t w[256]; const int n = MultiByteToWideChar(CP_UTF8, 0, t, -1, w, 256);
        if (n <= 1) return 0;
        return DrawTextW(dc, w, -1, r, fmt);
    }

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
        static volatile LONG* dragging = nullptr;
        char buf[200]; sprintf_s(buf, "%s: %ld%s", label, (long)*v, suffix);
        Text(u.dc, x + 6, y, buf, kText);
        const int tx = x + 6, ty = y + 26, tw = w - 12;
        if (u.click && Hit(u, tx - 4, ty - 8, tw + 8, 22)) dragging = v;
        if (!u.held) dragging = nullptr;
        if (dragging == v) {
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

    bool Button(Ui& u, int x, int y, int w, int h, const char* label) {
        const bool hot = Hit(u, x, y, w, h);
        RECT r = { x, y, x + w, y + h };
        HBRUSH hb = CreateSolidBrush(hot ? RGB(52, 58, 74) : kPanel); FillRect(u.dc, &r, hb); DeleteObject(hb);
        HBRUSH bd = CreateSolidBrush(kOff); FrameRect(u.dc, &r, bd); DeleteObject(bd);
        Text(u.dc, x + 10, y + (h - 16) / 2, label, kText);
        return hot && u.click;
    }

    const unsigned char kEspCols[6][3] = { { 0, 255, 70 }, { 255, 50, 50 }, { 0, 220, 255 }, { 255, 230, 0 }, { 255, 255, 255 }, { 255, 0, 220 } };

    void DrawMenu(Ui& u) {
        const int W = 380, H = 440;
        const int x0 = 0, y0 = 0;
        RECT panel = { x0, y0, x0 + W, y0 + H };
        HBRUSH pb = CreateSolidBrush(kBg); FillRect(u.dc, &panel, pb); DeleteObject(pb);
        HBRUSH bd = CreateSolidBrush(kAccent); FrameRect(u.dc, &panel, bd); DeleteObject(bd);
        RECT title = { x0, y0, x0 + W, y0 + 34 };
        HBRUSH tb = CreateSolidBrush(kPanel); FillRect(u.dc, &title, tb); DeleteObject(tb);
        Text(u.dc, x0 + 14, y0 + 8, "CS:S v34  |  Меню", kAccent);

        static int tab = 0;
        const char* names[5] = { "Триггер", "Визуал", "Аим", "Рейдж", "Прочее" };
        for (int t = 0; t < 5; ++t) {
            const int tx = x0 + 8 + t * 74;
            if (u.click && Hit(u, tx, y0 + 44, 70, 26)) tab = t;
            RECT tr = { tx, y0 + 44, tx + 70, y0 + 70 };
            HBRUSH tbr = CreateSolidBrush(tab == t ? kAccent : kPanel); FillRect(u.dc, &tr, tbr); DeleteObject(tbr);
            Text(u.dc, tx + 8, y0 + 49, names[t], tab == t ? RGB(15, 20, 15) : kText);
        }

        const int x = x0 + 12, w = W - 24; int y = y0 + 84;
        if (tab == 0) {
            Checkbox(u, x, y, w, "Триггербот включён", &g_enabled);                        y += 30;
            Text(u.dc, x + 6, y, "Условия", kDim);                                         y += 22;
            Checkbox(u, x, y, w, "Не стрелять по своим", &g_opt.teamCheck);                y += 28;
            Checkbox(u, x, y, w, "Стрелять только на земле", &g_opt.ground);               y += 28;
            Checkbox(u, x, y, w, "Остановиться перед выстрелом (точность)", &g_opt.speedCheck); y += 28;
            Checkbox(u, x, y, w, "Снайперки: авто-прицел перед выстрелом", &g_opt.scopeCheck); y += 34;
            Text(u.dc, x + 6, y, "Скорость", kDim);                                        y += 22;
            Slider(u, x, y, w, "Интервал выстрелов (меньше = быстрее)", &g_opt.intervalPct, 50, 200);
        } else if (tab == 1) {
            Checkbox(u, x, y, w, "Подсветка врагов (сквозь стены)", &g_opt.esp);           y += 28;
            Checkbox(u, x, y, w, "Рамка", &g_opt.espBox);                                  y += 28;
            Checkbox(u, x, y, w, "Полоска здоровья", &g_opt.espHp);                        y += 28;
            Checkbox(u, x, y, w, "Имя", &g_opt.espName);                                   y += 28;
            Checkbox(u, x, y, w, "Дистанция", &g_opt.espDist);                             y += 28;
            Checkbox(u, x, y, w, "Выделять цель под прицелом", &g_opt.espAimed);           y += 32;
            Text(u.dc, x + 6, y + 1, "Цвет", kText);
            for (int i = 0; i < 6; ++i) {
                const int sx = x + 70 + i * 40;
                if (u.click && Hit(u, sx, y, 32, 20)) InterlockedExchange(&g_opt.espColor, i);
                RECT sr = { sx, y, sx + 32, y + 20 };
                HBRUSH sb2 = CreateSolidBrush(RGB(kEspCols[i][0], kEspCols[i][1], kEspCols[i][2])); FillRect(u.dc, &sr, sb2); DeleteObject(sb2);
                if (g_opt.espColor == i) { HBRUSH fb = CreateSolidBrush(RGB(255, 255, 255)); FrameRect(u.dc, &sr, fb); RECT s2 = { sx + 1, y + 1, sx + 31, y + 19 }; FrameRect(u.dc, &s2, fb); DeleteObject(fb); }
            }
            y += 34;
            Checkbox(u, x, y, w, "Широкий экран (Hor+), если рамки плывут", &g_opt.wideFov); y += 34;
            Text(u.dc, x + 6, y, "Показываются только враги.", kDim);
        } else if (tab == 2) {
            Checkbox(u, x, y, w, "Аим-ассист", &g_opt.aimOn);                             y += 28;
            Checkbox(u, x, y, w, "Целиться в голову (выкл = в тело)", &g_opt.aimHead);     y += 28;
            Checkbox(u, x, y, w, "Плавный аим (видимый, выкл = тихий)", &g_opt.aimSmooth); y += 28;
            Checkbox(u, x, y, w, "Без разброса пуль (nospread)", &g_opt.noSpread);       y += 34;
            Slider(u, x, y, w, "Скорость наводки (больше = быстрее)", &g_opt.aimSpeed, 1, 100);  y += 50;
            Slider(u, x, y, w, "Радиус FOV", &g_opt.aimFov, 1, 180, "°");                   y += 50;
            const char* pn[3] = { "Приоритет: ближе к прицелу", "Приоритет: ближе по дистанции", "Приоритет: меньше здоровья" };
            if (Button(u, x, y, w, 26, pn[g_opt.aimPrio % 3])) InterlockedExchange(&g_opt.aimPrio, (g_opt.aimPrio + 1) % 3);
            y += 38;
            Text(u.dc, x + 6, y, "Работает при стрельбе и гасит отдачу.", kDim);          y += 20;
            Text(u.dc, x + 6, y, "Плавный режим ждёт наводки и стреляет.", kDim);
        } else if (tab == 3) {
            Checkbox(u, x, y, w, "РЕЙДЖ-режим (бета)", &g_opt.rage);                      y += 34;
            Text(u.dc, x + 6, y, "Каждый выстрел уходит во врага:", kDim);                 y += 20;
            Text(u.dc, x + 6, y, "голова, иначе тело, иначе ноги.", kDim);                 y += 20;
            Text(u.dc, x + 6, y, "Пуля летит строго в цель, без разброса.", kDim);        y += 30;
            Slider(u, x, y, w, "Компенсация отдачи", &g_opt.recoilPct, 0, 300);            y += 56;
            Text(u.dc, x + 6, y, "Пули летят выше цели - увеличь,", kDim);                 y += 20;
            Text(u.dc, x + 6, y, "ниже цели - уменьши. Старт: 200%.", kDim);               y += 28;
            Text(u.dc, x + 6, y, "Цель и голову берёт из вкладки Аим.", kDim);                 y += 24;
            Text(u.dc, x + 6, y, InterlockedCompareExchange(&g_fbHooked, 0, 0) ? "Серверный хук: активен" : "Серверный хук: не найден", kDim);
        } else {
            Checkbox(u, x, y, w, "Банихоп (держи пробел)", &g_opt.bhop);                   y += 28;
            Checkbox(u, x, y, w, "Автострейф (в воздухе, за мышью)", &g_opt.autoStrafe);    y += 40;
            Text(u.dc, x + 6, y, "Настройки", kDim);                                       y += 24;
            if (Button(u, x, y, 108, 28, "Сохранить")) CfgSave();
            if (Button(u, x + 116, y, 108, 28, "Загрузить")) CfgLoad();
            if (Button(u, x + 232, y, 108, 28, "Сбросить")) CfgReset();
            y += 42;
            Text(u.dc, x + 6, y, "Сохраняется само при закрытии меню.", kDim);            y += 20;
            Text(u.dc, x + 6, y, "Файл css_helper.ini лежит рядом с dll.", kDim);
        }
        Text(u.dc, x0 + 14, y0 + H - 26, "RShift - закрыть меню   |   END - выгрузить", kDim);
    }

    // ============================ in-game rendering (D3D9 Present hook) ============================
    volatile LONG g_scrW = 1024, g_scrH = 768;
    volatile LONG g_renderHooked = 0, g_resFreed = 0, g_renderFault = 0;
    HWND g_inputWnd = nullptr;
    HANDLE g_inputThread = nullptr;

    void LogMsg(const char* fmt, ...) {
        char path[MAX_PATH]; if (!GetModuleFileNameA(g_self, path, MAX_PATH)) return;
        char* s = strrchr(path, '\\'); if (!s) return; strcpy_s(s + 1, MAX_PATH - (s + 1 - path), "css_helper_log.txt");
        FILE* f = nullptr; if (fopen_s(&f, path, "a") != 0 || !f) return;
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fclose(f);
    }

    // ---- GDI canvas (DIB) that is uploaded to a texture ----
    struct Canvas {
        HDC dc = nullptr; HBITMAP bmp = nullptr; uint32_t* px = nullptr; int w = 0, h = 0;
        bool Init(int W, int H) {
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
            HDC sdc = GetDC(nullptr); dc = CreateCompatibleDC(sdc); ReleaseDC(nullptr, sdc);
            if (!dc) return false;
            bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
            if (!bmp || !px) return false;
            SelectObject(dc, bmp); SetBkMode(dc, TRANSPARENT); w = W; h = H;
            return true;
        }
        void Clear() { GdiFlush(); memset(px, 0, (size_t)w * h * 4); }
        void Finish() {   // black = transparent, everything else opaque
            GdiFlush();
            const size_t n = (size_t)w * h;
            for (size_t i = 0; i < n; ++i) { const uint32_t c = px[i] & 0x00FFFFFF; px[i] = c ? (c | 0xFF000000u) : 0u; }
        }
        void Free() { if (dc) DeleteDC(dc); if (bmp) DeleteObject(bmp); dc = nullptr; bmp = nullptr; px = nullptr; }
    };
    struct Tex { IDirect3DTexture9* t = nullptr; };
    struct VCT { float x, y, z, rhw; DWORD c; float u, v; };
    VCT g_vct[6144]; int g_nvct = 0;
    Canvas g_atlasCv; Tex g_atlasTex; HFONT g_fEsp = nullptr; int g_adv[95];
    const int kAtlasW = 256, kAtlasH = 128, kCell = 16;
    int TextWidth(const char* t) { int w = 0; for (; *t; ++t) { const int c = (unsigned char)*t - 32; w += (c >= 0 && c < 95) ? g_adv[c] : 0; } return w; }
    void GlyphQuads(int x, int y, const char* t, DWORD col) {
        for (; *t; ++t) {
            const int c = (unsigned char)*t - 32; if (c < 0 || c >= 95) continue;
            const int cx = (c % 16) * kCell, cy = (c / 16) * kCell, a = g_adv[c];
            if (g_nvct + 6 > 6144) return;
            const float x0 = (float)x - 0.5f, y0 = (float)y - 0.5f, x1 = (float)(x + a) - 0.5f, y1 = (float)(y + 15) - 0.5f;
            const float u0 = (float)cx / kAtlasW, u1 = (float)(cx + a) / kAtlasW, v0 = (float)cy / kAtlasH, v1 = (float)(cy + 15) / kAtlasH;
            const VCT p0 = { x0, y0, 0, 1, col, u0, v0 }, p1 = { x1, y0, 0, 1, col, u1, v0 }, p2 = { x1, y1, 0, 1, col, u1, v1 }, p3 = { x0, y1, 0, 1, col, u0, v1 };
            g_vct[g_nvct++] = p0; g_vct[g_nvct++] = p1; g_vct[g_nvct++] = p2;
            g_vct[g_nvct++] = p0; g_vct[g_nvct++] = p2; g_vct[g_nvct++] = p3;
            x += a;
        }
    }
    void DrawString(int x, int y, const char* t, DWORD col) {
        GlyphQuads(x + 1, y + 1, t, D3DCOLOR_ARGB(255, 0, 0, 0));
        GlyphQuads(x, y, t, col);
    }

    void Upload(Tex& t, Canvas& c) {
        D3DLOCKED_RECT lr;
        if (!t.t || FAILED(t.t->LockRect(0, &lr, nullptr, 0))) return;
        for (int y = 0; y < c.h; ++y) memcpy((unsigned char*)lr.pBits + (size_t)y * lr.Pitch, c.px + (size_t)y * c.w, (size_t)c.w * 4);
        t.t->UnlockRect(0);
    }

    // ---- vertex batches ----
    struct VC { float x, y, z, rhw; DWORD c; };
    struct VT { float x, y, z, rhw, u, v; };
    VC g_vc[3072]; int g_nvc = 0;

    void Quad(float x, float y, float w, float h, DWORD c) {
        if (g_nvc + 6 > 3072) return;
        const float x0 = x - 0.5f, y0 = y - 0.5f, x1 = x + w - 0.5f, y1 = y + h - 0.5f;
        const VC a = { x0, y0, 0, 1, c }, b = { x1, y0, 0, 1, c }, d = { x1, y1, 0, 1, c }, e = { x0, y1, 0, 1, c };
        g_vc[g_nvc++] = a; g_vc[g_nvc++] = b; g_vc[g_nvc++] = d;
        g_vc[g_nvc++] = a; g_vc[g_nvc++] = d; g_vc[g_nvc++] = e;
    }
    void Tri(float ax, float ay, float bx, float by, float cx, float cy, DWORD c) {
        if (g_nvc + 3 > 3072) return;
        const VC a = { ax - 0.5f, ay - 0.5f, 0, 1, c }, b = { bx - 0.5f, by - 0.5f, 0, 1, c }, d = { cx - 0.5f, cy - 0.5f, 0, 1, c };
        g_vc[g_nvc++] = a; g_vc[g_nvc++] = b; g_vc[g_nvc++] = d;
    }
    void BoxOutline(float x0, float y0, float x1, float y1, float th, DWORD c) {
        Quad(x0, y0, x1 - x0, th, c); Quad(x0, y1 - th, x1 - x0, th, c);
        Quad(x0, y0, th, y1 - y0, c); Quad(x1 - th, y0, th, y1 - y0, c);
    }

    void SetBaseStates(IDirect3DDevice9* d) {
        d->SetVertexShader(nullptr); d->SetPixelShader(nullptr);
        d->SetRenderState(D3DRS_ZENABLE, FALSE); d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD); d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); d->SetRenderState(D3DRS_LIGHTING, FALSE);
        d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE); d->SetRenderState(D3DRS_FOGENABLE, FALSE);
        d->SetRenderState(D3DRS_STENCILENABLE, FALSE); d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF); d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID); d->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
        d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    void FlushSolids(IDirect3DDevice9* d) {
        if (g_nvc >= 3) {
            d->SetTexture(0, nullptr);
            d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, g_nvc / 3, g_vc, sizeof(VC));
        }
        g_nvc = 0;
    }
    void DrawTex(IDirect3DDevice9* d, IDirect3DTexture9* t, float x, float y, float w, float h, float u1, float v1) {
        if (!t) return;
        d->SetTexture(0, t);
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        const float x0 = x - 0.5f, y0 = y - 0.5f, x1 = x + w - 0.5f, y1 = y + h - 0.5f;
        const VT v[6] = { { x0, y0, 0, 1, 0, 0 }, { x1, y0, 0, 1, u1, 0 }, { x1, y1, 0, 1, u1, v1 },
                          { x0, y0, 0, 1, 0, 0 }, { x1, y1, 0, 1, u1, v1 }, { x0, y1, 0, 1, 0, v1 } };
        d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, v, sizeof(VT));
    }

    // ---- frame state ----
    IDirect3DDevice9* g_dev = nullptr;
    Canvas g_menuCv, g_hudCv; Tex g_menuTex, g_hudTex;
    HFONT g_fMenu = nullptr, g_fHud = nullptr;
    char g_hudKey[256] = "";
    const int kMenuTex = 512, kHudW = 512, kHudH = 128, kMenuW = 380, kMenuH = 440, kHudUsedW = 320, kHudUsedH = 96;

    void FreeRes() {
        if (g_menuTex.t) { g_menuTex.t->Release(); g_menuTex.t = nullptr; }
        if (g_hudTex.t) { g_hudTex.t->Release(); g_hudTex.t = nullptr; }
        g_menuCv.Free(); g_hudCv.Free(); g_atlasCv.Free();
        if (g_atlasTex.t) { g_atlasTex.t->Release(); g_atlasTex.t = nullptr; }
        if (g_fEsp) { DeleteObject(g_fEsp); g_fEsp = nullptr; }
        if (g_fMenu) { DeleteObject(g_fMenu); g_fMenu = nullptr; }
        if (g_fHud) { DeleteObject(g_fHud); g_fHud = nullptr; }
        g_hudKey[0] = 0;
    }

    bool EnsureRes(IDirect3DDevice9* dev) {
        if (g_menuTex.t && g_hudTex.t && g_atlasTex.t) return true;
        FreeRes();
        if (!g_menuCv.Init(kMenuTex, kMenuTex) || !g_hudCv.Init(kHudW, kHudH)) { FreeRes(); return false; }
        if (FAILED(dev->CreateTexture(kMenuTex, kMenuTex, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_menuTex.t, nullptr)) ||
            FAILED(dev->CreateTexture(kHudW, kHudH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_hudTex.t, nullptr))) { FreeRes(); return false; }
        if (!g_atlasCv.Init(kAtlasW, kAtlasH) ||
            FAILED(dev->CreateTexture(kAtlasW, kAtlasH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_atlasTex.t, nullptr))) { FreeRes(); return false; }
        g_fEsp = CreateFontA(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Tahoma");
        {
            g_atlasCv.Clear();
            HGDIOBJ of = SelectObject(g_atlasCv.dc, g_fEsp);
            SetTextColor(g_atlasCv.dc, RGB(255, 255, 255));
            for (int c = 0; c < 95; ++c) {
                char ch = (char)(c + 32); SIZE sz = { 0, 0 };
                GetTextExtentPoint32A(g_atlasCv.dc, &ch, 1, &sz);
                g_adv[c] = sz.cx < 1 ? 1 : (sz.cx > kCell ? kCell : sz.cx);
                TextOutA(g_atlasCv.dc, (c % 16) * kCell, (c / 16) * kCell, &ch, 1);
            }
            SelectObject(g_atlasCv.dc, of);
            g_atlasCv.Finish(); Upload(g_atlasTex, g_atlasCv);
        }
        g_fMenu = CreateFontA(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        g_fHud = CreateFontA(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
        return true;
    }

    void BuildHudStrings(bool locked, bool on, char* l0, char* l1, char* l2, size_t n) {
        strcpy_s(l0, n, on ? "Trigger - on" : "Trigger - off");
        l1[0] = 0; l2[0] = 0;
        const LONG st = InterlockedCompareExchange(&g_status, 0, 0);
        if (st == 1) strcpy_s(l1, n, "интерфейсы не найдены");
        else if (st == 2) strcpy_s(l1, n, "клиент не поддерживается / хук не встал");
        else if (locked) {
            if (g_lockWho[0] && strcmp(g_lockWho, "not in game") != 0) sprintf_s(l1, n, "онлайн - отключено (%s)", g_lockWho);
            else strcpy_s(l1, n, g_lockWho[0] ? "ожидание: не в игре" : "ожидание...");
        }
        if ((g_enabled || g_opt.aimOn) && !locked) {
            const LONG hm = InterlockedCompareExchange(&g_headMode, 0, 0);
            sprintf_s(l2, n, "оружие: %s  голова: %s", g_wpnName, hm == 1 ? "скан" : (hm == 2 ? "запас" : "-"));
        }
    }

    void DrawFrame(IDirect3DDevice9* dev) {
        if (dev != g_dev) { FreeRes(); g_dev = dev; }
        IDirect3DSurface9* bb = nullptr;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
        D3DSURFACE_DESC sd; bb->GetDesc(&sd);
        const int sw = (int)sd.Width, sh = (int)sd.Height;
        InterlockedExchange(&g_scrW, sw); InterlockedExchange(&g_scrH, sh);
        if (!EnsureRes(dev)) { bb->Release(); return; }

        const bool locked = !InterlockedCompareExchange(&g_botsOnly, 0, 0);
        const bool open = InterlockedCompareExchange(&g_menuOpen, 0, 0) != 0;
        const bool on = InterlockedCompareExchange(&g_enabled, 0, 0) != 0;

        // --- textures (GDI side) ---
        const int mx = (sw - kMenuW) / 2, my = (sh - kMenuH) / 2;
        if (open) {
            if (!g_prevMenuOpen) { g_vx = sw / 2.0f; g_vy = sh / 2.0f; g_clickPending = 0; }
            g_menuCv.Clear();
            HGDIOBJ of = SelectObject(g_menuCv.dc, g_fMenu);
            Ui u = { g_menuCv.dc, InterlockedExchange(&g_clickPending, 0) != 0, g_lmbHeld, (int)g_vx - mx, (int)g_vy - my };
            DrawMenu(u);
            SelectObject(g_menuCv.dc, of);
            g_menuCv.Finish(); Upload(g_menuTex, g_menuCv);
        }
        g_prevMenuOpen = open;

        char l0[96], l1[96], l2[96];
        BuildHudStrings(locked, on, l0, l1, l2, sizeof(l0));
        char key[256]; sprintf_s(key, "%s|%s|%s", l0, l1, l2);
        if (strcmp(key, g_hudKey) != 0) {
            strcpy_s(g_hudKey, key);
            g_hudCv.Clear();
            HGDIOBJ of = SelectObject(g_hudCv.dc, g_fHud);
            RECT r0 = { 0, 2, kHudUsedW - 8, 26 }, r1 = { 0, 28, kHudUsedW - 8, 50 }, r2 = { 0, 50, kHudUsedW - 8, 72 };
            SetTextColor(g_hudCv.dc, RGB(0, 255, 70));
            DrawTextU8(g_hudCv.dc, l0, -1, &r0, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            SetTextColor(g_hudCv.dc, RGB(255, 80, 80));
            if (l1[0]) DrawTextU8(g_hudCv.dc, l1, -1, &r1, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            SetTextColor(g_hudCv.dc, RGB(120, 200, 140));
            if (l2[0]) DrawTextU8(g_hudCv.dc, l2, -1, &r2, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(g_hudCv.dc, of);
            g_hudCv.Finish(); Upload(g_hudTex, g_hudCv);
        }

        // --- ESP boxes ---
        if (g_opt.esp && !locked) {
            EspSnap es;
            if (ReadEsp(es) && GetTickCount() - es.t < 200) {
                for (int i = 0; i < es.n; ++i) {
                    const float h = es.duck[i] ? 54.0f : 72.0f;
                    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f; bool ok = true;
                    for (int c = 0; c < 8 && ok; ++c) {
                        const float q[3] = { es.feet[i][0] + ((c & 1) ? 16.0f : -16.0f),
                                             es.feet[i][1] + ((c & 2) ? 16.0f : -16.0f),
                                             es.feet[i][2] + ((c & 4) ? h : 0.0f) };
                        float x, y;
                        if (!WorldToScreen(es, q, (float)sw, (float)sh, x, y)) { ok = false; break; }
                        if (x < minx) minx = x; if (x > maxx) maxx = x;
                        if (y < miny) miny = y; if (y > maxy) maxy = y;
                    }
                    if (!ok || maxx - minx > sw * 2 || maxy - miny > sh * 2) continue;
                    const int ci = (int)(g_opt.espColor % 6);
                    DWORD col = D3DCOLOR_XRGB(kEspCols[ci][0], kEspCols[ci][1], kEspCols[ci][2]);
                    if (g_opt.espAimed && es.aimed[i]) col = D3DCOLOR_XRGB(255, 255, 255);
                    const float bx0 = floorf(minx), by0 = floorf(miny), bx1 = floorf(maxx), by1 = floorf(maxy);
                    if (g_opt.espBox) {
                        BoxOutline(bx0 - 1, by0 - 1, bx1 + 1, by1 + 1, 4.0f, D3DCOLOR_ARGB(160, 0, 0, 0));
                        BoxOutline(bx0, by0, bx1, by1, 2.0f, col);
                    }
                    if (g_opt.espHp) {
                        float hpf = (float)es.hp[i] / 100.0f; if (hpf > 1.0f) hpf = 1.0f; if (hpf < 0.0f) hpf = 0.0f;
                        const float bh = by1 - by0;
                        Quad(bx0 - 7, by0 - 1, 5, bh + 2, D3DCOLOR_ARGB(190, 0, 0, 0));
                        Quad(bx0 - 6, by1 - bh * hpf, 3, bh * hpf, D3DCOLOR_XRGB((int)(255 * (1.0f - hpf)), (int)(255 * hpf), 0));
                    }
                    const int mid = (int)((bx0 + bx1) * 0.5f);
                    if (g_opt.espName && es.idx[i] >= 0 && es.idx[i] < 65 && g_names[es.idx[i]][0]) {
                        char nb[32]; strncpy_s(nb, g_names[es.idx[i]], _TRUNCATE);
                        DrawString(mid - TextWidth(nb) / 2, (int)by0 - 17, nb, col);
                    }
                    {
                        char db[48]; db[0] = 0;
                        if (g_opt.espDist) {
                            const float ex = es.feet[i][0] - es.eye[0], ey = es.feet[i][1] - es.eye[1], ez = es.feet[i][2] - es.eye[2];
                            sprintf_s(db, "%dm", (int)(sqrtf(ex * ex + ey * ey + ez * ez) * 0.01905f));
                        }
                        if (g_opt.espHp) { char hb[16]; sprintf_s(hb, "%s%dhp", db[0] ? " " : "", es.hp[i]); strcat_s(db, hb); }
                        if (db[0]) DrawString(mid - TextWidth(db) / 2, (int)by1 + 2, db, col);
                    }
                }
            }
        }

        // --- draw ---
        IDirect3DSurface9* oldRt = nullptr; dev->GetRenderTarget(0, &oldRt);
        IDirect3DStateBlock9* sb = nullptr;
        if (SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) && sb) {
            if (SUCCEEDED(dev->BeginScene())) {
                dev->SetRenderTarget(0, bb);
                D3DVIEWPORT9 vp = { 0, 0, (DWORD)sw, (DWORD)sh, 0.0f, 1.0f }; dev->SetViewport(&vp);
                SetBaseStates(dev);
                FlushSolids(dev);                                                    // ESP boxes
                if (g_nvct >= 3 && g_atlasTex.t) {                                   // ESP text
                    dev->SetTexture(0, g_atlasTex.t);
                    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
                    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
                    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
                    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, g_nvct / 3, g_vct, sizeof(VCT));
                }
                g_nvct = 0;
                if (open) DrawTex(dev, g_menuTex.t, (float)mx, (float)my, (float)kMenuW, (float)kMenuH,
                                  (float)kMenuW / kMenuTex, (float)kMenuH / kMenuTex);
                DrawTex(dev, g_hudTex.t, (float)(sw - kHudUsedW), 0.0f, (float)kHudUsedW, (float)kHudUsedH,
                        (float)kHudUsedW / kHudW, (float)kHudUsedH / kHudH);
                if (open) {                                                           // cursor
                    const float cx = g_vx, cy = g_vy;
                    Tri(cx - 1, cy - 1, cx - 1, cy + 20, cx + 15, cy + 14, D3DCOLOR_XRGB(10, 10, 10));
                    Tri(cx, cy, cx, cy + 17, cx + 12, cy + 12, D3DCOLOR_XRGB(255, 255, 255));
                    FlushSolids(dev);
                }
                dev->EndScene();
            }
            sb->Apply(); sb->Release();
        }
        if (oldRt) { dev->SetRenderTarget(0, oldRt); oldRt->Release(); }
        bb->Release();
        static bool logged = false;
        if (!logged) { logged = true; LogMsg("first frame drawn %dx%d", sw, sh); }
    }

    typedef HRESULT(WINAPI* PresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    PresentFn g_origPresent = nullptr;

    HRESULT WINAPI HookPresent(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
        InterlockedIncrement(&g_activeHooks);
        if (InterlockedCompareExchange(&g_unloading, 0, 0)) {
            if (!InterlockedCompareExchange(&g_resFreed, 0, 0)) { FreeRes(); InterlockedExchange(&g_resFreed, 1); }
        } else if (!InterlockedCompareExchange(&g_renderFault, 0, 0)) {
            __try { DrawFrame(dev); }
            __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_renderFault, 1); }
        }
        const HRESULT hr = g_origPresent(dev, a, b, c, d);
        InterlockedDecrement(&g_activeHooks);
        return hr;
    }

    LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

    // creates a throw-away windowed device only to read the Present address of d3d9.dll
    bool FindPresent(void** out) {
        HMODULE m = GetModuleHandleA("d3d9.dll");
        if (!m) { LogMsg("d3d9.dll not loaded"); return false; }
        typedef IDirect3D9*(WINAPI * CreateFn)(UINT);
        CreateFn create = (CreateFn)GetProcAddress(m, "Direct3DCreate9");
        if (!create) return false;
        IDirect3D9* d3d = create(D3D_SDK_VERSION);
        if (!d3d) return false;
        const char* cls = "CSSHelperDummy";
        WNDCLASSEXA wc = {}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = DummyProc; wc.hInstance = g_self; wc.lpszClassName = cls;
        RegisterClassExA(&wc);
        HWND w = CreateWindowExA(0, cls, "", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, g_self, nullptr);
        bool ok = false;
        if (w) {
            D3DPRESENT_PARAMETERS pp = {};
            pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.hDeviceWindow = w;
            IDirect3DDevice9* dev = nullptr;
            HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w,
                                           D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &pp, &dev);
            if (FAILED(hr)) {
                LogMsg("CreateDevice HAL failed 0x%08lX, trying NULLREF", (unsigned long)hr);
                hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, w,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &pp, &dev);
            }
            if (SUCCEEDED(hr) && dev) { *out = (*(void***)dev)[17]; ok = true; dev->Release(); }
            else LogMsg("CreateDevice failed 0x%08lX", (unsigned long)hr);
            DestroyWindow(w);
        }
        UnregisterClassA(cls, g_self);
        d3d->Release();
        return ok;
    }

    bool InstallRender() {
        void* present = nullptr;
        if (!FindPresent(&present) || !present) return false;
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return false;
        if (MH_CreateHook(present, (void*)&HookPresent, (void**)&g_origPresent) != MH_OK) { LogMsg("MH_CreateHook(Present) failed"); return false; }
        if (MH_EnableHook(present) != MH_OK) { LogMsg("MH_EnableHook(Present) failed"); return false; }
        InterlockedExchange(&g_hooksReady, 1);
        InterlockedExchange(&g_renderHooked, 1);
        LogMsg("Present hooked");
        return true;
    }

    // ---- raw mouse input for the menu cursor (hidden window, own thread) ----
    LRESULT CALLBACK InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_INPUT: {
            if (!InterlockedCompareExchange(&g_menuOpen, 0, 0)) return 0;
            RAWINPUT ri; UINT sz = sizeof(ri);
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) == (UINT)-1) return 0;
            if (ri.header.dwType != RIM_TYPEMOUSE) return 0;
            const RAWMOUSE& m = ri.data.mouse;
            if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) { g_vx += (float)m.lLastX; g_vy += (float)m.lLastY; }
            const float W = (float)InterlockedCompareExchange(&g_scrW, 0, 0), H = (float)InterlockedCompareExchange(&g_scrH, 0, 0);
            if (g_vx < 0) g_vx = 0; if (g_vy < 0) g_vy = 0;
            if (g_vx > W - 1) g_vx = W - 1; if (g_vy > H - 1) g_vy = H - 1;
            if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) { g_lmbHeld = true; InterlockedExchange(&g_clickPending, 1); }
            if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) g_lmbHeld = false;
            return 0;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcA(hwnd, msg, wp, lp);
    }

    DWORD WINAPI InputThread(void*) {
        const char* cls = "CSSHelperInputV2";
        WNDCLASSEXA wc = {}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = InputProc; wc.hInstance = g_self; wc.lpszClassName = cls;
        if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;
        HWND h = CreateWindowExA(0, cls, "in", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_self, nullptr);   // never shown
        if (!h) return 1;
        g_inputWnd = h;
        RAWINPUTDEVICE rid = { 0x01, 0x02, RIDEV_INPUTSINK, h };
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        MSG m;
        while (GetMessageA(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageA(&m); }
        RAWINPUTDEVICE off = { 0x01, 0x02, RIDEV_REMOVE, nullptr };
        RegisterRawInputDevices(&off, 1, sizeof(off));
        g_inputWnd = nullptr;
        UnregisterClassA(cls, g_self);
        return 0;
    }

    // ============================ worker ============================
    DWORD WINAPI Worker(void*) {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcFreq = f.QuadPart;
        g_inputThread = CreateThread(nullptr, 0, InputThread, nullptr, 0, nullptr);
        CfgLoad();

        bool ok = false;
        for (int i = 0; i < 50 && !ok; ++i) { ok = InitInterfaces(); if (!ok) Sleep(200); }
        if (!ok) InterlockedExchange(&g_status, 1);
        else {
            if (!InstallHook()) InterlockedExchange(&g_status, 2);
            else InterlockedExchange(&g_ifaceReady, 1);
        }
        bool renderOk = false; DWORD lastTry = 0; int tries = 0; DWORD lastSv = 0;

        bool prevToggle = false, prevEnd = false;
        while (!InterlockedCompareExchange(&g_unloading, 0, 0)) {
            const DWORD now = GetTickCount();
            if (!renderOk && tries < 60 && now - lastTry >= 1000) {
                lastTry = now; ++tries;
                renderOk = InstallRender();
            }
            if (!InterlockedCompareExchange(&g_fbHooked, 0, 0) && now - lastSv >= 1000) {
                lastSv = now;
                if (InterlockedCompareExchange(&g_ifaceReady, 0, 0) && InstallServerHook()) LogMsg("server FireBullet hooked");
            }
            const bool fg = GameForeground();
            const bool tog = fg && (GetAsyncKeyState(VK_RSHIFT) & 0x8000);
            if (tog && !prevToggle) {
                const bool wasOpen = InterlockedCompareExchange(&g_menuOpen, 0, 0) != 0;
                InterlockedExchange(&g_menuOpen, wasOpen ? 0 : 1);
                if (wasOpen) CfgSave();
            }
            prevToggle = tog;
            const bool end = fg && (GetAsyncKeyState(VK_END) & 0x8000);
            if (end && !prevEnd) { InterlockedExchange(&g_unloading, 1); break; }
            prevEnd = end;
            Sleep(5);
        }

        CfgSave();
        InterlockedExchange(&g_enabled, 0);
        InterlockedExchange(&g_menuOpen, 0);
        InterlockedExchange(&g_ifaceReady, 0);
        if (InterlockedCompareExchange(&g_renderHooked, 0, 0))                    // let the render thread free its textures
            for (int i = 0; i < 300 && !InterlockedCompareExchange(&g_resFreed, 0, 0); ++i) Sleep(5);
        UninstallHook();
        if (g_inputWnd) PostMessageA(g_inputWnd, WM_CLOSE, 0, 0);
        if (g_inputThread) { WaitForSingleObject(g_inputThread, 1500); CloseHandle(g_inputThread); }
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
