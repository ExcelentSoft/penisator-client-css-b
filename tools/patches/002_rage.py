import sys
MARK = '[patch:002_rage]'
path = sys.argv[1]
s = open(path, encoding='utf-8', newline='').read().replace('\r\n', '\n')
if MARK in s:
    print('patch 002 already applied'); sys.exit(0)

def rep(a, b):
    global s
    if s.count(a) != 1:
        print('PATCH ERROR: pattern count %d for: %s' % (s.count(a), a[:90])); sys.exit(1)
    s = s.replace(a, b)

# options + config
rep('        volatile LONG noSpread = 1;\n', '        volatile LONG noSpread = 1;\n        volatile LONG rage = 0, recoilPct = 200;\n')
rep('{ "noSpread", &g_opt.noSpread, 1, 0, 1 },\n', '{ "noSpread", &g_opt.noSpread, 1, 0, 1 },\n        { "rage", &g_opt.rage, 0, 0, 1 }, { "recoilPct", &g_opt.recoilPct, 200, 0, 300 },\n')

# normal aim stays untouched, but is skipped while rage is on
rep('        if (g_opt.aimOn && (cmd->buttons & IN_ATTACK)) {', '        if (!g_opt.rage && g_opt.aimOn && (cmd->buttons & IN_ATTACK)) {')

rage = '''        // ---- RAGE (beta): every shot is silently redirected to the best reachable enemy, recoil fully compensated
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
                    bp[0] = pts[q][0]; bp[1] = pts[q][1]; bp[2] = pts[q][2]; found = true;
                }
            }
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
            const float dd = (yaw - cmd->viewangles[1]) * kPi / 180.0f;
            const float f = cmd->forwardmove, sd = cmd->sidemove;
            cmd->forwardmove = f * cosf(dd) - sd * sinf(dd);
            cmd->sidemove = f * sinf(dd) + sd * cosf(dd);
            cmd->viewangles[0] = pitch; cmd->viewangles[1] = yaw;
        }

'''
rep('        // ---- no spread:', rage + '        // ---- no spread:')

# menu: 5 tabs (new "Рейдж" tab), clearer nospread label
rep('const char* names[4] = { "Триггер", "Визуал", "Аим", "Прочее" };', 'const char* names[5] = { "Триггер", "Визуал", "Аим", "Рейдж", "Прочее" };')
rep('for (int t = 0; t < 4; ++t) {\n            const int tx = x0 + 10 + t * 91;', 'for (int t = 0; t < 5; ++t) {\n            const int tx = x0 + 8 + t * 74;')
rep('Hit(u, tx, y0 + 44, 86, 26)) tab = t;', 'Hit(u, tx, y0 + 44, 70, 26)) tab = t;')
rep('RECT tr = { tx, y0 + 44, tx + 86, y0 + 70 };', 'RECT tr = { tx, y0 + 44, tx + 70, y0 + 70 };')
rep('Text(u.dc, tx + 12, y0 + 49, names[t]', 'Text(u.dc, tx + 8, y0 + 49, names[t]')
rep('"Без разброса пуль (и без аима)"', '"Без разброса пуль (nospread)"')
rep('        } else {\n            Checkbox(u, x, y, w, "Банихоп (держи пробел)"', '''        } else if (tab == 3) {
            Checkbox(u, x, y, w, "РЕЙДЖ-режим (бета)", &g_opt.rage);                      y += 34;
            Text(u.dc, x + 6, y, "Каждый выстрел уходит во врага:", kDim);                 y += 20;
            Text(u.dc, x + 6, y, "голова, иначе тело, иначе ноги.", kDim);                 y += 20;
            Text(u.dc, x + 6, y, "Работает, пока стреляешь. FOV не нужен.", kDim);        y += 30;
            Slider(u, x, y, w, "Компенсация отдачи", &g_opt.recoilPct, 0, 300);            y += 56;
            Text(u.dc, x + 6, y, "Пули летят выше цели - увеличь,", kDim);                 y += 20;
            Text(u.dc, x + 6, y, "ниже цели - уменьши. Старт: 200%.", kDim);               y += 28;
            Text(u.dc, x + 6, y, "Цель и голову берёт из вкладки Аим.", kDim);
        } else {
            Checkbox(u, x, y, w, "Банихоп (держи пробел)"''')

s = '// ' + MARK + ' applied\n' + s
open(path, 'w', encoding='utf-8', newline='').write(s)
print('patch 002 applied')
