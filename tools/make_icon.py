"""Generates assets/app.ico (16/32/48 px battery glyph) without third-party libs.
Only needed if you want to change the exe icon; the icon is committed."""
import struct, sys, os

def render(n):
    px = [[(0, 0, 0, 0)] * n for _ in range(n)]
    def rect(x0, y0, x1, y1, c):
        for y in range(max(0, int(y0)), min(n, int(y1))):
            for x in range(max(0, int(x0)), min(n, int(x1))):
                px[y][x] = c
    s = n / 16.0
    white, green, dark = (255, 255, 255, 255), (60, 200, 90, 255), (30, 30, 30, 255)
    rect(1*s, 4*s, 14*s, 12*s, white)            # body outline
    rect(2*s, 5*s, 13*s, 11*s, dark)             # inside
    rect(14*s, 6*s, 15.5*s, 10*s, white)         # terminal
    rect(3*s, 6*s, 10*s, 10*s, green)            # charge level
    return px

def bmp_entry(n):
    px = render(n)
    header = struct.pack('<IIIHHIIIIII', 40, n, n * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    rows = b''.join(bytes(b for (r, g, bl, a) in px[y] for b in (bl, g, r, a)) for y in range(n - 1, -1, -1))
    mask_row = ((n + 31) // 32) * 4
    return header + rows + b'\0' * (mask_row * n)

sizes = [16, 32, 48]
images = [bmp_entry(n) for n in sizes]
out = struct.pack('<HHH', 0, 1, len(sizes))
offset = 6 + 16 * len(sizes)
for n, img in zip(sizes, images):
    out += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(img), offset)
    offset += len(img)
out += b''.join(images)
dst = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets', 'app.ico')
open(dst, 'wb').write(out)
print('wrote', os.path.normpath(dst), len(out), 'bytes')
