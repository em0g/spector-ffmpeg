# SPECTOR FFmpeg

The source that SPECTOR's users need to rebuild and replace `SpectorFFmpeg.framework`, the FFmpeg component inside the SPECTOR app for Apple TV.

`SpectorFFmpeg.framework` holds libavcodec and libavutil from [FFmpeg](https://ffmpeg.org/) 9.0.2, licensed under the GNU Lesser General Public License version 2.1 or later, linked statically together with a small C bridge (`Scripts/ffmpeg-bridge`). The bridge exports only `spector_ffmpeg_*` functions. The app links the framework dynamically. To meet the LGPL (section 6), this repository publishes the bridge's source and the build script, so you can rebuild the framework with a modified FFmpeg and put it in the app in place of the original.

FFmpeg is a trademark of Fabrice Bellard, originator of the FFmpeg project.

## Contents

| Path | What it is |
|---|---|
| `Scripts/ffmpeg-bridge/SpectorFFmpeg.h`, `SpectorFFmpeg.c` | The bridge: the app's only entry points into FFmpeg |
| `Scripts/build-ffmpeg.sh` | Downloads FFmpeg's source, checks its SHA-256, configures it LGPL-only, and builds `Vendor/FFmpeg/SpectorFFmpeg.xcframework` (tvOS, iOS, their simulators, macOS) |
| `BUILD_INFO.txt` | The FFmpeg version, decoders and configure flags of the shipped framework |
| `LICENSES/FFmpeg-LGPL-2.1.txt` | The GNU Lesser General Public License, version 2.1 |

## FFmpeg's source

<https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz>, SHA-256 `8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e`, built unmodified with the configure flags in `Scripts/build-ffmpeg.sh` and `BUILD_INFO.txt`.

## Rebuilding

On a Mac with Xcode:

```sh
Scripts/build-ffmpeg.sh --clean
```

The result is in `Vendor/FFmpeg/SpectorFFmpeg.xcframework`. From `engine-0.2.1` on, built unmodified with the Xcode named in `BUILD_INFO.txt`, it is byte-identical to the shipped framework from any directory. (`engine-0.2.0`'s framework differs only in FFmpeg's configuration string, which recorded the build directory.) From `engine-0.6.0` on, each slice also has a dSYM (`dSYMs/SpectorFFmpeg.framework.dSYM`), byte-identical as well, with its source paths under `/spector-ffmpeg-build`. To use a modified FFmpeg, change the source the script builds (for example, point it at your own tree), rebuild, and replace `SpectorFFmpeg.framework` in the app bundle with the matching slice. Then re-sign the app for your device.

## Terms for the bridge

The bridge is SPECTOR's own code, published here so that the LGPL's requirements can be met. You may modify it and rebuild it with FFmpeg, for your own use, to make and use a replacement `SpectorFFmpeg.framework` with SPECTOR. You may also reverse-engineer the framework to debug such modifications. FFmpeg's own code stays under the LGPL-2.1-or-later.

## Versions

Each tag matches the SPECTOR Engine release that first shipped its framework:

- `engine-0.2.0`: SPECTOR Engine 0.2.0.
- `engine-0.2.1`: SPECTOR Engine 0.2.1 to 0.5.1, which shipped the same framework.
- `engine-0.6.0`: SPECTOR Engine 0.6.0 and 0.7.0. The framework was rebuilt with Xcode 27.1 and carries dSYMs. FFmpeg, its configuration and the bridge are unchanged.
- `engine-0.8.0`: SPECTOR Engine 0.8.0 and 0.8.1. Adds FFmpeg's DTS decoder and parser (`dca`); the bridge passes a codec's extradata, converts 32-bit samples and reports the channel layout it decodes. FFmpeg 9.0.2 and Xcode 27.1 as before.
- `engine-0.8.2`: SPECTOR Engine 0.8.2 on. Adds iOS and iOS Simulator slices, for the iPhone app. FFmpeg, its configuration, the bridge and the tvOS and macOS slices are unchanged from `engine-0.8.0`.
