#!/bin/bash
# docksmith_gc.sh - Safely prunes orphaned layers and cache entries.

echo -e "\033[1;36m========================================\033[0m"
echo -e "\033[1;35m      DOCKSMITH GARBAGE COLLECTOR       \033[0m"
echo -e "\033[1;36m========================================\033[0m"

DOCKSMITH_DIR=${SUDO_USER:+/home/$SUDO_USER/.docksmith}
DOCKSMITH_DIR=${DOCKSMITH_DIR:-$HOME/.docksmith}

IMAGES_DIR="$DOCKSMITH_DIR/images"
LAYERS_DIR="$DOCKSMITH_DIR/layers"
CACHE_DIR="$DOCKSMITH_DIR/cache"

if [ ! -d "$IMAGES_DIR" ]; then
    echo "No images found."
    exit 0
fi

mkdir -p /tmp/docksmith_gc
ACTIVE_LAYERS="/tmp/docksmith_gc/active_layers.txt"
> "$ACTIVE_LAYERS"

# 1. Collect all active layer digests from existing manifests
for manifest in "$IMAGES_DIR"/*.json; do
    if [ -f "$manifest" ]; then
        grep -o '"digest"\s*:\s*"sha256:[a-f0-9]*"' "$manifest" | grep -v 'sha256:$' | cut -d':' -f2 | tr -d '"' >> "$ACTIVE_LAYERS"
    fi
done

sort -u "$ACTIVE_LAYERS" -o "$ACTIVE_LAYERS"
active_count=$(wc -l < "$ACTIVE_LAYERS")
echo -e "Found \033[1;32m$active_count\033[0m actively referenced layers across all images."

cleaned_layers=0
cleaned_bytes=0
cleaned_caches=0

# 2. Prune orphaned layer tar files
echo -e "\nScanning for orphaned layer files..."
for tar in "$LAYERS_DIR"/*.tar; do
    if [ -f "$tar" ]; then
        basename_tar=$(basename "$tar")
        digest="${basename_tar%.tar}"

        if ! grep -q "^${digest}$" "$ACTIVE_LAYERS"; then
            size=$(stat -c%s "$tar")
            cleaned_bytes=$((cleaned_bytes + size))
            rm -f "$tar"
            cleaned_layers=$((cleaned_layers + 1))
        fi
    fi
done

# 3. Prune orphaned cache mappings
echo "Scanning for orphaned cache mappings..."
for cache_file in "$CACHE_DIR"/*; do
    if [ -f "$cache_file" ]; then
        target_digest=$(cat "$cache_file" | tr -d '\r\n')
        if ! grep -q "^${target_digest}$" "$ACTIVE_LAYERS"; then
            rm -f "$cache_file"
            cleaned_caches=$((cleaned_caches + 1))
        fi
    fi
done

# Clean up temp
rm -rf /tmp/docksmith_gc

mb=$(echo "scale=2; $cleaned_bytes / 1048576" | bc)
echo -e "\n\033[1;32m[GC COMPLETE]\033[0m"
echo -e "Deleted \033[1;31m$cleaned_layers\033[0m orphaned layer tars (Freed $mb MB)."
echo -e "Deleted \033[1;31m$cleaned_caches\033[0m dangling cache mappings."
echo -e "\033[1;36m========================================\033[0m\n"
