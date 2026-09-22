#!/usr/bin/env python3
"""Build Electron-compatible icons from the SVG sources (requires rsvg-convert)."""
from pathlib import Path
import struct
import subprocess

ICONS = Path(__file__).resolve().parents[1] / 'icons'
OUTPUT = ICONS / 'generated'
OUTPUT.mkdir(exist_ok=True)


def render(source, target, white=False):
    svg = (ICONS / source).read_text()
    if white:
        svg = svg.replace('stroke="#202020"', 'stroke="#FFFFFF"')
    subprocess.run(['rsvg-convert', '-o', str(OUTPUT / target)], input=svg.encode(), check=True)
    return (OUTPUT / target).read_bytes()


render('sidekick-master.svg', 'sidekick.png')
render('macos/sidekick-menubar-22.svg', 'sidekickTemplate.png')
render('macos/sidekick-menubar-44.svg', 'sidekickTemplate@2x.png')

# ICO stores all supplied Windows sizes; Windows selects the appropriate DPI.
sizes = (16, 20, 24, 32, 40, 48, 64)
for suffix in ('', '-white'):
    images = [render(f'windows/sidekick-tray-{size}.svg', f'windows-{size}{suffix}.png', bool(suffix)) for size in sizes]
    offset = 6 + 16 * len(images)
    directory = bytearray(struct.pack('<HHH', 0, 1, len(images)))
    for size, image in zip(sizes, images):
        directory.extend(struct.pack('<BBBBHHII', size, size, 0, 0, 1, 32, len(image), offset))
        offset += len(image)
    (OUTPUT / f'sidekick{suffix}.ico').write_bytes(directory + b''.join(images))

    for size in (16, 22, 24, 32):
        render(f'linux/sidekick-tray-{size}.svg', f'linux-{size}{suffix}.png', bool(suffix))
print(f'Built tray and application icons in {OUTPUT}')
