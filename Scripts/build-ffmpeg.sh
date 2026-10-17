#!/usr/bin/env bash
# Builds Vendor/FFmpeg/SpectorFFmpeg.xcframework (ADR 0010, ADR 0017, ADR 0020):
# one dynamic framework holding the engine's bridge (Scripts/ffmpeg-bridge) and
# an LGPL-only FFmpeg, libavcodec and libavutil, linked statically into it. It
# exports only the bridge's spector_ffmpeg_* functions, so its FFmpeg never
# clashes with another FFmpeg in the same app. Slices for tvOS, the tvOS
# Simulator and macOS (for tests).
# The framework and BUILD_INFO.txt are committed (ADR 0020), so run this only to
# change them: a new FFmpeg release, decoder, slice or bridge change. With the
# same Xcode a rebuild is byte-identical, so `--clean` followed by
# `git status --short Vendor/FFmpeg` checks the committed framework.
#
# Usage: Scripts/build-ffmpeg.sh [--clean]
set -euo pipefail

cd "$(dirname "$0")/.."

FFMPEG_VERSION="9.0.2"
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"
# Matches the hash pinned independently by Homebrew's ffmpeg formula (2026-10-16).
FFMPEG_SHA256="8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e"
TVOS_MIN="18.0"
MACOS_MIN="15.0"
BUNDLE_ID="app.spector.engine.ffmpeg"

# Every decoder is listed explicitly, and each needs a licensing check before it
# is added here (ADR 0017).
DECODERS="mp1,mp1float,mp2,mp2float"

OUT="$PWD/Vendor/FFmpeg"
WORK="$OUT/.work"
SOURCE="$WORK/ffmpeg-${FFMPEG_VERSION}"
FRAMEWORK="SpectorFFmpeg"
BRIDGE_SOURCE="$PWD/Scripts/ffmpeg-bridge"

# name  arch     sdk                target triple
SLICES=(
    "tvos         arm64  appletvos         arm64-apple-tvos${TVOS_MIN}"
    "tvsimulator  arm64  appletvsimulator  arm64-apple-tvos${TVOS_MIN}-simulator"
    "tvsimulator  x86_64 appletvsimulator  x86_64-apple-tvos${TVOS_MIN}-simulator"
    "macos        arm64  macosx            arm64-apple-macos${MACOS_MIN}"
    "macos        x86_64 macosx            x86_64-apple-macos${MACOS_MIN}"
)
PLATFORMS=(tvos tvsimulator macos)

if [[ "${1:-}" == "--clean" ]]; then
    rm -rf "$WORK" "$OUT"/*.xcframework "$OUT/BUILD_INFO.txt"
fi

fetch_source() {
    [[ -d "$SOURCE" ]] && return
    mkdir -p "$WORK/download"
    local archive="$WORK/download/ffmpeg-${FFMPEG_VERSION}.tar.xz"
    [[ -f "$archive" ]] || curl -sSfL -o "$archive" "$FFMPEG_URL"
    local actual
    actual="$(shasum -a 256 "$archive" | cut -d' ' -f1)"
    if [[ "$actual" != "$FFMPEG_SHA256" ]]; then
        echo "error: SHA-256 mismatch for $archive: $actual" >&2
        rm -f "$archive"
        exit 1
    fi
    tar -xf "$archive" -C "$WORK"
}

configure_flags() {
    local arch="$1" sdk="$2" triple="$3"
    local sysroot
    sysroot="$(xcrun --sdk "$sdk" --show-sdk-path)"
    local flags=(
        --enable-cross-compile --target-os=darwin
        --arch="$([[ "$arch" == arm64 ]] && echo aarch64 || echo x86_64)"
        --cc="$(xcrun --sdk "$sdk" -f clang)"
        --sysroot="$sysroot"
        --extra-cflags="-target $triple"
        --extra-ldflags="-target $triple"
        # Licence: LGPL 2.1 or later only. Never add --enable-gpl,
        # --enable-nonfree or --enable-version3 (ADR 0010, ADR 0017).
        --disable-everything --enable-decoder="$DECODERS"
        --disable-autodetect --disable-network --disable-programs --disable-doc
        --disable-avformat --disable-avdevice --disable-avfilter
        --disable-swscale --disable-swresample
        # Static archives, linked into the framework with the bridge (ADR 0020).
        --disable-debug --enable-pic --enable-static --disable-shared
    )
    # The Mac has no nasm; the enabled decoders have no x86 assembly anyway.
    [[ "$arch" == x86_64 ]] && flags+=(--disable-x86asm)
    printf '%s\n' "${flags[@]}"
}

# Fails the build if the configuration is anything but LGPL 2.1 or later.
check_licence() {
    local config="$1"
    for option in CONFIG_GPL CONFIG_NONFREE CONFIG_VERSION3; do
        if ! grep -q "^#define $option 0$" "$config"; then
            echo "error: $option is not 0 in $config" >&2
            exit 1
        fi
    done
}

build_slice() {
    local name="$1" arch="$2" sdk="$3" triple="$4"
    local build="$WORK/build/$name-$arch" prefix="$WORK/install/$name-$arch"
    if [[ -f "$prefix/lib/libavcodec.a" ]]; then return; fi
    echo "::group::FFmpeg $FFMPEG_VERSION for $name $arch"
    rm -rf "$build" "$prefix"
    mkdir -p "$build"
    local flags=()
    while IFS= read -r flag; do flags+=("$flag"); done < <(configure_flags "$arch" "$sdk" "$triple")
    (cd "$build" && "$SOURCE/configure" --prefix="$prefix" "${flags[@]}" > configure.log)
    check_licence "$build/config.h"
    make -C "$build" -j"$(sysctl -n hw.ncpu)" install > "$build/make.log"
    echo "::endgroup::"
}

install_name() {
    if [[ "$1" == macos ]]; then
        echo "@rpath/$FRAMEWORK.framework/Versions/A/$FRAMEWORK"
    else
        echo "@rpath/$FRAMEWORK.framework/$FRAMEWORK"
    fi
}

# Links the bridge and FFmpeg's archives for one slice into a dynamic library
# that exports only spector_ffmpeg_*: FFmpeg's own symbols stay inside it.
link_slice() {
    local name="$1" arch="$2" sdk="$3" triple="$4" platform="$5" output="$6"
    local prefix="$WORK/install/$name-$arch" private
    # The system libraries FFmpeg's static archives need (-lm, -pthread).
    private="$(sed -n 's/^Libs.private: //p' "$prefix/lib/pkgconfig/libavcodec.pc" "$prefix/lib/pkgconfig/libavutil.pc" | tr '\n' ' ')"
    mkdir -p "$(dirname "$output")"
    # shellcheck disable=SC2086
    "$(xcrun --sdk "$sdk" -f clang)" -target "$triple" -isysroot "$(xcrun --sdk "$sdk" --show-sdk-path)" \
        -dynamiclib -O2 -Wall -Werror -fvisibility=hidden \
        -I "$prefix/include" "$BRIDGE_SOURCE/$FRAMEWORK.c" \
        "$prefix/lib/libavcodec.a" "$prefix/lib/libavutil.a" $private \
        -Wl,-exported_symbol,'_spector_ffmpeg_*' -Wl,-dead_strip \
        -install_name "$(install_name "$platform")" \
        -compatibility_version 1 -current_version "$FFMPEG_VERSION" \
        -o "$output"
}

info_plist() {
    local platform="$1"
    local minimum=""
    case "$platform" in
        tvos | tvsimulator)
            minimum="<key>MinimumOSVersion</key><string>$TVOS_MIN</string>" ;;
        macos)
            minimum="<key>LSMinimumSystemVersion</key><string>$MACOS_MIN</string>" ;;
    esac
    cat <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>$FRAMEWORK</string>
    <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>$FRAMEWORK</string>
    <key>CFBundlePackageType</key><string>FMWK</string>
    <key>CFBundleShortVersionString</key><string>$FFMPEG_VERSION</string>
    <key>CFBundleVersion</key><string>$FFMPEG_VERSION</string>
    $minimum
</dict>
</plist>
EOF
}

module_map() {
    cat <<EOF
framework module $FRAMEWORK {
    header "$FRAMEWORK.h"
    export *
}
EOF
}

# Builds <platform>/SpectorFFmpeg.framework: shallow on tvOS, versioned on macOS.
make_framework() {
    local platform="$1"
    local slices=()
    for slice in "${SLICES[@]}"; do
        read -r name arch sdk triple <<< "$slice"
        [[ "$name" == "$platform" ]] || continue
        slices+=("$WORK/dylib/$name-$arch/$FRAMEWORK")
        link_slice "$name" "$arch" "$sdk" "$triple" "$platform" "$WORK/dylib/$name-$arch/$FRAMEWORK"
    done
    local root="$WORK/frameworks/$platform/$FRAMEWORK.framework" contents
    rm -rf "$root"
    if [[ "$platform" == macos ]]; then
        contents="$root/Versions/A"
        mkdir -p "$contents/Resources"
    else
        contents="$root"
        mkdir -p "$contents"
    fi
    lipo -create "${slices[@]}" -output "$contents/$FRAMEWORK"
    mkdir -p "$contents/Headers" "$contents/Modules"
    cp "$BRIDGE_SOURCE/$FRAMEWORK.h" "$contents/Headers/"
    module_map > "$contents/Modules/module.modulemap"
    if [[ "$platform" == macos ]]; then
        info_plist "$platform" > "$contents/Resources/Info.plist"
        ln -s A "$root/Versions/Current"
        for entry in "$FRAMEWORK" Headers Modules Resources; do
            ln -s "Versions/Current/$entry" "$root/$entry"
        done
    else
        info_plist "$platform" > "$contents/Info.plist"
    fi
}

# Fails the build if the framework exports anything but the bridge.
check_exports() {
    local binary="$1" unexpected
    unexpected="$(nm -gUj "$binary" | grep -v '^_spector_ffmpeg_' | grep -v '^$' | grep -v ':$' || true)"
    if [[ -n "$unexpected" ]]; then
        echo "error: $binary exports more than spector_ffmpeg_*:" >&2
        echo "$unexpected" | head >&2
        exit 1
    fi
}

# xcodebuild lists an XCFramework's slices in no fixed order. Sorting them
# keeps a rebuild byte-identical, so `git status` can check the framework.
sort_xcframework_libraries() {
    python3 -I - "$1" <<'EOF'
import plistlib, sys
with open(sys.argv[1], "rb") as f:
    plist = plistlib.load(f)
plist["AvailableLibraries"].sort(key=lambda library: library["LibraryIdentifier"])
with open(sys.argv[1], "wb") as f:
    plistlib.dump(plist, f, sort_keys=True)
EOF
}

fetch_source
for slice in "${SLICES[@]}"; do
    # shellcheck disable=SC2086
    build_slice $slice
done
arguments=()
for platform in "${PLATFORMS[@]}"; do
    make_framework "$platform"
    check_exports "$WORK/frameworks/$platform/$FRAMEWORK.framework/$FRAMEWORK"
    arguments+=(-framework "$WORK/frameworks/$platform/$FRAMEWORK.framework")
done
rm -rf "$OUT"/*.xcframework
xcodebuild -create-xcframework "${arguments[@]}" -output "$OUT/$FRAMEWORK.xcframework" > /dev/null
sort_xcframework_libraries "$OUT/$FRAMEWORK.xcframework/Info.plist"

cat > "$OUT/BUILD_INFO.txt" <<EOF
FFmpeg $FFMPEG_VERSION (libavcodec, libavutil; static, inside $FRAMEWORK.framework)
Source: $FFMPEG_URL
SHA-256: $FFMPEG_SHA256
Decoders: $DECODERS
Bridge: Scripts/ffmpeg-bridge/$FRAMEWORK.c, exports spector_ffmpeg_* only
Configure flags (tvOS arm64):
$(configure_flags arm64 appletvos "arm64-apple-tvos${TVOS_MIN}" | sed 's/^/  /')
Built with: $(xcodebuild -version | paste -sd ' ' -)
EOF
echo "$FRAMEWORK $FFMPEG_VERSION written to $OUT"
