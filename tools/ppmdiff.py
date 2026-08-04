import sys

def read_ppm(path):
    with open(path, 'rb') as f:
        d = f.read()
    assert d[:2] == b'P6'
    i = 2
    vals = []
    while len(vals) < 3:
        while d[i] in b' \t\r\n':
            i += 1
        j = i
        while d[j] not in b' \t\r\n':
            j += 1
        vals.append(int(d[i:j]))
        i = j
    w, h, mx = vals
    i += 1
    return w, h, d[i:i + w * h * 3]

a_path, b_path = sys.argv[1], sys.argv[2]
w, h, a = read_ppm(a_path)
w2, h2, b = read_ppm(b_path)
assert (w, h) == (w2, h2)
diffs = 0
first = None
for i in range(0, len(a), 3):
    if a[i:i+3] != b[i:i+3]:
        diffs += 1
        if first is None:
            px = i // 3
            first = (px % w, px // w)
print(f'{a_path} vs {b_path}: {diffs} of {w*h} pixels differ; first diff at {first}')
