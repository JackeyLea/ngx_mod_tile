#!/bin/sh
# renderd container entrypoint.
# Roles are selected with RENDERD_ROLE:
#   import  - one-shot: wait for the database, apply carto DSN, run
#             indexes/functions SQL, osm2pgsql import, external data
#   render  - run renderd with the compiled style
set -e

ROLE="${RENDERD_ROLE:-render}"
DB_HOST="${DB_HOST:-db}"
DB_PORT="${DB_PORT:-5432}"
DB_USER="${DB_USER:-gis}"
DB_PASSWORD="${DB_PASSWORD:-gispass}"
DB_NAME="${DB_NAME:-gis}"
MAPS_DIR="${MAPS_DIR:-/maps}"
CARTO_DIR=/opt/osm-carto

export PGHOST="$DB_HOST" PGPORT="$DB_PORT" PGUSER="$DB_USER" \
       PGPASSWORD="$DB_PASSWORD" PGDATABASE="$DB_NAME"

# Mapnik input plugin path differs across mapnik versions: detect it.
# The conf may be a read-only bind mount, so work on a writable copy.
cp /etc/renderd/renderd.conf /tmp/renderd.conf
RENDERD_CONF=/tmp/renderd.conf

PLUGDIR=$(find /usr/lib -type d -path "*mapnik*input" 2>/dev/null | head -1)
if [ -n "$PLUGDIR" ]; then
    sed -i "s|^plugins_dir=.*|plugins_dir=$PLUGDIR|" "$RENDERD_CONF"
fi

# make the style fonts visible to mapnik/renderd (get-fonts.py downloads them
# into the style directory; fall back to system fonts)
mkdir -p /usr/share/fonts/truetype/osmcarto
if [ -d "$CARTO_DIR/fonts" ]; then
    ln -sf "$CARTO_DIR"/fonts/*.ttf /usr/share/fonts/truetype/osmcarto/ 2>/dev/null || true
fi

if [ "$ROLE" = "import" ]; then
    echo "[import] waiting for the database..."
    until psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" -c "SELECT 1" >/dev/null 2>&1; do
        sleep 2
    done
    echo "[import] database is up"

    cd "$CARTO_DIR"

    # runtime DSN into project.mml (CartoCSS YAML), then re-compile
    python3 - <<PYEOF
import yaml

with open("project.mml") as f:
    mml = yaml.safe_load(f)

for layer in mml.get("Layer", []):
    ds = layer.get("Datasource", {})
    if ds.get("type") == "postgis":
        ds["dbname"] = "__DB_NAME__"
        ds["host"] = "__DB_HOST__"
        ds["port"] = "__DB_PORT__"
        ds["user"] = "__DB_USER__"
        ds["password"] = "__DB_PASSWORD__"

with open("project.mml", "w") as f:
    yaml.safe_dump(mml, f, default_flow_style=False)
print("[import] project.mml DSN updated")
PYEOF

    sed -i "s/__DB_NAME__/$DB_NAME/; s/__DB_HOST__/$DB_HOST/; s/__DB_PORT__/$DB_PORT/; s/__DB_USER__/$DB_USER/; s/__DB_PASSWORD__/$DB_PASSWORD/" project.mml

    carto project.mml > mapnik.xml
    echo "[import] mapnik.xml compiled"


    psql -c "CREATE EXTENSION IF NOT EXISTS postgis" || true
    psql -c "CREATE EXTENSION IF NOT EXISTS hstore" || true

    PBF=$(ls "$MAPS_DIR"/*.pbf 2>/dev/null | head -1)
    if [ -z "$PBF" ]; then
        echo "[import] ERROR: no .pbf file found in $MAPS_DIR" >&2
        echo "[import] run maps/download-demo-map.sh on the host first" >&2
        exit 1
    fi
    echo "[import] importing $PBF with osm2pgsql"
    osm2pgsql --create --slim --drop --hstore \
        --database "$DB_NAME" \
        --style "$CARTO_DIR/openstreetmap-carto.style" \
        --tag-transform-script "$CARTO_DIR/openstreetmap-carto.lua" \
        --number-processes 2 \
        "$PBF"

    echo "[import] applying indexes.sql and functions.sql"
    psql -f indexes.sql || echo "[import] indexes.sql reported errors (may already exist)"
    psql -f functions.sql || echo "[import] functions.sql reported errors"

    if [ "${IMPORT_EXTERNAL:-yes}" = "skip" ]; then
        echo "[import] skipping external data; removing external layers from mapnik.xml"
        python3 - <<'PYEOF'
import xml.etree.ElementTree as ET

path = "/opt/osm-carto/mapnik.xml"
doc = ET.parse(path)
root = doc.getroot()
drop = {"icesheet-poly", "ocean-lz", "ocean", "icesheet-outlines", "necountries"}
removed = []
for layer in list(root):
    if layer.tag == "Layer" and layer.get("name") in drop:
        root.remove(layer)
        removed.append(layer.get("name"))
doc.write(path, xml_declaration=True, encoding="UTF-8")
print("[import] removed external layers:", ",".join(removed))
PYEOF
    else
        echo "[import] fetching external data (land/water/borders)"
        python3 scripts/get-external-data.py || \
            echo "[import] WARNING: get-external-data.py failed; sea areas will be blank"
    fi

    echo "[import] done"
    exit 0
fi

# render role: drop external-data layers whose tables are not present.
# This makes renderd work both with a full import (land/ocean/icesheet layers
# kept) and with the IMPORT_EXTERNAL=skip fast path (they are removed).
python3 - <<'PYEOF'
import xml.etree.ElementTree as ET
import psycopg2
import os

tables = {
    "icesheet-poly": "icesheet_polygons",
    "icesheet-outlines": "icesheet_outlines",
    "ocean": "water_polygons",
    "ocean-lz": "simplified_water_polygons",
    "necountries": "ne_110m_admin_0_boundary_lines_land",
}
conn = psycopg2.connect(
    host=os.environ.get("DB_HOST", "db"),
    port=os.environ.get("DB_PORT", "5432"),
    dbname=os.environ.get("DB_NAME", "gis"),
    user=os.environ.get("DB_USER", "gis"),
    password=os.environ.get("DB_PASSWORD", "gispass"))
cur = conn.cursor()
missing = set()
for layer, tbl in tables.items():
    cur.execute(
        "SELECT EXISTS(SELECT 1 FROM information_schema.tables "
        "WHERE table_schema='public' AND table_name=%s)", (tbl,))
    if not cur.fetchone()[0]:
        missing.add(layer)
conn.close()

if missing:
    path = "/opt/osm-carto/mapnik.xml"
    doc = ET.parse(path)
    root = doc.getroot()
    for layer in list(root):
        if layer.tag == "Layer" and layer.get("name") in missing:
            root.remove(layer)
    doc.write(path, xml_declaration=True, encoding="UTF-8")
    print("[renderd] removed layers with missing external tables:",
          ",".join(sorted(missing)), flush=True)
else:
    print("[renderd] external tables present, full style kept", flush=True)
PYEOF

echo "[renderd] starting renderd with $RENDERD_CONF"
exec renderd -f -c "$RENDERD_CONF"
