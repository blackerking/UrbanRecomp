import sys, struct, zlib

def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:2] == b'P6'
    i = 2
    vals = []
    while len(vals) < 3:
        while data[i] in b' \t\r\n':
            i += 1
        if data[i:i+1] == b'#':
            while data[i] not in b'\r\n':
                i += 1
            continue
        j = i
        while data[j] not in b' \t\r\n':
            j += 1
        vals.append(int(data[i:j]))
        i = j
    w, h, maxval = vals
    i += 1
    pixels = data[i:i + w * h * 3]
    return w, h, pixels

def write_png(path, w, h, rgb):
    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))
    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw.extend(rgb[y*stride:(y+1)*stride])
    idat = zlib.compress(bytes(raw), 9)
    with open(path, 'wb') as f:
        f.write(sig)
        f.write(chunk(b'IHDR', ihdr))
        f.write(chunk(b'IDAT', idat))
        f.write(chunk(b'IEND', b''))

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    w, h, px = read_ppm(src)
    write_png(dst, w, h, px)
    print(f'{src} -> {dst} ({w}x{h})')
