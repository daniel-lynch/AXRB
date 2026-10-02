#!/usr/bin/env python3
"""Checks the emulator's ETC2 decode against a host reference decoder.

  compare_etc2.py gen <blocks.bin> <vkformat> <width> <height> <mips> [seed] [images]
      writes random ETC2/EAC blocks (every bit pattern is a valid block, so
      random data exercises every mode: individual, differential, T, H, planar).
  compare_etc2.py check <blocks.bin> <vkformat> <width> <height> <mips> <got.rgba> [diff.png] [images]
      decodes <blocks.bin> with texture2ddecoder and compares it with what
      tests/android/vulkan_etc2_android read back from the GPU, per level and
      per ETC2 mode of the block.

Needs: pip install texture2ddecoder numpy pillow
"""
import sys

import numpy as np
import texture2ddecoder as t2d

ETC2_RGB = {147: False, 148: True}
ETC2_RGBA = {151: False, 152: True}


def block_bytes(fmt):
    return 16 if fmt in ETC2_RGBA else 8


def levels(width, height, mips):
    for m in range(mips):
        yield m, max(1, width >> m), max(1, height >> m)


def level_bytes(fmt, w, h):
    return ((w + 3) // 4) * ((h + 3) // 4) * block_bytes(fmt)


def decode(fmt, data, w, h):
    # texture2ddecoder works on whole blocks; crop afterwards.
    bw, bh = (w + 3) // 4 * 4, (h + 3) // 4 * 4
    fn = t2d.decode_etc2a8 if fmt in ETC2_RGBA else t2d.decode_etc2
    bgra = np.frombuffer(fn(bytes(data), bw, bh), np.uint8).reshape(bh, bw, 4)[:h, :w]
    return bgra[..., [2, 1, 0, 3]].astype(np.float64)


def srgb_to_linear(c):
    c = c / 255.0
    return np.round(np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4) * 255.0)


def etc_mode(block8):
    """ETC2 RGB mode of an 8-byte color block (first byte first)."""
    b = block8
    if not (b[3] & 2):
        return 'individual'
    r, g, bl = b[0] >> 3, b[1] >> 3, b[2] >> 3
    dr = ((b[0] & 7) ^ 4) - 4
    dg = ((b[1] & 7) ^ 4) - 4
    db = ((b[2] & 7) ^ 4) - 4
    if not 0 <= r + dr <= 31:
        return 'T'
    if not 0 <= g + dg <= 31:
        return 'H'
    if not 0 <= bl + db <= 31:
        return 'planar'
    return 'differential'


def main():
    cmd, path, fmt, width, height, mips = sys.argv[1], sys.argv[2], *map(int, sys.argv[3:7])
    if fmt not in ETC2_RGB and fmt not in ETC2_RGBA:
        sys.exit(f'format {fmt} is not an ETC2 RGB/RGBA8 format')
    total = sum(level_bytes(fmt, w, h) for _, w, h in levels(width, height, mips))
    if cmd == 'gen':
        seed = int(sys.argv[7]) if len(sys.argv) > 7 else 1
        images = int(sys.argv[8]) if len(sys.argv) > 8 else 1
        open(path, 'wb').write(np.random.default_rng(seed).integers(0, 256, total * images, np.uint8).tobytes())
        print(f'{total * images} bytes of random blocks')
        return
    data = open(path, 'rb').read()
    got_all = np.frombuffer(open(sys.argv[7], 'rb').read(), np.uint8)
    srgb = ETC2_RGB.get(fmt, ETC2_RGBA.get(fmt))
    images = int(sys.argv[9]) if len(sys.argv) > 9 else 1
    at = tat = 0
    worst = 0
    for image, (m, w, h) in ((i, lv) for i in range(images) for lv in levels(width, height, mips)):
        n = level_bytes(fmt, w, h)
        ref = decode(fmt, data[at:at + n], w, h)
        if srgb:
            ref[..., :3] = srgb_to_linear(ref[..., :3])
        got = got_all[tat * 4:(tat + w * h) * 4].reshape(h, w, 4).astype(np.float64)
        d = np.abs(got - ref).max(axis=2)
        bad = d > 2
        # Per-block verdict, grouped by the color block's ETC2 mode.
        modes = {}
        bpr = (w + 3) // 4
        color_at = 8 if fmt in ETC2_RGBA else 0
        for by in range((h + 3) // 4):
            for bx in range(bpr):
                k = (by * bpr + bx) * block_bytes(fmt)
                mode = etc_mode(data[at + k + color_at:at + k + color_at + 8])
                wrong = bool(bad[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4].any())
                s = modes.setdefault(mode, [0, 0])
                s[0] += 1
                s[1] += wrong
        summary = ', '.join(f'{k} {v[1]}/{v[0]} wrong' for k, v in sorted(modes.items()))
        print(f'image {image} level {m} {w}x{h}: {100.0 * bad.mean():.2f}% texels off by >2, max diff {int(d.max())}; blocks: {summary}')
        worst = max(worst, int(d.max()))
        if image == 0 and m == 0 and len(sys.argv) > 8 and sys.argv[8] != '-':
            from PIL import Image
            side = np.concatenate([ref, got, np.repeat((bad * 255.0)[..., None], 4, axis=2)], axis=1)
            side[..., 3] = 255
            Image.fromarray(side.astype(np.uint8), 'RGBA').save(sys.argv[8])
        at += n
        tat += w * h
    sys.exit(0 if worst <= 2 else 1)


if __name__ == '__main__':
    main()
