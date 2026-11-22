#!/usr/bin/env python3
"""Generate a mod_tile-compatible metatile store for tests.

Layout follows upstream mod_tile (store_file_utils.c xyzo_to_meta and
metatile.h struct meta_layout):
  path  <dir>/<xml>/<z>/<h4>/<h3>/<h2>/<h1>/<h0>.meta[.gzip]
  hash[i] = ((x & 0xf) << 4) | (y & 0xf)   over the metatile origin
  header: magic(4) count(4) x(4) y(4) z(4) + index[64]{offset(4) size(4)}
  payload order: column-major dx*8+dy, absolute offsets from file start
"""
import gzip
import os
import struct
import sys
import time
import zlib

METATILE = 8


def tile_png(z, x, y):
    """Minimal deterministic 1x1 PNG whose RGB encodes z/x/y (stable bytes)."""
    def chunk(ctype, data):
        c = ctype + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    r = (z * 17 + x * 3) % 256
    g = (x * 7 + y * 11) % 256
    b = (y * 13 + z * 5) % 256
    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
    idat = zlib.compress(bytes([0, r, g, b]))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", idat) + chunk(b"IEND", b""))


def meta_path(base, xml, z, ox, oy, gz=False):
    """ox/oy are the metatile ORIGIN tile coordinates (multiples of 8)."""
    x, y = ox, oy
    hash_bits = []
    for _ in range(5):
        hash_bits.append(((x & 0x0F) << 4) | (y & 0x0F))
        x >>= 4
        y >>= 4
    # upstream xyzo_to_meta prints the hash bytes in DECIMAL (%u)
    name = "%d.meta" % hash_bits[0]
    return os.path.join(base, xml, str(z),
                        "%d" % hash_bits[4], "%d" % hash_bits[3],
                        "%d" % hash_bits[2], "%d" % hash_bits[1],
                        name)


def write_metatile(base, xml, z, mx, my, payloads, gz=False):
    """payloads: dict {(dx, dy): bytes}"""
    path = meta_path(base, xml, z, mx, my, gz)
    os.makedirs(os.path.dirname(path), exist_ok=True)

    header_len = 20 + 64 * 8
    index = []
    offset = header_len
    body = b""
    for dy in range(METATILE):
        for dx in range(METATILE):
            data = payloads.get((dx, dy), b"")
            index.append((offset, len(data)))
            body += data
            offset += len(data)

    mxo, myo = mx & ~(METATILE - 1), my & ~(METATILE - 1)
    # native (little-endian) layout: upstream writes raw C structs on x86
    out = b"METZ" if gz else b"META"
    out += struct.pack("<iiii", 64, mxo, myo, z)
    for off, sz in index:
        out += struct.pack("<ii", off, sz)
    out += body

    with open(path, "wb") as f:
        f.write(out)


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else "/tmp/tilestore"
    os.makedirs(os.path.join(base, "test"), exist_ok=True)
    os.makedirs(os.path.join(base, "osmtest"), exist_ok=True)

    # planet-import-complete: present and recent (tiles older than it expire)
    with open(os.path.join(base, "test", "planet-import-complete"), "w"):
        pass
    os.utime(os.path.join(base, "test", "planet-import-complete"), None)

    now = time.time()

    # --- fresh tiles (META) for z0..z6 ---
    for z in range(0, 7):
        for mx in range(0, 4):
            for my in range(0, 4):
                payloads = {}
                for dy in range(METATILE):
                    for dx in range(METATILE):
                        payloads[(dx, dy)] = tile_png(z, mx * 8 + dx, my * 8 + dy)
                write_metatile(base, "test", z, mx * 8, my * 8, payloads)

    # --- compressed (METZ) metatile at origin (64,64) ---
    payloads = {}
    for dy in range(METATILE):
        for dx in range(METATILE):
            payloads[(dx, dy)] = gzip.compress(tile_png(7, 64 + dx, 64 + dy))
    write_metatile(base, "test", 7, 64, 64, payloads, gz=True)

    # --- old tile: expired 100 days ago (older than planet time, <365d) ---
    payloads = {(dx, dy): tile_png(8, 64 + dx, 64 + dy) for dy in range(8) for dx in range(8)}
    write_metatile(base, "test", 8, 64, 64, payloads)
    os.utime(meta_path(base, "test", 8, 64, 64), (now - 100 * 86400, now - 100 * 86400))

    # --- very old tile: 400 days ago ---
    payloads = {(dx, dy): tile_png(9, 64 + dx, 64 + dy) for dy in range(8) for dx in range(8)}
    write_metatile(base, "test", 9, 64, 64, payloads)
    os.utime(meta_path(base, "test", 9, 64, 64), (now - 400 * 86400, now - 400 * 86400))

    # --- second layer for the renderd.conf import test ---
    with open(os.path.join(base, "osmtest", "planet-import-complete"), "w"):
        pass
    for z in range(0, 3):
        for mx in range(0, 2):
            for my in range(0, 2):
                payloads = {}
                for dy in range(METATILE):
                    for dx in range(METATILE):
                        payloads[(dx, dy)] = tile_png(z, mx * 8 + dx, my * 8 + dy)
                write_metatile(base, "osmtest", z, mx * 8, my * 8, payloads)

    print("tile store generated at %s" % base)


if __name__ == "__main__":
    main()
