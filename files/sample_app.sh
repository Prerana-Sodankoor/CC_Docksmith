#!/bin/sh
# sample_app.sh — Docksmith demo application
#
# Uses only /bin/sh which is present in alpine:3.18 by default.
# Reads MODE from environment (set via ENV in Docksmithfile,
# overridable via -e at runtime).

echo "================================="
echo "  Docksmith Container Running!   "
echo "================================="
echo "  MODE = ${MODE:-not set}"
echo "  Working dir = $(pwd)"
echo "  Build artifact: $(cat /app/build_info.txt 2>/dev/null || echo 'none')"
echo "================================="
