import re, sys
MARK = '[patch:001_aim_ru]'
path = sys.argv[1]
s = open(path, encoding='utf-8', newline='').read().replace('\r\n', '\n')
if MARK in s:
    print('patch 001 already applied'); sys.exit(0)

def rep(a, b):
    global s
    if s.count(a) != 1:
        print('PATCH ERROR: pattern count %d for: %s' % (s.count(a), a[:90])); sys.exit(1)
    s = s.replace(a, b)

def rep_between(start, end, new):
    global s
    a = s.find(start)
    if a < 0: print('PATCH ERROR: start not found: ' + start[:80]); sys.exit(1)
    b = s.find(end, a)
    if b < 0: print('PATCH ERROR: end not found: ' + end[:80]); sys.exit(1)
    s = s[:a] + new + s[b:]

# ---------- options / config ----------
rep('        volatile LONG bhop = 0, autoStrafe = 0;\n', '        volatile LONG bhop = 0, autoStrafe = 0;\n        volatile LONG noSpread = 1;\n')
rep('{ "autoStrafe", &g_opt.autoStrafe, 0, 0, 1 },\n', '{ "autoStrafe", &g_opt.autoStrafe, 0, 0, 1 },\n        { "noSpread", &g_opt.noSpread, 1, 0, 1 },\n')

# ---------- weapon fire intervals (closer to the real fire rates) ----------
rep_between('    const WInfo kWeapons[] = {', '    char g_wpnName[40]', '''    const WInfo kWeapons[] = {
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
''')
rep('    char g_wpnName[40] = "?";\n', '    char g_wpnName[40] = "?";\n    volatile LONG g_headMode = 0;     // debug: 1 = head found by scan, 2 = fallback guess\n')

# ---------- head locator + nospread ----------
rep_between('    // Finds the real head position:', '    void OnCreateMove(UserCmd* cmd) {', '''    // ---- engine ray clipped against ONE entity (its real hitboxes in the current client pose)
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
        const float fx = feet[0], fy = feet[1];
        const float zHi = feet[2] + (duck ? 62.0f : 80.0f), zLo = feet[2] + (duck ? 36.0f : 52.0f);
        float topZ = -1e9f;
        for (float z = zHi; z >= zLo; z -= 1.0f) {
            const float pt[3] = { fx, fy, z };
            if (ClipHit(ent, eye, pt)) { topZ = z; break; }
        }
        if (topZ < -1e8f) return false;
        const float hz = topZ - 5.0f;
        const float dx = fx - eye[0], dy = fy - eye[1];
        const float dl = sqrtf(dx * dx + dy * dy);
        out[0] = fx; out[1] = fy; out[2] = hz;
        if (dl < 1.0f) return true;
        const float px = -dy / dl, py = dx / dl;
        float lo = 1e9f, hi = -1e9f;
        for (int o = -10; o <= 10; ++o) {
            const float pt[3] = { fx + px * o, fy + py * o, hz };
            if (ClipHit(ent, eye, pt)) { if ((float)o < lo) lo = (float)o; if ((float)o > hi) hi = (float)o; }
        }
        if (lo <= hi) { const float mid = (lo + hi) * 0.5f; out[0] = fx + px * mid; out[1] = fy + py * mid; }
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

''')

# ---------- trigger remembers its target ----------
rep('        // ---- triggerbot\n', '        // ---- triggerbot\n        int trigBest = -1;\n')
rep('                cmd->buttons |= IN_ATTACK;\n                g_nextShot = now', '                cmd->buttons |= IN_ATTACK; trigBest = best;\n                g_nextShot = now')

# ---------- aim block ----------
rep_between('        // ---- FOV aim assist: only while firing', '    // ---------------- hook ----------------', '''        // ---- FOV aim assist: only while firing
        if (g_opt.aimOn && (cmd->buttons & IN_ATTACK)) {
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

        // ---- no spread: pick the random seed whose bullet spread is ~zero (server uses the seed we send)
        if (g_opt.noSpread && (cmd->buttons & IN_ATTACK) && wc != W_SHOTGUN) cmd->random_seed = BestSeed();
    }

''')

# ---------- Russian UI (UTF-8 source, converted to UTF-16 for GDI) ----------
rep('    void Text(HDC dc, int x, int y, const char* t, COLORREF c) { SetTextColor(dc, c); TextOutA(dc, x, y, t, (int)strlen(t)); }',
'''    void Text(HDC dc, int x, int y, const char* t, COLORREF c) {
        wchar_t w[256]; const int n = MultiByteToWideChar(CP_UTF8, 0, t, -1, w, 256);
        if (n <= 1) return;
        SetTextColor(dc, c); TextOutW(dc, x, y, w, n - 1);
    }
    int DrawTextU8(HDC dc, const char* t, int, RECT* r, UINT fmt) {
        wchar_t w[256]; const int n = MultiByteToWideChar(CP_UTF8, 0, t, -1, w, 256);
        if (n <= 1) return 0;
        return DrawTextW(dc, w, -1, r, fmt);
    }''')
rep('char buf[96]; sprintf_s(buf, "%s: %ld%s", label, (long)*v, suffix);', 'char buf[200]; sprintf_s(buf, "%s: %ld%s", label, (long)*v, suffix);')

rep_between('    void DrawMenu(Ui& u) {', '    // ============================ in-game rendering', '''    void DrawMenu(Ui& u) {
        const int W = 380, H = 440;
        const int x0 = 0, y0 = 0;
        RECT panel = { x0, y0, x0 + W, y0 + H };
        HBRUSH pb = CreateSolidBrush(kBg); FillRect(u.dc, &panel, pb); DeleteObject(pb);
        HBRUSH bd = CreateSolidBrush(kAccent); FrameRect(u.dc, &panel, bd); DeleteObject(bd);
        RECT title = { x0, y0, x0 + W, y0 + 34 };
        HBRUSH tb = CreateSolidBrush(kPanel); FillRect(u.dc, &title, tb); DeleteObject(tb);
        Text(u.dc, x0 + 14, y0 + 8, "CS:S v34  |  Меню", kAccent);

        static int tab = 0;
        const char* names[4] = { "Триггер", "Визуал", "Аим", "Прочее" };
        for (int t = 0; t < 4; ++t) {
            const int tx = x0 + 10 + t * 91;
            if (u.click && Hit(u, tx, y0 + 44, 86, 26)) tab = t;
            RECT tr = { tx, y0 + 44, tx + 86, y0 + 70 };
            HBRUSH tbr = CreateSolidBrush(tab == t ? kAccent : kPanel); FillRect(u.dc, &tr, tbr); DeleteObject(tbr);
            Text(u.dc, tx + 12, y0 + 49, names[t], tab == t ? RGB(15, 20, 15) : kText);
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
            Checkbox(u, x, y, w, "Без разброса пуль (и без аима)", &g_opt.noSpread);       y += 34;
            Slider(u, x, y, w, "Скорость наводки (больше = быстрее)", &g_opt.aimSpeed, 1, 100);  y += 50;
            Slider(u, x, y, w, "Радиус FOV", &g_opt.aimFov, 1, 180, "°");                   y += 50;
            const char* pn[3] = { "Приоритет: ближе к прицелу", "Приоритет: ближе по дистанции", "Приоритет: меньше здоровья" };
            if (Button(u, x, y, w, 26, pn[g_opt.aimPrio % 3])) InterlockedExchange(&g_opt.aimPrio, (g_opt.aimPrio + 1) % 3);
            y += 38;
            Text(u.dc, x + 6, y, "Работает при стрельбе и гасит отдачу.", kDim);          y += 20;
            Text(u.dc, x + 6, y, "Плавный режим ждёт наводки и стреляет.", kDim);
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

''')

# HUD strings + drawing
rep('strcpy_s(l1, n, "interfaces not found");', 'strcpy_s(l1, n, "интерфейсы не найдены");')
rep('strcpy_s(l1, n, "unsupported client.dll / hook failed");', 'strcpy_s(l1, n, "клиент не поддерживается / хук не встал");')
rep('sprintf_s(l1, n, "online - disabled (%s)", g_lockWho);', 'sprintf_s(l1, n, "онлайн - отключено (%s)", g_lockWho);')
rep('strcpy_s(l1, n, g_lockWho[0] ? "waiting: not in game" : "waiting...");', 'strcpy_s(l1, n, g_lockWho[0] ? "ожидание: не в игре" : "ожидание...");')
rep('sprintf_s(l2, n, "wpn: %s", g_wpnName);', '''{
            const LONG hm = InterlockedCompareExchange(&g_headMode, 0, 0);
            sprintf_s(l2, n, "оружие: %s  голова: %s", g_wpnName, hm == 1 ? "скан" : (hm == 2 ? "запас" : "-"));
        }''')
assert s.count('DrawTextA(g_hudCv.dc,') == 3
s = s.replace('DrawTextA(g_hudCv.dc,', 'DrawTextU8(g_hudCv.dc,')

s = '// ' + MARK + ' applied\n' + s
open(path, 'w', encoding='utf-8', newline='').write(s)
print('patch 001 applied')
