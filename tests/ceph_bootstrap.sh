#!/bin/bash
# Bootstrap a minimal single-node, single-OSD Ceph cluster for RADOS tests.
set -eu

FSID=4f3b8e2d-70c2-4c5d-a3c4-2c82a2d9e501
IP=$(hostname -i | awk '{print $1}')
MON_DATA=/var/lib/ceph/mon/ceph-ceph
MGR_DATA=/var/lib/ceph/mgr/ceph-ceph
OSD_DATA=/var/lib/ceph/osd/ceph-0

mkdir -p /etc/ceph "$MON_DATA" "$MGR_DATA" "$OSD_DATA"

cat > /etc/ceph/ceph.conf <<EOF
[global]
fsid = $FSID
mon initial members = ceph
mon host = $IP
public network = 0.0.0.0/0
auth cluster required = none
auth service required = none
auth client required = none
osd pool default size = 1
osd pool default min size = 1
mon allow pool size one = true
osd crush chooseleaf type = 0

[mon.ceph]
host = ceph
mon addr = $IP:6789
mon data = $MON_DATA

[mgr.ceph]
mgr data = $MGR_DATA

[osd.0]
osd data = $OSD_DATA
bluestore block path = /var/lib/ceph/osd-block-0
bluestore block size = 1073741824
EOF

ceph-mon --mkfs -i ceph --mon-data "$MON_DATA" --fsid "$FSID" \
    --public-addr "$IP:6789"
ceph-mon -i ceph --mon-data "$MON_DATA" --public-addr "$IP:6789" \
    --foreground > /tmp/ceph-mon.log 2>&1 &

for i in $(seq 1 60); do
    if ceph -s >/tmp/ceph-status.log 2>&1; then
        break
    fi
    sleep 1
done
ceph -s

ceph-mgr -i ceph --mgr-data "$MGR_DATA" --foreground \
    > /tmp/ceph-mgr.log 2>&1 &

OSD_ID=$(ceph osd create)
test "$OSD_ID" = "0"
ceph mon getmap -o /tmp/ceph-monmap
ceph-osd --mkfs -i 0 --osd-data "$OSD_DATA" --monmap /tmp/ceph-monmap \
    --osd-uuid 4f3b8e2d-70c2-4c5d-a3c4-2c82a2d9e502
ceph-osd -i 0 --osd-data "$OSD_DATA" --foreground \
    > /tmp/ceph-osd.log 2>&1 &

for i in $(seq 1 60); do
    if ceph osd stat | grep -q '1 osds: 1 up'; then
        break
    fi
    sleep 1
done

ceph osd pool create tiles 8 8
ceph osd pool get tiles size
ceph osd pool get tiles min_size
ceph -s
touch /tmp/ceph-ready
echo "Ceph test cluster ready; RADOS pool: tiles"

wait
