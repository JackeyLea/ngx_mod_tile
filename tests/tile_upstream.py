#!/usr/bin/env python3
"""Minimal read-only HTTP tile upstream for integration tests."""
import http.server
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_metatiles import tile_png

class TileHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        # expected URI: /<z>/<x>/<y>.png
        parts = self.path.strip('/').split('/')
        if len(parts) == 3 and parts[2].endswith('.png'):
            try:
                z, x = int(parts[0]), int(parts[1])
                y = int(parts[2][:-4])
                data = tile_png(z, x, y)
                self.send_response(200)
                self.send_header('Content-Type', 'image/png')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            except ValueError:
                pass
        self.send_response(404)
        self.end_headers()

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    server = http.server.ThreadingHTTPServer(('0.0.0.0', port), TileHandler)
    print("Tile upstream listening on port %d" % port, flush=True)
    server.serve_forever()
