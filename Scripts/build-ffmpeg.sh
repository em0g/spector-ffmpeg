#!/usr/bin/env bash
# Builds Vendor/FFmpeg/SpectorFFmpeg.xcframework (ADR 0010, ADR 0017, ADR 0020):
# one dynamic framework holding the engine's bridge (Scripts/ffmpeg-bridge) and
# an LGPL-only FFmpeg, libavcodec and libavutil, linked statically into it. It
# exports only the bridge's spector_ffmpeg_* functions, so its FFmpeg never
# clashes with another FFmpeg in the same app. Slices for tvOS, the tvOS
# Simulator and macOS (for tests). Each slice carries its dSYM, so that crashes
# inside the framework can be symbolicated (App Store Connect asks for it).
# The framework and BUILD_INFO.txt are committed (ADR 0020), so run this only to
# change them: a new FFmpeg release, decoder, slice or bridge change. FFmpeg
# records its configure flags in the binary, so they name no path of the build
# machine, and the debug information maps the checkout and Xcode to neutral
# paths: with the same Xcode a rebuild is byte-identical from any checkout,
# dSYMs included, and `--clean` followed by `git status --short Vendor/FFmpeg`
# checks the committed framework.
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
DECODERS="mp1,mp1float,mp2,mp2float,dca"
# Parsers decode nothing: the bridge asks the `dca` parser for a DTS frame's
# profile, so that it refuses DTS Express before decoding it (ADR 0034).
PARSERS="dca"

OUT="$PWD/Vendor/FFmpeg"
WORK="$OUT/.work"
SOURCE="$WORK/ffmpeg-${FFMPEG_VERSION}"
# The prefix FFmpeg is configured with, and so records in the binary
# (avcodec_configuration). Each slice installs under $WORK/install/<slice>
# through DESTDIR, so no path of the build machine ends up in the framework.
PREFIX="/spector-ffmpeg"
FRAMEWORK="SpectorFFmpeg"
BRIDGE_SOURCE="$PWD/Scripts/ffmpeg-bridge"

# The debug information names the checkout as $DEBUG_ROOT and Xcode's developer
# directory as its default location, wherever they are on the build machine.
DEBUG_ROOT="/spector-ffmpeg-build"
DEVELOPER="$(dirname "$(dirname "$(dirname "$(xcrun --find xcodebuild)")")")"
DEBUG_PREFIX_MAPS=(
    "-fdebug-prefix-map=$PWD=$DEBUG_ROOT"
    "-fdebug-prefix-map=$DEVELOPER=/Applications/Xcode.app/Contents/Developer"
)
# Archive members and the linker's debug map record no modification times.
export ZERO_AR_DATE=1

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

# Where a slice's FFmpeg is installed: DESTDIR plus the neutral prefix.
install_dir() {
    echo "$WORK/install/$1-$2$PREFIX"
}

configure_flags() {
    local arch="$1" sdk="$2" triple="$3"
    local flags=(
        --enable-cross-compile --target-os=darwin
        --arch="$([[ "$arch" == arm64 ]] && echo aarch64 || echo x86_64)"
        # Through xcrun, which also supplies the SDK, so the flags FFmpeg
        # records name no path of the Mac's Xcode.
        --cc="xcrun --sdk $sdk clang"
        --extra-cflags="-target $triple"
        --extra-ldflags="-target $triple"
        # Licence: LGPL 2.1 or later only. Never add --enable-gpl,
        # --enable-nonfree or --enable-version3 (ADR 0010, ADR 0017).
        --disable-everything --enable-decoder="$DECODERS" --enable-parser="$PARSERS"
        --disable-autodetect --disable-network --disable-programs --disable-doc
        --disable-avformat --disable-avdevice --disable-avfilter
        --disable-swscale --disable-swresample
        # Static archives, linked into the framework with the bridge (ADR 0020).
        # With debug information (FFmpeg's default -g), for the dSYMs; the
        # framework's binary is stripped of it.
        --enable-pic --enable-static --disable-shared
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
    local build="$WORK/build/$name-$arch" destination="$WORK/install/$name-$arch"
    if [[ -f "$(install_dir "$name" "$arch")/lib/libavcodec.a" ]]; then return; fi
    echo "::group::FFmpeg $FFMPEG_VERSION for $name $arch"
    rm -rf "$build" "$destination"
    mkdir -p "$build"
    local flags=()
    while IFS= read -r flag; do flags+=("$flag"); done < <(configure_flags "$arch" "$sdk" "$triple")
    # The prefix maps reach clang through the driver's override variable (the
    # leading "#" keeps it quiet) instead of --extra-cflags, which FFmpeg would
    # record in the binary with the build machine's paths.
    local overrides="#"
    for map in "${DEBUG_PREFIX_MAPS[@]}"; do overrides+=" +$map"; done
    (
        export CCC_OVERRIDE_OPTIONS="$overrides"
        cd "$build"
        "$SOURCE/configure" --prefix="$PREFIX" "${flags[@]}" > configure.log
        check_licence "$build/config.h"
        make -C "$build" -j"$(sysctl -n hw.ncpu)" install DESTDIR="$destination" > "$build/make.log"
    )
    echo "::endgroup::"
}

install_name() {
    if [[ "$1" == macos ]]; then
        echo "@rpath/$FRAMEWORK.framework/Versions/A/$FRAMEWORK"
    else
        echo "@rpath/$FRAMEWORK.framework/$FRAMEWORK"
    fi
}

# FFmpeg's archives hold members with the same name from different directories
# (libavutil's cpu.o and aarch64/cpu.o). With ZERO_AR_DATE dsymutil cannot tell
# them apart and drops one, so the slice is linked against copies of the
# archives whose members are named by their path in the build tree. Prints the
# archive's path.
unique_archive() {
    local name="$1" arch="$2" library="$3"
    local build="$WORK/build/$name-$arch" objects="$WORK/objects/$name-$arch/$library"
    local archive="$objects.a" members=() object relative
    rm -rf "$objects" "$archive"
    mkdir -p "$objects"
    while IFS= read -r object; do
        relative="${object#"$build/$library/"}"
        cp "$object" "$objects/${relative//\//_}"
        members+=("$objects/${relative//\//_}")
    done < <(find "$build/$library" -name '*.o' | LC_ALL=C sort)
    # The copies must be exactly the installed archive's members.
    local installed
    installed="$(ar t "$(install_dir "$name" "$arch")/lib/$library.a" | grep -cv '^__\.SYMDEF')"
    if [[ "$installed" != "${#members[@]}" ]]; then
        echo "error: $library for $name $arch has $installed members, the build tree ${#members[@]} objects" >&2
        exit 1
    fi
    if [[ -n "$(printf '%s\n' "${members[@]##*/}" | sort | uniq -d)" ]]; then
        echo "error: $library for $name $arch still has duplicate member names" >&2
        exit 1
    fi
    ar rcs "$archive" "${members[@]}"
    echo "$archive"
}

# Links the bridge and FFmpeg's archives for one slice into a dynamic library
# that exports only spector_ffmpeg_*: FFmpeg's own symbols stay inside it. The
# library keeps its debug map until make_framework has made the dSYM.
link_slice() {
    local name="$1" arch="$2" sdk="$3" triple="$4" platform="$5" output="$6"
    local prefix private clang sysroot bridge_object avcodec avutil
    prefix="$(install_dir "$name" "$arch")"
    avcodec="$(unique_archive "$name" "$arch" libavcodec)"
    avutil="$(unique_archive "$name" "$arch" libavutil)"
    # The system libraries FFmpeg's static archives need (-lm, -pthread).
    private="$(sed -n 's/^Libs.private: //p' "$prefix/lib/pkgconfig/libavcodec.pc" "$prefix/lib/pkgconfig/libavutil.pc" | tr '\n' ' ')"
    clang="$(xcrun --sdk "$sdk" -f clang)"
    sysroot="$(xcrun --sdk "$sdk" --show-sdk-path)"
    # Compiled on its own, so that the debug map points at an object file that
    # still exists when dsymutil reads it.
    bridge_object="$WORK/bridge/$name-$arch/$FRAMEWORK.o"
    mkdir -p "$(dirname "$output")" "$(dirname "$bridge_object")"
    "$clang" -target "$triple" -isysroot "$sysroot" \
        -c -g -O2 -Wall -Werror -fvisibility=hidden "${DEBUG_PREFIX_MAPS[@]}" \
        -I "$prefix/include" "$BRIDGE_SOURCE/$FRAMEWORK.c" -o "$bridge_object"
    # The debug map names its object files relative to the checkout
    # (-oso_prefix); dsymutil finds them with --oso-prepend-path.
    # shellcheck disable=SC2086
    "$clang" -target "$triple" -isysroot "$sysroot" -dynamiclib \
        "$bridge_object" "$avcodec" "$avutil" $private \
        -Wl,-exported_symbol,'_spector_ffmpeg_*' -Wl,-dead_strip \
        -Wl,-oso_prefix,"$PWD/" \
        -install_name "$(install_name "$platform")" \
        -compatibility_version 1 -current_version "$FFMPEG_VERSION" \
        -o "$output"
}

# Writes the dSYM for a framework's binary, then strips its debug map. The
# dSYM matches the binary by UUID, which stripping keeps.
make_dsym() {
    local binary="$1" dsym="$2" log
    rm -rf "$dsym"
    # Relative to the checkout, because the dSYM records the binary's path.
    log="$(dsymutil "${binary#"$PWD/"}" --oso-prepend-path="$PWD" -o "$dsym" 2>&1)"
    if [[ -n "$log" ]]; then
        echo "error: dsymutil reported problems for $binary:" >&2
        echo "$log" | head >&2
        exit 1
    fi
    strip -S "$binary"
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
    make_dsym "$contents/$FRAMEWORK" "$WORK/dsyms/$platform/$FRAMEWORK.framework.dSYM"
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

# Fails the build if the framework or a dSYM names the checkout's path, which
# would leak the build machine's paths and break the byte-identical rebuild.
check_no_build_paths() {
    local found
    found="$(grep -rlF "$PWD" "$1" || true)"
    if [[ -n "$found" ]]; then
        echo "error: the build directory's path is recorded in:" >&2
        echo "$found" >&2
        exit 1
    fi
}

# xcodebuild lists an XCFramework's slices in no fixed order. Sorting them is
# part of keeping a rebuild byte-identical, so `git status` can check it.
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
    arguments+=(-debug-symbols "$WORK/dsyms/$platform/$FRAMEWORK.framework.dSYM")
done
rm -rf "$OUT"/*.xcframework
xcodebuild -create-xcframework "${arguments[@]}" -output "$OUT/$FRAMEWORK.xcframework" > /dev/null
sort_xcframework_libraries "$OUT/$FRAMEWORK.xcframework/Info.plist"
check_no_build_paths "$OUT/$FRAMEWORK.xcframework"

cat > "$OUT/BUILD_INFO.txt" <<EOF
FFmpeg $FFMPEG_VERSION (libavcodec, libavutil; static, inside $FRAMEWORK.framework)
Source: $FFMPEG_URL
SHA-256: $FFMPEG_SHA256
Decoders: $DECODERS
Parsers: $PARSERS
Bridge: Scripts/ffmpeg-bridge/$FRAMEWORK.c, exports spector_ffmpeg_* only
Debug symbols: one dSYM per slice (dSYMs/$FRAMEWORK.framework.dSYM), paths under $DEBUG_ROOT
Configure flags (tvOS arm64):
$(configure_flags arm64 appletvos "arm64-apple-tvos${TVOS_MIN}" | sed 's/^/  /')
Built with: $(xcodebuild -version | paste -sd ' ' -)
EOF
echo "$FRAMEWORK $FFMPEG_VERSION written to $OUT"
