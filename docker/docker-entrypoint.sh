#!/bin/sh
set -e

TILE_DIR="${TILE_DIR:-/data/tiles}"
DEMO_RENDERD="${DEMO_RENDERD:-internal}"
GENERATE_DEMO_STORE="${GENERATE_DEMO_STORE:-yes}"

mkdir -p "$TILE_DIR" /var/log/nginx /var/cache/nginx

# generate the demo store once (volume may be fresh); the real Mapnik stack
# disables this so placeholder tiles cannot shadow real rendered tiles
if [ "$GENERATE_DEMO_STORE" = "yes" ] && [ ! -f "$TILE_DIR/.demo-generated" ]; then
    python3 /opt/demo/gen_metatiles.py "$TILE_DIR" default osmtest
    touch "$TILE_DIR/.demo-generated"
fi

if [ "$DEMO_RENDERD" = "internal" ]; then
    # single-container mode: run the demo renderer here and point the
    # "renderd" hostname at loopback
    python3 /opt/demo/demo_renderd.py \
        --host 127.0.0.1 --port 7654 --tile-dir "$TILE_DIR" &
    grep -qE '^\s*127\.0\.0\.1\s+renderd' /etc/hosts \
        || echo "127.0.0.1 renderd" >> /etc/hosts
fi

NGINX_CONF="${NGINX_CONF:-/etc/nginx/nginx.conf}"

exec /usr/local/nginx/sbin/nginx -g 'daemon off;' -c "$NGINX_CONF"
