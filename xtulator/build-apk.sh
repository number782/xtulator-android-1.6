#!/bin/bash
set -euo pipefail

# ── Project root and config ──────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

NDK_ROOT="${NDK_ROOT:-/opt/android/android-ndk-r5c}"
ANT_DEBUG_OUT="android/bin/XTulator-debug.apk"
FINAL_APK="xtulator-debug.apk"
PACKAGE_NAME="com.xtulator.android"
SDK_PATH="${ANDROID_HOME:-/opt/android-sdk}"

echo "=== XTulator APK Build ==="
echo "Script dir: $SCRIPT_DIR"
echo "NDK root:   $NDK_ROOT"
echo "SDK path:   $SDK_PATH"

# ── Pre-flight: NDK must exist ───────────────────────────────────────────────
if [ ! -d "$NDK_ROOT" ]; then
    echo "ERROR: Android NDK not found at: $NDK_ROOT"
    echo "Set NDK_ROOT to your NDK r5c (or compatible) installation path."
    exit 1
fi

# ── Step 1: Clean previous build artifacts ───────────────────────────────────
echo "=== Step 1: Clean ==="
rm -rf android/bin android/libs android/obj android/gen
rm -rf android/libs/armeabi-v7a android/obj/local/armeabi-v7a

# ── Step 2: Remove stale generated R.java ────────────────────────────────────
echo "=== Step 2: Remove stale R.java ==="
rm -f android/src/com/xtulator/android/R.java
rm -f android/gen/com/xtulator/android/R.java

# ── Step 3: Write local.properties (SDK path for ant) ────────────────────────
echo "=== Step 3: Write local.properties ==="
echo "sdk.dir=$SDK_PATH" > android/local.properties

# ── NDK r5c workaround: pre-create intermediate object directories ───────────
echo "=== Pre-create NDK r5c intermediate dirs ==="
OBJ_BASE="android/obj/local/armeabi"
for _build_type in objs objs-debug; do
    mkdir -p "$OBJ_BASE/$_build_type/xtulator"
    mkdir -p "$OBJ_BASE/$_build_type/xtulator/__/__/XTulator"
    mkdir -p "$OBJ_BASE/$_build_type/xtulator/__/__/XTulator/chipset"
    mkdir -p "$OBJ_BASE/$_build_type/xtulator/__/__/XTulator/cpu"
    mkdir -p "$OBJ_BASE/$_build_type/xtulator/__/__/XTulator/modules/audio"
    mkdir -p "$OBJ_BASE/$_build_type/xtulator/__/__/XTulator/modules/disk"
    mkdir -p "$OBJ_BASE/$_build_type/xtulator/__/__/XTulator/modules/input"
    mkdir -p "$OBJ_BASE/$_build_type/xtulator/__/__/XTulator/modules/video"
done

# ── Ensure a debug keystore exists ───────────────────────────────────────────
DEBUG_KEYSTORE="${HOME}/.android/debug.keystore"
DEBUG_KEY_ALIAS="androiddebugkey"
if [ ! -f "$DEBUG_KEYSTORE" ]; then
    echo "Generating debug keystore: $DEBUG_KEYSTORE"
    mkdir -p "$(dirname "$DEBUG_KEYSTORE")"
    keytool -genkey -v \
        -keystore "$DEBUG_KEYSTORE" \
        -alias "$DEBUG_KEY_ALIAS" \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -storepass android -keypass android \
        -dname "CN=Android Debug,O=Android,C=US"
fi

# ── Step 4: Build native library (ndk-build) ─────────────────────────────────
echo "=== Step 4: ndk-build ==="
"$NDK_ROOT/ndk-build" -C "$SCRIPT_DIR/android/jni"

if [ ! -f "android/libs/armeabi/libxtulator.so" ]; then
    echo "ERROR: libxtulator.so not found at android/libs/armeabi/libxtulator.so"
    exit 1
fi
echo "Native library: android/libs/armeabi/libxtulator.so"

# ── Step 5: Build APK with ant (without packaging resources) ─────────────────
echo "=== Step 5: ant debug (compile only) ==="
(cd android && ant debug -Dandroid.packaging.dex=false -Dandroid.packaging.resources=false)

# ── Step 5b: Package resources with aapt (no compression for .dat, .bin) ──────
echo "=== Step 5b: Package resources with aapt (uncompressed assets) ==="
AAPT=""
for _candidate in \
    "$SDK_PATH/build-tools/19.1.0/aapt" \
    "$SDK_PATH/tools/aapt" \
    "$ANDROID_HOME/build-tools/19.1.0/aapt" \
    "$ANDROID_HOME/tools/aapt" \
    "/opt/android-sdk/build-tools/19.1.0/aapt" \
    "/opt/android-sdk/tools/aapt" \
    "/opt/android-sdk/android-sdk-linux/build-tools/19.1.0/aapt" \
    "/opt/android-sdk/android-sdk-linux/tools/aapt"; do
    if [ -x "$_candidate" ]; then
        AAPT="$_candidate"
        break
    fi
done
if [ -z "$AAPT" ]; then
    echo "ERROR: aapt not found. Install Android SDK build-tools."
    exit 1
fi
echo "Found aapt: $AAPT"

# Package resources with no compression for .dat, .bin, .img
"$AAPT" package -f -m -J android/gen -M android/AndroidManifest.xml \
    -S android/res -I "$SDK_PATH/platforms/android-19/android.jar" \
    -F "$ANT_DEBUG_OUT" \
    -0 dat -0 bin -0 img \
    android/assets

# ── Step 5c: Build dex and add to APK ────────────────────────────────────────
echo "=== Step 5c: Build dex and add to APK ==="
(cd android && ant debug -Dandroid.packaging.resources=false)

# ── Step 6: Sign with SHA1 (jarsigner) and zipalign ──────────────────────────
echo "=== Step 6: Sign (SHA1) and zipalign ==="

ZIPALIGN=""
for _candidate in \
    "$SDK_PATH/build-tools/19.1.0/zipalign" \
    "$SDK_PATH/tools/zipalign" \
    "$ANDROID_HOME/build-tools/19.1.0/zipalign" \
    "$ANDROID_HOME/tools/zipalign" \
    "/opt/android-sdk/build-tools/19.1.0/zipalign" \
    "/opt/android-sdk/tools/zipalign" \
    "/opt/android-sdk/android-sdk-linux/build-tools/19.1.0/zipalign" \
    "/opt/android-sdk/android-sdk-linux/tools/zipalign"; do
    if [ -x "$_candidate" ]; then
        ZIPALIGN="$_candidate"
        break
    fi
done
if [ -z "$ZIPALIGN" ]; then
    echo "ERROR: zipalign not found. Install Android SDK build-tools."
    exit 1
fi
echo "Found zipalign: $ZIPALIGN"

# Strip existing signatures
zip -d "$ANT_DEBUG_OUT" "META-INF/MANIFEST.MF" "META-INF/CERT.SF" "META-INF/CERT.RSA" >/dev/null 2>&1 || true

# Sign the APK with SHA1withRSA
echo "Signing APK with SHA1 (jarsigner)..."
jarsigner -verbose -sigalg SHA1withRSA -digestalg SHA1 \
    -keystore "$DEBUG_KEYSTORE" -storepass android -keypass android \
    "$ANT_DEBUG_OUT" "$DEBUG_KEY_ALIAS"

# Zipalign to final APK at the script root
echo "Zipaligning to $FINAL_APK..."
"$ZIPALIGN" -f 4 "$ANT_DEBUG_OUT" "$FINAL_APK"

# ── Done ─────────────────────────────────────────────────────────────────────
echo ""
echo "=== Build Complete ==="
echo "APK:        $SCRIPT_DIR/$FINAL_APK"
echo "Native lib: $SCRIPT_DIR/android/libs/armeabi/libxtulator.so"
echo ""
echo "To install on device:"
echo "  adb uninstall $PACKAGE_NAME"
echo "  adb install $SCRIPT_DIR/$FINAL_APK"
