#!/bin/sh
# =============================================================================
# get_quickjs.sh — fetch the latest QuickJS-NG engine into ./src
#
# The QuickJS-NG source is intentionally NOT stored in this git repository;
# it is fetched from GitHub.  The Make/CMake build files run this script
# automatically when src/quickjs.h is missing.  You can also run it manually:
#
#     ./get_quickjs.sh
#
# By default the LATEST QuickJS-NG from the upstream default branch is used
# (no version pinning).  To pin to a specific tag or commit instead:
#
#     QJS_REF=v0.16.1        ./get_quickjs.sh
#     QJS_REF=<commit-hash>  ./get_quickjs.sh
#
# Environment:
#   QJS_REPO  upstream URL     (default https://github.com/quickjs-ng/quickjs.git)
#   QJS_REF   tag or commit    (optional; default = latest default branch)
#
# Idempotent: does nothing if src/quickjs.h already exists.  A partial or
# leftover checkout is removed and re-fetched.
# =============================================================================
set -e

QJS_REPO="${QJS_REPO:-https://github.com/quickjs-ng/quickjs.git}"
QJS_REF="${QJS_REF:-}"
DEST="src"

# Resolve this script's directory so it works from any CWD.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR"

if [ -f "$DEST/quickjs.h" ]; then
    echo "[quickjs] already present: $DEST/quickjs.h (nothing to do)"
    exit 0
fi

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

if [ -n "$QJS_REF" ]; then
    echo "[quickjs] fetching QuickJS-NG ${QJS_REF} from ${QJS_REPO} ..."
    # Fetch exactly the requested ref (works for tags and commit hashes).
    git init --quiet "$TMP_DIR/qjs"
    git -C "$TMP_DIR/qjs" remote add origin "$QJS_REPO"
    git -C "$TMP_DIR/qjs" fetch --quiet --depth 1 origin "$QJS_REF"
    git -C "$TMP_DIR/qjs" checkout --quiet FETCH_HEAD
else
    echo "[quickjs] fetching latest QuickJS-NG from ${QJS_REPO} ..."
    git clone --quiet --depth 1 "$QJS_REPO" "$TMP_DIR/qjs"
fi

if [ ! -f "$TMP_DIR/qjs/quickjs.h" ]; then
    echo "[quickjs] ERROR: fetch did not produce quickjs.h" >&2
    exit 1
fi

# Replace any partial/leftover checkout with the fetched source.
rm -rf "$DEST"
mkdir -p "$DEST"
# Copy everything except the upstream .git directory.
( cd "$TMP_DIR/qjs" && tar --exclude='./.git' -cf - . ) | ( cd "$DEST" && tar -xf - )

echo "[quickjs] QuickJS-NG ready at $DEST/ (latest default branch)"
