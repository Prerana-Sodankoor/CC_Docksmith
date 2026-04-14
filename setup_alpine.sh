#!/bin/bash
set -e

# Dynamically find the correct .docksmith folder, even if run with sudo
DOCKSMITH_DIR=${SUDO_USER:+/home/$SUDO_USER/.docksmith}
DOCKSMITH_DIR=${DOCKSMITH_DIR:-$HOME/.docksmith}

echo "[Setup] Using state directory: $DOCKSMITH_DIR"
echo "[Setup] Downloading real Alpine 3.18 rootfs..."
cd /tmp
wget -q https://dl-cdn.alpinelinux.org/alpine/v3.18/releases/x86_64/alpine-minirootfs-3.18.4-x86_64.tar.gz -O alpine.tar.gz
gunzip -f alpine.tar.gz

LAYER_DIGEST=$(sha256sum alpine.tar | awk '{print $1}')
LAYER_SIZE=$(stat -c%s alpine.tar 2>/dev/null || stat -f%z alpine.tar 2>/dev/null)
echo "[Setup] Alpine tar digest: $LAYER_DIGEST"
echo "[Setup] Alpine tar size:   $LAYER_SIZE bytes"

mkdir -p "$DOCKSMITH_DIR/layers" "$DOCKSMITH_DIR/images"
mv alpine.tar "$DOCKSMITH_DIR/layers/$LAYER_DIGEST.tar"

# Build canonical manifest
CANONICAL="{
  \"name\": \"alpine\",
  \"tag\": \"3.18\",
  \"digest\": \"\",
  \"created\": \"2023-01-01T00:00:00Z\",
  \"config\": {
    \"Env\": [],
    \"Cmd\": [\"/bin/sh\"],
    \"WorkingDir\": \"/\"
  },
  \"layers\": [
    { \"digest\": \"sha256:$LAYER_DIGEST\", \"size\": $LAYER_SIZE, \"createdBy\": \"alpine 3.18 base layer\" }
  ]
}"

MANIFEST_DIGEST=$(printf '%s' "$CANONICAL" | sha256sum | awk '{print $1}')
echo "[Setup] Manifest digest: sha256:$MANIFEST_DIGEST"

cat <<JSON > "$DOCKSMITH_DIR/images/alpine_3.18.json"
{
  "name": "alpine",
  "tag": "3.18",
  "digest": "sha256:$MANIFEST_DIGEST",
  "created": "2023-01-01T00:00:00Z",
  "config": {
    "Env": [],
    "Cmd": ["/bin/sh"],
    "WorkingDir": "/"
  },
  "layers": [
    { "digest": "sha256:$LAYER_DIGEST", "size": $LAYER_SIZE, "createdBy": "alpine 3.18 base layer" }
  ]
}
JSON
echo "[Setup] Done writing real Alpine image!"
