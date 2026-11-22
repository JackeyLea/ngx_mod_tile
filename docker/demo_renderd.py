#!/usr/bin/env python3
"""Demo renderd for the ngx_mod_tile docker stack.

Speaks the mod_tile v1/v2/v3 wire protocol over TCP and renders
deterministic placeholder PNG tiles into the shared tile store, so the
on-demand rendering loop is fully demoable without Mapnik. Point
tile_renderd_socket_addr at a real renderd for production use.
"""
import argparse
import asyncio
import os
import struct
import sys
import time
import zlib

METATILE = 8

CMD_DONE, CMD_NOTDONE = 3, 4
NAMES = {0: "Ignore", 1: "Render", 2: "Dirty", 3: "Done", 4: "NotDone",
         5: "RenderPrio", 6: "RenderBulk", 7: "RenderLow"}


def tile_png(z, x, y, style=None):
    """Deterministic 1x1 PNG whose RGB encodes z/x/y (same scheme as the
    test generator, so volumes are cross-compatible)."""
    def chunk(ctype, data):
        c = ctype + data
        return (struct.pack(">I", len(data)) + c
                + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF))

    if style:
        x = (x + 7 * len(style)) & 0x7FFFFFFF
    r = (z * 17 + x * 3) % 256
    g = (x * 7 + y * 11) % 256
    b = (y * 13 + z * 5) % 256
    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
    idat = zlib.compress(bytes([0, r, g, b]))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", idat) + chunk(b"IEND", b""))


def meta_path(base, xml, z, ox, oy, options=None):
    """mod_tile store path: decimal hash dirs, <h0>.<options>.meta variants."""
    x, y = ox, oy
    hash_bits = []
    for _ in range(5):
        hash_bits.append(((x & 0x0F) << 4) | (y & 0x0F))
        x >>= 4
        y >>= 4
    name = ("%d.%s.meta" % (hash_bits[0], options)) if options \
        else ("%d.meta" % hash_bits[0])
    return os.path.join(base, xml, str(z),
                        "%d" % hash_bits[4], "%d" % hash_bits[3],
                        "%d" % hash_bits[2], "%d" % hash_bits[1], name)


def write_metatile(base, xml, z, ox, oy, payloads, options=None):
    path = meta_path(base, xml, z, ox, oy, options)
    os.makedirs(os.path.dirname(path), exist_ok=True)

    header_len = 20 + 64 * 8
    index, body, offset = [], b"", header_len
    for dy in range(METATILE):
        for dx in range(METATILE):
            data = payloads.get((dx, dy), b"")
            index.append((offset, len(data)))
            body += data
            offset += len(data)

    out = b"META" + struct.pack("<iiii", 64, ox, oy, z)
    for off, sz in index:
        out += struct.pack("<ii", off, sz)
    out += body

    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)


def simulate_render(req, tile_dir):
    z, x, y = req[2], req[3], req[4]
    if z < 0 or x < 0 or y < 0:
        return
    ox, oy = (x // METATILE) * METATILE, (y // METATILE) * METATILE
    payloads = {}
    for dy in range(METATILE):
        for dx in range(METATILE):
            payloads[(dx, dy)] = tile_png(z, ox + dx, oy + dy,
                                          style=req[6] or None)
    write_metatile(tile_dir, req[5], z, ox, oy, payloads,
                   options=req[6] or None)


class Proto:
    def __init__(self, raw):
        (self.ver,) = struct.unpack_from("<i", raw, 0)
        if self.ver <= 1:
            self.cmd, self.x, self.y, self.z = struct.unpack_from("<iiii", raw, 4)
            self.xmlname, self.mimetype, self.options = "default", "", ""
        elif self.ver == 2:
            self.cmd, self.x, self.y, self.z = struct.unpack_from("<iiii", raw, 4)
            self.xmlname = raw[20:61].split(b"\0")[0].decode()
            self.mimetype, self.options = "", ""
        else:
            self.cmd, self.x, self.y, self.z = struct.unpack_from("<iiii", raw, 4)
            self.xmlname = raw[20:61].split(b"\0")[0].decode()
            self.mimetype = raw[61:102].split(b"\0")[0].decode()
            self.options = raw[102:143].split(b"\0")[0].decode()

    def reply(self, cmd):
        if self.ver <= 2:
            return struct.pack("<iiiii41s", self.ver, cmd, self.x, self.y,
                               self.z, self.xmlname.encode()[:40])
        return struct.pack("<iiiii41s41s41s", self.ver, cmd, self.x, self.y,
                           self.z, self.xmlname.encode()[:40],
                           self.mimetype.encode()[:40],
                           self.options.encode()[:40])

    def desc(self):
        return "ver=%d cmd=%s(%d) xml=%s z=%d x=%d y=%d opts=%s" % (
            self.ver, NAMES.get(self.cmd, "?"), self.cmd, self.xmlname,
            self.z, self.x, self.y, self.options)


async def handle(reader, writer, args):
    try:
        header = await reader.readexactly(4)
        (ver,) = struct.unpack_from("<i", header, 0)
        want = {1: 16, 2: 57, 3: 139}.get(ver)
        if want is None:
            return
        raw = header + await reader.readexactly(want - 4)

        if ver <= 1:
            cmd, x, y, z = struct.unpack_from("<iiii", raw, 4)
            xml, mimetype, options = "default", "", ""
        elif ver == 2:
            cmd, x, y, z = struct.unpack_from("<iiii", raw, 4)
            xml = raw[20:61].split(b"\0")[0].decode()
            mimetype, options = "", ""
        else:
            cmd, x, y, z = struct.unpack_from("<iiii", raw, 4)
            xml = raw[20:61].split(b"\0")[0].decode()
            mimetype = raw[61:102].split(b"\0")[0].decode()
            options = raw[102:143].split(b"\0")[0].decode()

        args.logf.write("%s ver=%d cmd=%s(%d) xml=%s z=%d x=%d y=%d opts=%s\n"
                        % (time.strftime("%H:%M:%S"), ver, NAMES.get(cmd, "?"),
                           cmd, xml, z, x, y, options))
        args.logf.flush()

        if args.delay:
            await asyncio.sleep(args.delay)

        if cmd in (1, 5, 6, 7):  # render flavours
            simulate_render((ver, cmd, z, x, y, xml, options), args.tile_dir)

        if ver <= 2:
            reply = struct.pack("<iiiii41s", ver, CMD_DONE, x, y, z,
                                xml.encode()[:40])
        else:
            reply = struct.pack("<iiiii41s41s41s", ver, CMD_DONE, x, y, z,
                                xml.encode()[:40], mimetype.encode()[:40],
                                options.encode()[:40])
        writer.write(reply)
        await writer.drain()
        args.logf.write("  sent %d byte reply (cmd=%d)\n" % (len(reply), reply[4]))
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def main_async(args):
    server = await asyncio.start_server(
        lambda r, w: handle(r, w, args), host=args.host, port=args.port)
    print("demo renderd listening on %s:%d, tile dir %s"
          % (args.host, args.port, args.tile_dir), flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=7654)
    ap.add_argument("--tile-dir", default="/data/tiles")
    ap.add_argument("--log", default="/var/log/demo-renderd.log")
    ap.add_argument("--delay", type=float, default=0.0)
    args = ap.parse_args()

    args.logf = open(args.log, "a")
    asyncio.run(main_async(args))
