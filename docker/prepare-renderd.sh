#!/bin/sh
# Prepare the openstreetmap-carto style on the HOST before building the
# renderd image (the container cannot always reach github directly).
#
#   ./docker/prepare-renderd.sh
#
# Env overrides:
#   OSCARTO_VERSION=v5.9.0
#   GH_PROXY=https://ghfast.top/   (github accelerator, empty = direct)

set -e
cd "$(dirname "$0")/.."

OSCARTO_VERSION="${OSCARTO_VERSION:-v5.9.0}"
GH_PROXY="${GH_PROXY:-}"
DIR=".osm-carto"

if [ -d "$DIR/.git" ]; then
    echo "$DIR already cloned, skipping (delete it to re-clone)"
    exit 0
fi

echo "cloning openstreetmap-carto $OSCARTO_VERSION"
git clone --depth 1 --branch "$OSCARTO_VERSION" \
    "${GH_PROXY}https://github.com/gravitystorm/openstreetmap-carto.git" "$DIR"
echo "done: $(ls $DIR | head -5 | tr '\n' ' ')..."

# vendored Leaflet for the tileserver image (downloaded on the host; the
# container's curl has TLS issues with the npm mirror)
if [ ! -f docker/leaflet.tgz ]; then
    echo "downloading leaflet dist (npmmirror)"
    curl -fsSL https://registry.npmmirror.com/leaflet/-/leaflet-1.9.4.tgz \
        -o docker/leaflet.tgz
fi
