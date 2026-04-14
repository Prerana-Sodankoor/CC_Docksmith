#!/bin/bash
# run_dashboard.sh — Start the Docksmith web dashboard
#
# Usage:
#   bash run_dashboard.sh
#   bash run_dashboard.sh --port 8080

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DASHBOARD_DIR="$SCRIPT_DIR/dashboard"

echo ""
echo "  ╔══════════════════════════════════════╗"
echo "  ║     Docksmith Dashboard Setup        ║"
echo "  ╚══════════════════════════════════════╝"
echo ""

# Check Python 3
if ! command -v python3 &>/dev/null; then
    echo "  [ERROR] python3 not found. Install with:"
    echo "          sudo apt install python3 python3-pip"
    exit 1
fi

# Check / install Flask
if ! python3 -c "import flask" &>/dev/null; then
    echo "  [INFO] Flask not found. Installing..."
    pip3 install flask --quiet
    echo "  [OK]   Flask installed."
else
    echo "  [OK]   Flask is available."
fi

# Check ~/.docksmith exists
DOCKSMITH_DIR="${SUDO_USER:+/home/$SUDO_USER/.docksmith}"
DOCKSMITH_DIR="${DOCKSMITH_DIR:-$HOME/.docksmith}"

if [ ! -d "$DOCKSMITH_DIR" ]; then
    echo "  [WARN] $DOCKSMITH_DIR does not exist yet."
    echo "         Run a build first:  sudo ./docksmith build -t myapp:latest ."
    echo "         The dashboard will still start and show empty state."
else
    echo "  [OK]   Found state dir: $DOCKSMITH_DIR"
    IMG_COUNT=$(ls "$DOCKSMITH_DIR/images/"*.json 2>/dev/null | wc -l)
    LAYER_COUNT=$(ls "$DOCKSMITH_DIR/layers/"*.tar 2>/dev/null | wc -l)
    echo "         Images: $IMG_COUNT   Layers: $LAYER_COUNT"
fi

echo ""
echo "  Starting dashboard server..."
echo "  Open in browser: http://localhost:9876"
echo "  Press Ctrl+C to stop."
echo ""

# Run
cd "$DASHBOARD_DIR"
python3 dashboard.py "$@"
