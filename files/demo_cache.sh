#!/bin/bash
set -e

echo "[Setup] Preparing local environment..."
mkdir -p ~/.docksmith/images ~/.docksmith/layers ~/.docksmith/cache

# Create a valid empty tar for the mock alpine base layer
TAR_FILE="$HOME/.docksmith/layers/mock_alpine_layer.tar"
tar -cf "$TAR_FILE" -T /dev/null

# Get the digest
LAYER_DIGEST=$(sha256sum "$TAR_FILE" | awk '{print $1}')
FINAL_TAR="$HOME/.docksmith/layers/$LAYER_DIGEST.tar"
mv "$TAR_FILE" "$FINAL_TAR"

# Create the top level manifest digest (just any hash)
MANIFEST_DIGEST=$(echo "manifest" | sha256sum | awk '{print $1}')

# Write the Alpine manifest
JSON_FILE="$HOME/.docksmith/images/alpine_3.18.json"
cat <<EOF > "$JSON_FILE"
{
  "name": "alpine",
  "tag": "3.18",
  "digest": "sha256:$MANIFEST_DIGEST",
  "layers": [
    {
      "digest": "sha256:$LAYER_DIGEST",
      "size": 10240,
      "createdBy": "fake base layer"
    }
  ]
}
EOF

echo "[Setup] Made mock Alpine 3.18 image at $JSON_FILE"
echo "[Setup] Compiling docksmith engine..."
make clean
make

echo -e "\n============================================="
echo "       RUNNING DOCKSMITH BUILD 1 (MISS)"
echo -e "=============================================\n"
./docksmith build -t c_test:latest .

echo -e "\n============================================="
echo "       RUNNING DOCKSMITH BUILD 2 (HIT)"
echo -e "=============================================\n"
./docksmith build -t c_test:latest .
