import sys
MARK = '[patch:005_dmg]'
path = sys.argv[1]
s = open(path, encoding='utf-8', newline='').read().replace('\r\n', '\n')
if MARK in s:
    print('patch 005 already applied'); sys.exit(0)

def rep(a, b):
    global s
    if s.count(a) != 1:
        print('PATCH ERROR: pattern count %d for: %s' % (s.count(a), a[:90])); sys.exit(1)
    s = s.replace(a, b)

# no distance falloff and a damage boost for rage shots, so a hit really kills
rep('                ang = a2; spread = 0.0f; x = 0.0f; y = 0.0f;\n',
    '                ang = a2; spread = 0.0f; x = 0.0f; y = 0.0f;\n                rmod = 1.0f; dmg *= 4;\n')
# aim a little higher inside the head
rep('sz / (float)n + 0.5f;', 'sz / (float)n + 1.0f;')

s = '// ' + MARK + ' applied\n' + s
open(path, 'w', encoding='utf-8', newline='').write(s)
print('patch 005 applied')
