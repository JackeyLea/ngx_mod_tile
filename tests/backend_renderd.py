#!/usr/bin/env python3
"""TCP renderd test double that stores rendered metatiles in memcached."""
import argparse
import asyncio
import socket
import struct
import sys
import time

sys.path.insert(0, '/opt/demo')
from demo_renderd import Proto, tile_png

CMD_DONE = 3
RENDER_COMMANDS = (1, 5, 6, 7)


def build_metatile(req):
    ox, oy = (req.x // 8) * 8, (req.y // 8) * 8
    header_len = 20 + 64 * 8
    index = []
    offset = header_len
    body = b''
    for dy in range(8):
        for dx in range(8):
            data = tile_png(req.z, ox + dx, oy + dy,
                            style=req.options or None)
            index.append((offset, len(data)))
            body += data
            offset += len(data)
    out = b'META' + struct.pack('<iiii', 64, ox, oy, req.z)
    for off, size in index:
        out += struct.pack('<ii', off, size)
    return out + body, ox, oy


def store_memcached(host, port, req):
    payload, ox, oy = build_metatile(req)
    now = int(time.time())
    # struct stat_info: off_t + 3*time_t + int + ABI tail padding
    stat_info = struct.pack('<qqqqi4x', len(payload), now, now, now, 0)
    suffix = '.' + req.options if req.options else ''
    key = '%s/%d/%d/%d%s.meta' % (req.xmlname, ox, oy, req.z, suffix)
    blob = stat_info + payload
    command = (b'set ' + key.encode() + b' 0 0 '
               + str(len(blob)).encode() + b'\r\n' + blob + b'\r\n')
    with socket.create_connection((host, port), timeout=3) as sock:
        sock.sendall(command)
        if not sock.recv(1024).startswith(b'STORED'):
            raise RuntimeError('memcached rejected %s' % key)


async def handle(reader, writer, args):
    try:
        header = await reader.readexactly(4)
        (ver,) = struct.unpack_from('<i', header, 0)
        size = {1: 20, 2: 61, 3: 143}.get(ver)
        if size is None:
            return
        req = Proto(header + await reader.readexactly(size - 4))
        print('%s %s' % (time.strftime('%H:%M:%S'), req.desc()), flush=True)
        if req.cmd in RENDER_COMMANDS:
            await asyncio.to_thread(store_memcached,
                                    args.memcached_host,
                                    args.memcached_port, req)
        writer.write(req.reply(CMD_DONE))
        await writer.drain()
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        writer.close()
        await writer.wait_closed()


async def main(args):
    server = await asyncio.start_server(
        lambda r, w: handle(r, w, args), host=args.host, port=args.port)
    print('backend renderd listening on %s:%d; memcached=%s:%d'
          % (args.host, args.port, args.memcached_host, args.memcached_port),
          flush=True)
    async with server:
        await server.serve_forever()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=7654)
    parser.add_argument('--memcached-host', default='memcached')
    parser.add_argument('--memcached-port', type=int, default=11211)
    asyncio.run(main(parser.parse_args()))
