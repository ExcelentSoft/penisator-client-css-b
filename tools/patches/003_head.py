import sys
MARK = '[patch:003_head]'
path = sys.argv[1]
s = open(path, encoding='utf-8', newline='').read().replace('\r\n', '\n')
if MARK in s:
    print('patch 003 already applied'); sys.exit(0)

a = s.find('    bool HeadLocate(void* ent, const float* eye, const float* feet, bool duck, float* out) {')
b = s.find("    // Valve's uniform random stream")
if a < 0 or b < 0 or b < a:
    print('PATCH ERROR: HeadLocate markers'); sys.exit(1)

new = '''    bool HeadLocate(void* ent, const float* eye, const float* feet, bool duck, float* out) {
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
        out[0] = fx + px * mo; out[1] = fy + py * mo; out[2] = sz / (float)n + 0.5f;
        return true;
    }

'''
s = s[:a] + new + s[b:]
s = '// ' + MARK + ' applied\n' + s
open(path, 'w', encoding='utf-8', newline='').write(s)
print('patch 003 applied')
