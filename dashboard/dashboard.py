#!/usr/bin/env python3
"""
dashboard.py — Docksmith Interactive Web Dashboard

Reads ~/.docksmith/ state and provides a live dashboard with:
  - Real-time image, layer, and cache display
  - Interactive delete (rmi) button per image
  - Garbage collection trigger (docksmith_gc.sh)
  - Build log tail
  - 5-second auto-refresh

Usage:
    python3 dashboard.py                    # http://localhost:9876
    python3 dashboard.py --port 8080
    python3 dashboard.py --docksmith /path/to/docksmith
"""

import os
import re
import json
import subprocess
import argparse
from pathlib import Path
from datetime import datetime, timezone
from flask import Flask, render_template, jsonify, request

# Strip ANSI escape codes from CLI output
_ANSI_RE = re.compile(r'\x1b\[[0-9;]*m')
def strip_ansi(s: str) -> str:
    return _ANSI_RE.sub('', s)

app = Flask(__name__)

# Path to the docksmith binary — resolved at startup
DOCKSMITH_BIN = None
GC_SCRIPT     = None

# ── Locate ~/.docksmith ────────────────────────────────────────────────────

def get_docksmith_dir() -> Path:
    """Mirror the C code's get_docksmith_dir() logic exactly."""
    sudo_user = os.environ.get("SUDO_USER")
    if sudo_user:
        return Path(f"/home/{sudo_user}/.docksmith")
    return Path.home() / ".docksmith"

# ── Data readers ───────────────────────────────────────────────────────────

def read_images(ds_dir: Path) -> list:
    images = []
    images_dir = ds_dir / "images"
    if not images_dir.exists():
        return images

    for json_path in sorted(images_dir.glob("*.json")):
        try:
            with open(json_path) as f:
                manifest = json.load(f)   # will raise on malformed JSON

            layers        = manifest.get("layers", [])
            total_size    = sum(l.get("size", 0) for l in layers)
            created_raw   = manifest.get("created", "")

            try:
                dt = datetime.fromisoformat(created_raw.replace("Z", "+00:00"))
                created_display = dt.strftime("%Y-%m-%d %H:%M:%S UTC")
                created_ago     = _time_ago(dt)
            except Exception:
                created_display = created_raw
                created_ago     = ""

            full_digest = manifest.get("digest", "")
            short_id    = full_digest.replace("sha256:", "")[:12] if full_digest else "?"

            images.append({
                "name":           manifest.get("name", "?"),
                "tag":            manifest.get("tag", "?"),
                "full_name":      f"{manifest.get('name','?')}:{manifest.get('tag','?')}",
                "digest":         full_digest,
                "short_id":       short_id,
                "created":        created_display,
                "created_ago":    created_ago,
                "created_raw":    created_raw,
                "layer_count":    len(layers),
                "total_size":     total_size,
                "total_size_fmt": _fmt_size(total_size),
                "layers":         layers,
                "config":         manifest.get("config", {}),
                "manifest_file":  json_path.name,
            })
        except json.JSONDecodeError as e:
            print(f"[dashboard] Warning: malformed JSON in {json_path.name}: {e}")
            # Still show it — partial data is better than nothing
            images.append({
                "name":           json_path.stem.replace("_", ":"),
                "tag":            "?",
                "full_name":      json_path.stem.replace("_", ":"),
                "digest":         "",
                "short_id":       "??????",
                "created":        "unknown",
                "created_ago":    "",
                "created_raw":    "",
                "layer_count":    0,
                "total_size":     0,
                "total_size_fmt": "?",
                "layers":         [],
                "config":         {},
                "manifest_file":  json_path.name,
                "parse_error":    str(e),
            })
        except Exception as e:
            print(f"[dashboard] Warning: could not read {json_path}: {e}")

    images.sort(key=lambda x: x.get("created_raw", ""), reverse=True)
    return images


def read_layers(ds_dir: Path, images: list) -> list:
    layers_dir = ds_dir / "layers"
    if not layers_dir.exists():
        return []

    digest_to_images = {}
    for img in images:
        for lyr in img.get("layers", []):
            d = lyr.get("digest", "").replace("sha256:", "")
            if d:
                digest_to_images.setdefault(d, []).append(img["full_name"])

    layers = []
    for tar_path in sorted(layers_dir.glob("*.tar")):
        digest = tar_path.stem
        size   = tar_path.stat().st_size
        mtime  = tar_path.stat().st_mtime
        dt     = datetime.fromtimestamp(mtime, tz=timezone.utc)
        layers.append({
            "digest":       digest,
            "short_id":     digest[:12],
            "size":         size,
            "size_fmt":     _fmt_size(size),
            "modified":     dt.strftime("%Y-%m-%d %H:%M:%S UTC"),
            "modified_ago": _time_ago(dt),
            "used_by":      digest_to_images.get(digest, []),
            "orphaned":     len(digest_to_images.get(digest, [])) == 0,
        })

    layers.sort(key=lambda x: x["size"], reverse=True)
    return layers


def read_cache(ds_dir: Path) -> dict:
    cache_dir = ds_dir / "cache"
    if not cache_dir.exists():
        return {"entries": [], "total": 0, "valid": 0, "dangling": 0,
                "hit_ratio_pct": 0}

    entries = []
    for cache_file in sorted(cache_dir.iterdir()):
        if not cache_file.is_file():
            continue
        try:
            layer_digest = cache_file.read_text().strip()
            mtime        = cache_file.stat().st_mtime
            dt           = datetime.fromtimestamp(mtime, tz=timezone.utc)
            layer_tar    = ds_dir / "layers" / f"{layer_digest}.tar"
            valid        = layer_tar.exists()
            entries.append({
                "cache_key":    cache_file.name,
                "short_key":    cache_file.name[:16],
                "layer_digest": layer_digest,
                "short_digest": layer_digest[:12],
                "valid":        valid,
                "created":      dt.strftime("%Y-%m-%d %H:%M:%S UTC"),
                "created_ago":  _time_ago(dt),
            })
        except Exception:
            pass

    valid_count = sum(1 for e in entries if e["valid"])
    return {
        "entries":       entries,
        "total":         len(entries),
        "valid":         valid_count,
        "dangling":      len(entries) - valid_count,
        "hit_ratio_pct": round((valid_count / len(entries) * 100) if entries else 0, 1),
    }


def read_storage_summary(ds_dir: Path) -> dict:
    def dir_size(path: Path):
        if not path.exists(): return 0
        return sum(f.stat().st_size for f in path.rglob("*") if f.is_file())

    images_size = dir_size(ds_dir / "images")
    layers_size = dir_size(ds_dir / "layers")
    cache_size  = dir_size(ds_dir / "cache")
    total       = images_size + layers_size + cache_size

    return {
        "images_size":     images_size,
        "images_size_fmt": _fmt_size(images_size),
        "layers_size":     layers_size,
        "layers_size_fmt": _fmt_size(layers_size),
        "cache_size":      cache_size,
        "cache_size_fmt":  _fmt_size(cache_size),
        "total":           total,
        "total_fmt":       _fmt_size(total),
        "docksmith_dir":   str(ds_dir),
    }

# ── Helpers ────────────────────────────────────────────────────────────────

def _fmt_size(b: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if b < 1024: return f"{b:.1f} {unit}"
        b /= 1024
    return f"{b:.1f} TB"

def _time_ago(dt: datetime) -> str:
    diff = int((datetime.now(tz=timezone.utc) - dt).total_seconds())
    if diff < 60:   return f"{diff}s ago"
    if diff < 3600: return f"{diff//60}m ago"
    if diff < 86400: return f"{diff//3600}h ago"
    return f"{diff//86400}d ago"

# ── Flask routes ───────────────────────────────────────────────────────────

@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/data")
def api_data():
    ds_dir  = get_docksmith_dir()
    images  = read_images(ds_dir)
    layers  = read_layers(ds_dir, images)
    cache   = read_cache(ds_dir)
    storage = read_storage_summary(ds_dir)

    return jsonify({
        "timestamp":     datetime.now(tz=timezone.utc).isoformat(),
        "docksmith_dir": str(ds_dir),
        "images":        images,
        "layers":        layers,
        "cache":         cache,
        "storage":       storage,
        "summary": {
            "image_count":   len(images),
            "layer_count":   len(layers),
            "orphan_count":  sum(1 for l in layers if l["orphaned"]),
            "cache_entries": cache["total"],
            "total_storage": storage["total_fmt"],
        },
        "capabilities": {
            "can_rmi": DOCKSMITH_BIN is not None,
            "can_gc":  GC_SCRIPT    is not None,
        },
    })


@app.route("/api/rmi", methods=["POST"])
def api_rmi():
    """
    Delete an image.  POST body: { "image": "name:tag" }
    Calls: sudo ./docksmith rmi <name:tag>
    Requires the docksmith binary to be found at startup.
    """
    if not DOCKSMITH_BIN:
        return jsonify({"ok": False,
                        "error": "docksmith binary not found. "
                                 "Run the dashboard from the project directory."}), 400

    data  = request.get_json(force=True, silent=True) or {}
    image = (data.get("image") or "").strip()
    if not image or ":" not in image:
        return jsonify({"ok": False, "error": "image must be name:tag"}), 400

    # Safety: only allow alphanumeric, colons, dots, hyphens, underscores
    if not re.fullmatch(r'[a-zA-Z0-9_.:\-/]+', image):
        return jsonify({"ok": False, "error": "invalid image name"}), 400

    try:
        result = subprocess.run(
            ["sudo", str(DOCKSMITH_BIN), "rmi", image],
            capture_output=True, text=True, timeout=15
        )
        ok  = result.returncode == 0
        out = strip_ansi((result.stdout + result.stderr).strip())
        return jsonify({"ok": ok, "output": out, "image": image})
    except subprocess.TimeoutExpired:
        return jsonify({"ok": False, "error": "command timed out"}), 500
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 500


@app.route("/api/gc", methods=["POST"])
def api_gc():
    """
    Run garbage collection (docksmith_gc.sh).
    """
    if not GC_SCRIPT:
        return jsonify({"ok": False,
                        "error": "docksmith_gc.sh not found."}), 400
    try:
        result = subprocess.run(
            ["sudo", "bash", str(GC_SCRIPT)],
            capture_output=True, text=True, timeout=30
        )
        ok  = result.returncode == 0
        out = strip_ansi((result.stdout + result.stderr).strip())
        return jsonify({"ok": ok, "output": out})
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 500


@app.route("/api/health")
def health():
    ds_dir = get_docksmith_dir()
    return jsonify({
        "status":          "ok",
        "docksmith_dir":   str(ds_dir),
        "dir_exists":      ds_dir.exists(),
        "docksmith_bin":   str(DOCKSMITH_BIN) if DOCKSMITH_BIN else None,
        "gc_script":       str(GC_SCRIPT)     if GC_SCRIPT     else None,
        "server_time":     datetime.now(tz=timezone.utc).isoformat(),
    })


# ── Entry point ────────────────────────────────────────────────────────────

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Docksmith Web Dashboard")
    parser.add_argument("--port",       type=int, default=9876)
    parser.add_argument("--host",       default="127.0.0.1")
    parser.add_argument("--docksmith",  default=None,
                        help="Path to docksmith binary (auto-detected if omitted)")
    parser.add_argument("--debug",      action="store_true")
    args = parser.parse_args()

    # Auto-detect docksmith binary and gc script
    search_dirs = [Path("."), Path(__file__).parent, Path(__file__).parent.parent]
    for d in search_dirs:
        b = d / "docksmith"
        if b.exists() and os.access(b, os.X_OK):
            DOCKSMITH_BIN = b.resolve()
            break
    if args.docksmith:
        DOCKSMITH_BIN = Path(args.docksmith).resolve()

    for d in search_dirs:
        g = d / "docksmith_gc.sh"
        if g.exists():
            GC_SCRIPT = g.resolve()
            break

    ds_dir = get_docksmith_dir()
    print(f"\n  Docksmith Dashboard")
    print(f"  {'─'*45}")
    print(f"  State dir   : {ds_dir}")
    print(f"  Binary      : {DOCKSMITH_BIN or 'not found (rmi disabled)'}")
    print(f"  GC script   : {GC_SCRIPT     or 'not found (gc disabled)'}")
    print(f"  Dashboard   : http://{args.host}:{args.port}")
    print(f"  {'─'*45}\n")

    app.run(host=args.host, port=args.port, debug=args.debug)