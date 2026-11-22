#!/usr/bin/env python3
"""Mock renderd for ngx_mod_tile end-to-end tests.

Speaks the mod_tile v2/v3 wire protocol over a unix socket.

Options:
  --socket PATH        unix socket to listen on (default /tmp/mock-renderd.sock)
  --log PATH           append received requests here (default stdout+file)
  --delay SECONDS      sleep before answering (simulates render time)
  --mode done|notdone|never   reply mode (never = accept and never reply)
  --render-dir PATH    write a deterministic tile into this metatile store
                       before replying done (simulates a real render)

Protocol (protocol.h): v1 20B {ver,cmd,x,y,z}; v2 61B v1+xmlname[41];
v3 143B {ver,cmd,x,y,z,xmlname[41],mimetype[41],options[41]}.
"""
import argparse
import asyncio
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_metatiles import write_metatile, tile_png  # noqa: E402

CMD_IGNORE, CMD_RENDER, CMD_DIRTY = 0, 1, 2
CMD_DONE, CMD_NOTDONE, CMD_RENDER_PRIO, CMD_RENDER_BULK, CMD_RENDER_LOW = 3, 4, 5, 6, 7

NAMES = {0: "Ignore", 1: "Render", 2: "Dirty", 3: "Done", 4: "NotDone",
         5: "RenderPrio", 6: "RenderBulk", 7: "RenderLow"}


class Proto:
    def __init__(self, raw):
        (self.ver,) = struct.unpack_from("<i", raw, 0)
        if self.ver == 1:
            self.cmd, self.x, self.y, self.z = struct.unpack_from("<iiii", raw, 4)
            self.xmlname, self.mimetype, self.options = "", "", ""
        elif self.ver == 2:
            self.cmd, self.x, self.y, self.z = struct.unpack_from("<iiii", raw, 4)
            self.xmlname = raw[20:61].split(b"\0")[0].decode()
            self.mimetype, self.options = "", ""
        else:  # v3
            self.cmd, self.x, self.y, self.z = struct.unpack_from("<iiii", raw, 4)
            self.xmlname = raw[20:61].split(b"\0")[0].decode()
            self.mimetype = raw[61:102].split(b"\0")[0].decode()
            self.options = raw[102:143].split(b"\0")[0].decode()

    def want(self):
        return {1: 20, 2: 61, 3: 143}[self.ver]

    def reply(self, cmd):
        """Reply with the same version as the request."""
        if self.ver <= 2:
            name = self.xmlname.encode()[:40]
            return struct.pack("<iiiii41s", self.ver, cmd, self.x, self.y,
                               self.z, name)
        return struct.pack("<iiiii41s41s41s", self.ver, cmd, self.x, self.y,
                           self.z, self.xmlname.encode()[:40],
                           self.mimetype.encode()[:40],
                           self.options.encode()[:40])

    def desc(self):
        return ("ver=%d cmd=%s(%d) xml=%s z=%d x=%d y=%d opts=%s"
                % (self.ver, NAMES.get(self.cmd, "?"), self.cmd, self.xmlname,
                   self.z, self.x, self.y, self.options))


def simulate_render(req, render_dir):
    """Write the requested tile as a metatile, like renderd would."""
    z, x, y = req.z, req.x, req.y
    if z < 0 or x < 0 or y < 0:
        return
    ox, oy = (x // 8) * 8, (y // 8) * 8
    payloads = {}
    for dy in range(8):
        for dx in range(8):
            payloads[(dx, dy)] = tile_png(z, ox + dx, oy + dy)
    write_metatile(render_dir, req.xmlname, z, ox, oy, payloads)


async def handle(reader, writer, args):
    logf = args.logf
    try:
        header = await reader.readexactly(4)
        (ver,) = struct.unpack_from("<i", header, 0)
        rest = await reader.readexactly({1: 16, 2: 57, 3: 139}.get(ver, 57))
        req = Proto(header + rest)

        line = "%s %s\n" % (time.strftime("%H:%M:%S"), req.desc())
        logf.write(line)
        logf.flush()

        if args.delay:
            await asyncio.sleep(args.delay)

        if args.mode == "never":
            await writer.drain()
            await asyncio.sleep(3600)
            return

        if args.render_dir and req.cmd in (CMD_RENDER, CMD_RENDER_PRIO,
                                           CMD_RENDER_BULK, CMD_RENDER_LOW):
            simulate_render(req, args.render_dir)

        reply_cmd = CMD_DONE if args.mode == "done" else CMD_NOTDONE
        writer.write(req.reply(reply_cmd))
        await writer.drain()
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def main_async(args):
    if os.path.exists(args.socket):
        os.unlink(args.socket)
    server = await asyncio.start_unix_server(
        lambda r, w: handle(r, w, args), path=args.socket)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--socket", default="/tmp/mock-renderd.sock")
    ap.add_argument("--log", default="/tmp/mock-renderd.log")
    ap.add_argument("--delay", type=float, default=0.0)
    ap.add_argument("--mode", choices=["done", "notdone", "never"], default="done")
    ap.add_argument("--render-dir", default=None)
    args = ap.parse_args()

    args.logf = open(args.log, "a")
    asyncio.run(main_async(args))
