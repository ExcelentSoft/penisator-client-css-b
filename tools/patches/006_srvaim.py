import sys
MARK = '[patch:006_srvaim]'
path = sys.argv[1]
s = open(path, encoding='utf-8', newline='').read().replace('\r\n', '\n')
if MARK in s:
    print('patch 006 already applied'); sys.exit(0)

def rep(a, b):
    global s
    if s.count(a) != 1:
        print('PATCH ERROR: pattern count %d for: %s' % (s.count(a), a[:90])); sys.exit(1)
    s = s.replace(a, b)

# state: is the chosen point the head?
rep('    volatile LONG g_fbHooked = 0, g_fbBad = 0', '    volatile LONG g_rHead = 0;\n    volatile LONG g_fbHooked = 0, g_fbBad = 0')
rep('bp[0] = pts[q][0]; bp[1] = pts[q][1]; bp[2] = pts[q][2]; found = true;',
    'bp[0] = pts[q][0]; bp[1] = pts[q][1]; bp[2] = pts[q][2]; found = true; g_rHead = (g_opt.aimHead && q == 0) ? 1 : 0;')

# server-side truth: trace against the SERVER hitboxes and find the real head
fn = '''    FireBulletFn g_origFireBullet = nullptr;

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
'''
rep('    FireBulletFn g_origFireBullet = nullptr;\n', fn)

# use it in the hook; drop the earlier damage boost (no manual damage changes)
rep('                ang = a2; spread = 0.0f; x = 0.0f; y = 0.0f;\n                rmod = 1.0f; dmg *= 4;\n',
    '''                { float ra[3]; const float src3[3] = { sx, sy, sz };
                  if (RefineAim(src3, g_rPt, g_rHead != 0, self, ra)) { a2[0] = ra[0]; a2[1] = ra[1]; a2[2] = ra[2]; } }
                ang = a2; spread = 0.0f; x = 0.0f; y = 0.0f;
''')

# server trace interface
rep('        if (MH_CreateHook(fn, (void*)&HookFireBullet, (void**)&g_origFireBullet) != MH_OK) { InterlockedExchange(&g_fbBad, 1); return false; }\n',
    '''        { HMODULE en = GetModuleHandleA("engine.dll");
          CreateInterfaceFn ci = en ? (CreateInterfaceFn)GetProcAddress(en, "CreateInterface") : nullptr;
          g_strace = ci ? (void**)ci("EngineTraceServer003", nullptr) : nullptr;
          LogMsg("server trace interface: %p", (void*)g_strace); }
        if (MH_CreateHook(fn, (void*)&HookFireBullet, (void**)&g_origFireBullet) != MH_OK) { InterlockedExchange(&g_fbBad, 1); return false; }
''')

s = '// ' + MARK + ' applied\n' + s
open(path, 'w', encoding='utf-8', newline='').write(s)
print('patch 006 applied')
