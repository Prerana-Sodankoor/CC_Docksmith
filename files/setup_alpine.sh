#!/bin/bash
set -e

echo "[Setup] Downloading real Alpine 3.18 rootfs..."
cd /tmp
wget -q https://dl-cdn.alpinelinux.org/alpine/v3.18/releases/x86_64/alpine-minirootfs-3.18.4-x86_64.tar.gz -O alpine.tar.gz
gunzip -f alpine.tar.gz

LAYER_DIGEST=$(sha256sum alpine.tar | awk '{print $1}')
echo "[Setup] Alpine tar digest: $LAYER_DIGEST"

mkdir -p ~/.docksmith/layers ~/.docksmith/images
mv alpine.tar ~/.docksmith/layers/$LAYER_DIGEST.tar

cat <<EOF > ~/.docksmith/images/alpine_3.18.json
{
  "name": "alpine",
  "tag": "3.18",
  "digest": "sha256:abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234",
  "created": "2023-01-01T00:00:00Z",
  "layers": [
    {
      "digest": "sha256:$LAYER_DIGEST",
      "size": 10240,
      "createdBy": "real alpine base layer"
    }
  ]
}
EOF
echo "[Setup] Done writing real Alpine image!"
