#!/bin/sh
# Download a lightweight OSM extract for testing into maps/.
#
#   ./maps/download-demo-map.sh [city]
#
# Default city is Baku (~12 MB from BBBike; reachable from CN networks, unlike
# download.geofabrik.de). Set OSM_PBF_URL to override the source entirely,
# e.g. a Geofabrik extract:
#   OSM_PBF_URL="https://download.geofabrik.de/asia/azerbaijan-latest.osm.pbf" \
#       ./maps/download-demo-map.sh azerbaijan

set -e
cd "$(dirname "$0")"

CITY="${1:-Baku}"
OUT="${CITY}.osm.pbf"
URL="${OSM_PBF_URL:-https://download.bbbike.org/osm/bbbike/${CITY}/${CITY}.osm.pbf}"

if [ -s "$OUT" ] && [ "$(stat -c%s "$OUT")" -gt 1000000 ]; then
    echo "$OUT already present ($(stat -c%s "$OUT") bytes), skipping download"
    exit 0
fi

echo "downloading $URL"
curl -fL -C - --retry 5 --connect-timeout 15 -o "$OUT" "$URL" --silent --show-error
echo "done: $(ls -l "$OUT")"
