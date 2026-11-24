#!/usr/bin/env python3
"""Seed metatiles into the Ceph RADOS test pool using the rados CLI."""
import os
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, '/opt/tests')
from gen_metatiles import tile_png


def build_metatile(z, mx, my):
    header_len = 20 + 64 * 8
    index = []
    offset = header_len
    body = b''
    for dy in range(8):
        for dx in range(8):
            data = tile_png(z, mx + dx, my + dy)
            index.append((offset, len(data)))
            body += data
            offset += len(data)
    out = b'META' + struct.pack('<iiii', 64, mx, my, z)
    for off, size in index:
        out += struct.pack('<ii', off, size)
    return out + body


def main():
    now = int(time.time())
    for z in range(0, 3):
        for mx in range(0, 2):
            for my in range(0, 2):
                payload = build_metatile(z, mx * 8, my * 8)
                stat_info = struct.pack('<qqqqi4x', len(payload),
                                        now, now, now, 0)
                key = 'default/%d/%d/%d.meta' % (z, mx * 8, my * 8)
                with tempfile.NamedTemporaryFile() as tmp:
                    tmp.write(stat_info + payload)
                    tmp.flush()
                    subprocess.run(['rados', '-p', 'tiles', 'put', key,
                                    tmp.name], check=True)
    print('Successfully seeded metatiles into RADOS pool tiles')


if __name__ == '__main__':
    main()
