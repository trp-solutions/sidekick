#!/usr/bin/env python3
"""Prepare lossless RGB565 loops from the project's MP4s for desktop packaging."""
import gzip
from pathlib import Path
import subprocess

APP = Path(__file__).resolve().parents[1]
ROOT = APP.parent / 'esp32'
output = APP / 'media'
output.mkdir(exist_ok=True)
for name in ('colors-idle',):
    raw = subprocess.run([
        'ffmpeg', '-v', 'error', '-i', str(ROOT / 'media' / f'{name}.mp4'),
        '-vf', 'fps=10,scale=160:160', '-an', '-pix_fmt', 'rgb565be',
        '-sws_dither', 'none', '-threads', '1', '-f', 'rawvideo', 'pipe:1',
    ], capture_output=True, check=True).stdout
    if not raw or len(raw) % 51200:
        raise ValueError(f'Invalid video: {name}')
    target = output / f'{name}.rgb565.gz'
    target.write_bytes(gzip.compress(raw, mtime=0))
    print(f'{target.name}: {len(raw) // 51200} frames, {target.stat().st_size} bytes')
