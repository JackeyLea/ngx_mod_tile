#!/usr/bin/env python3
"""Seed metatiles into a memcached server for ngx_mod_tile integration tests."""
import socket
import struct
import sys
import time

sys.path.insert(0, '/opt/tests')
from gen_metatiles import tile_png

def build_metatile(z, mx, my):
    header_len = 20 + 64 * 8
    index = []
    offset = header_len
    body = b""
    for dy in range(8):
        for dx in range(8):
            data = tile_png(z, mx + dx, my + dy)
            index.append((offset, len(data)))
            body += data
            offset += len(data)
    out = b"META" + struct.pack("<iiii", 64, mx, my, z)
    for off, sz in index:
        out += struct.pack("<ii", off, sz)
    return out + body

def mc_set(sock, key, data):
    cmd = b"set " + key.encode() + b" 0 0 " + str(len(data)).encode() + b"\r\n" + data + b"\r\n"
    sock.sendall(cmd)
    resp = sock.recv(1024)
    return resp.startswith(b"STORED")

def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "memcached"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 11211
    xml = "default"

    # struct stat_info: off_t(8) + time_t(8) + time_t(8) + time_t(8) + int(4) + pad(4)
    now = int(time.time())

    for z in range(0, 4):
        for mx in range(0, 4):
            for my in range(0, 4):
                payload = build_metatile(z, mx * 8, my * 8)
                stat_info = struct.pack("<qqqqi4x", len(payload), now, now, now, 0)
                blob = stat_info + payload
                key = "%s/%d/%d/%d.meta" % (xml, mx * 8, my * 8, z)
                s = socket.create_connection((host, port))
                ok = mc_set(s, key, blob)
                s.close()
                if not ok:
                    print("FAILED to seed %s" % key)
                    sys.exit(1)
    print("Successfully seeded metatiles into memcached at %s:%d" % (host, port))

if __name__ == "__main__":
    main()
