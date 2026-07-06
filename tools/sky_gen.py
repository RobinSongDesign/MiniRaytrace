#!/usr/bin/env python3
"""Generate a simple procedural sky HDRI (Radiance .hdr, flat RGBE encoding).

Gradient sky + ground + sun disk with glow. Output is equirectangular,
directly loadable by mrt_viewer's --env option (decoded by stb_image).

Usage: python3 tools/sky_gen.py [output.hdr] [width]
"""
import math
import struct
import sys


def rgbe(r: float, g: float, b: float) -> bytes:
    """Encode linear RGB to 4-byte RGBE."""
    m = max(r, g, b)
    if m < 1e-32:
        return b"\x00\x00\x00\x00"
    frac, exp = math.frexp(m)          # m = frac * 2^exp, 0.5 <= frac < 1
    scale = frac * 256.0 / m
    return struct.pack(
        "4B",
        min(255, int(r * scale)),
        min(255, int(g * scale)),
        min(255, int(b * scale)),
        exp + 128,
    )


def sky(dx: float, dy: float, dz: float) -> tuple:
    """Sky radiance for a unit direction (y up)."""
    # Sun towards (0.4, 0.6, 0.3)
    sl = math.sqrt(0.4 ** 2 + 0.6 ** 2 + 0.3 ** 2)
    sx, sy, sz = 0.4 / sl, 0.6 / sl, 0.3 / sl
    cos_sun = dx * sx + dy * sy + dz * sz

    if dy < 0.0:                      # ground: neutral grey bounce
        f = 1.0 + dy                  # darker towards nadir
        return (0.28 * f + 0.05, 0.26 * f + 0.05, 0.24 * f + 0.05)

    # Horizon-to-zenith gradient
    t = dy ** 0.55
    r = 0.95 * (1 - t) + 0.22 * t
    g = 0.97 * (1 - t) + 0.42 * t
    b = 1.00 * (1 - t) + 0.85 * t

    # Sun disk (~0.0465 rad) + soft glow
    if cos_sun > 0.0:
        ang = math.acos(min(1.0, cos_sun))
        if ang < 0.0465:
            return (1500.0, 1400.0, 1250.0)
        glow = math.exp(-ang * 14.0) * 3.0
        r, g, b = r + glow, g + glow * 0.9, b + glow * 0.7
    return (r, g, b)


def main() -> None:
    out = sys.argv[1] if len(sys.argv) > 1 else "sky.hdr"
    width = int(sys.argv[2]) if len(sys.argv) > 2 else 1024
    height = width // 2

    with open(out, "wb") as f:
        f.write(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n")
        f.write(f"-Y {height} +X {width}\n".encode())
        for y in range(height):
            theta = (y + 0.5) / height * math.pi
            st, ct = math.sin(theta), math.cos(theta)
            row = bytearray()
            for x in range(width):
                phi = (x + 0.5) / width * 2.0 * math.pi
                d = (st * math.cos(phi), ct, st * math.sin(phi))
                row += rgbe(*sky(*d))
            f.write(row)
    print(f"wrote {out} ({width}x{height})")


if __name__ == "__main__":
    main()
