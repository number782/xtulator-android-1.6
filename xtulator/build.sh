#!/bin/bash
adb uninstall com.xtulator.android
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Read version before build (build-apk.sh will increment it)
PREV_VERSION=$(cat VERSION 2>/dev/null || echo "0")
echo "Building from VERSION=$PREV_VERSION"

rm -f xtulator-debug.apk
adb -s SSHEX010990 uninstall com.xtulator.android 2>/dev/null || true
docker run --rm -v "$PWD":/src xtulator-android16 /bin/bash -c "cd /src && ./build-apk.sh"

# Show the new version
NEW_VERSION=$(cat VERSION)
echo "=== Build complete: v1.$(printf '%03d' "$NEW_VERSION")A ==="

adb -s SSHEX010990 install -r xtulator-debug.apk
adb -s SSHEX010990 logcat -c
echo "=== Deployed v1.$(printf '%03d' "$NEW_VERSION")A ==="
echo "Check logcat for: BUILD VERSION: 1.$(printf '%03d' "$NEW_VERSION")A"
