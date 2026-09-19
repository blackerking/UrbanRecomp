#!/usr/bin/env python3
"""Verify centered screens/pop-ups with a stationary city HUD, using real ROM inputs.

The stock PPU with BG3/OBJ hidden and the old panel's subscreen occlusion
removed is an independent oracle for the dimmed advisor background. No guest
execution is forced; the diagnostic register is restored after drawing.
Requires Pillow.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image, ImageDraw


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('exe', 'rom', 'artifacts'):
        ap.add_argument('--' + name, type=Path, required=True)
    args = ap.parse_args()
    exe = args.exe.resolve(strict=True)
    rom = args.rom.resolve(strict=True)
    args.artifacts.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='simcity-layout-', dir=args.artifacts.resolve()))
    print('Artifacts:', root, flush=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('SC_', 'LNG_', 'SNESRECOMP_'))}
    route = ['--input', '600:10:0008', '--input', '1000:10:0001',
             '--input', '1500:10:0001', '--input', '2000:10:0001']

    def run(name, options, extra_env=None):
        path = root / name
        path.mkdir()
        child = env | {'SC_DUMP_DIR': str(path), 'SC_DUMP_INTERVAL': '100',
                       'SC_RENDER_AUDIT': '1', 'SC_STATE_TRACE': str(path / 'state.txt')}
        child.update(extra_env or {})
        with (path / 'run.log').open('wb') as log:
            subprocess.run([str(exe), str(rom), '--qualify', '2400'] + options + route,
                           cwd=path, env=child, stdout=log, stderr=log, check=True, timeout=180)
        print('Captured', name, flush=True)
        return path

    stock = run('stock', ['--no-widescreen'])
    background = run('stock-background', ['--no-widescreen'],
                     {'SC_LAYER_MASK': '3', 'SC_SUB_WINDOW_MASK': '0'})
    assert (stock / 'state.txt').read_bytes() == (background / 'state.txt').read_bytes()
    results = []
    previews = []
    for name, options, size in [
        ('wide-left', ['--aspect', '32:9'], (684, 224)),
        ('portrait-left', ['--aspect', 'Fit', '--window-size', '720x1280'], (256, 532)),
    ]:
        path = run(name, ['--widescreen', '--view-position', 'TopLeft'] + options)
        assert (stock / 'state.txt').read_bytes() == (path / 'state.txt').read_bytes()
        advisor_frames = []
        for f in sorted(stock.glob('frame_*.ppm')):
            frame = int(f.stem.split('_')[1])
            a = Image.open(f).convert('RGB')
            b = Image.open(path / f.name).convert('RGB')
            assert b.size == size
            audit = json.loads((path / (f.name + '.json')).read_text())
            cx, cy = (size[0] - 256) // 2, (size[1] - 224) // 2
            x, y = audit['core_x'], audit['core_y']
            # These points have stable, known screens on the ordinary route.
            if frame in (100, 500, 700, 1200):
                assert (x, y) == (cx, cy), (name, frame, 'menu not centered')
            if frame in (2100, 2200, 2300):
                assert (x, y) == (0, 0), (name, frame, 'city HUD moved')
            if frame in (1700, 1800, 1900):
                assert audit['advisor_centered'], (name, frame, 'advisor not centered')
            if audit['advisor_centered']:
                assert (x, y) == (0, 0)
                advisor_frames.append(frame)
                bg = Image.open(background / f.name).convert('RGB')
                mask = Image.open(path / (f.name + '.panel.pgm'))
                # A foreground pixel must reproduce the original finished
                # page, including its text, portrait, palette and brightness.
                for yy in range(224):
                    for xx in range(256):
                        if 8 <= xx < 248 and a.getpixel((xx, yy)) != bg.getpixel((xx, yy)):
                            assert mask.getpixel((xx, yy)), (frame, xx, yy, 'missing foreground')
                        if mask.getpixel((xx, yy)):
                            assert a.getpixel((xx, yy)) == b.getpixel((cx + xx, cy + yy)), \
                                (frame, xx, yy, 'changed page pixel')
                        # The exposed native core must match the stock PPU's
                        # background-only pass, except overscan staging edges
                        # and pixels covered by the newly positioned panel.
                        dx, dy = xx - cx, yy - cy
                        covered = 0 <= dx < 256 and 0 <= dy < 224 and mask.getpixel((dx, dy))
                        if 8 <= xx < 248 and not covered:
                            assert bg.getpixel((xx, yy)) == b.getpixel((xx, yy)), \
                                (name, frame, xx, yy, 'city/HUD background moved')
            else:
                for row, repair in enumerate(audit['edge_repairs']):
                    left = 8 if repair & 1 else 0
                    right = 248 if repair & 2 else 256
                    assert a.crop((left, row, right, row + 1)).tobytes() == \
                        b.crop((x + left, y + row, x + right, y + row + 1)).tobytes(), \
                        (name, frame, row, 'native picture changed')
            if frame in (500, 700, 1200, 1900, 2300):
                b.save(path / f'{frame}.png')
                previews.append((name + ' ' + str(frame), b.copy()))
        assert advisor_frames
        results.append({'case': name, 'frames': 2400, 'advisor_captures': advisor_frames})
        print('PASS', name, 'page pixels, background-only oracle, menu/HUD anchors and guest state', flush=True)
    contact = Image.new('RGB', (1026, 5 * 240), '#222222')
    draw = ImageDraw.Draw(contact)
    for i, (label, im) in enumerate(previews):
        col, row = i // 5, i % 5
        im.thumbnail((680, 212) if col == 0 else (342, 212))
        x = 0 if col == 0 else 684
        contact.paste(im, (x, row * 240 + 24))
        draw.text((x + 4, row * 240 + 4), label, fill='white')
    contact.save(root / 'review.png')
    (root / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
    print('PASS: inspect', root / 'review.png', flush=True)


if __name__ == '__main__':
    main()
