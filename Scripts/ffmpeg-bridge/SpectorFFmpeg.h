#ifndef SPECTOR_FFMPEG_H
#define SPECTOR_FFMPEG_H

// The engine's only entry points into FFmpeg (ADR 0017, ADR 0020).
// Scripts/build-ffmpeg.sh links this bridge and FFmpeg's static libavcodec and
// libavutil into SpectorFFmpeg.framework, which exports only these functions,
// so its FFmpeg never clashes with another FFmpeg in the same app (such as
// AetherEngine's). The header includes none of FFmpeg's headers, so clients
// never load FFmpeg's modules.
//
// Because FFmpeg is linked statically into the framework, the bridge's source
// (this file and SpectorFFmpeg.c) is given to the app's users with FFmpeg's
// source and the build script, so that they can rebuild and replace the
// framework (LGPL-2.1 section 6, docs/licensing.md).

#include <stdint.h>

#define SPECTOR_FFMPEG_EXPORT __attribute__((visibility("default")))

/// The codecs the engine decodes with FFmpeg.
typedef enum {
    SpectorFFmpegCodecMP1 = 1,
    SpectorFFmpegCodecMP2 = 2,
} SpectorFFmpegCodec;

/// `spector_ffmpeg_audio_decoder_receive` result for a sample format the
/// bridge does not convert. Negative, like FFmpeg's own errors.
#define SPECTOR_FFMPEG_ERROR_SAMPLE_FORMAT (-0x46535053)

/// libavcodec's licence, for example "LGPL version 2.1 or later".
SPECTOR_FFMPEG_EXPORT const char *spector_ffmpeg_license(void);

/// The configure flags libavcodec was built with.
SPECTOR_FFMPEG_EXPORT const char *spector_ffmpeg_configuration(void);

/// libavcodec's version, `major << 16 | minor << 8 | micro`.
SPECTOR_FFMPEG_EXPORT uint32_t spector_ffmpeg_avcodec_version(void);

/// Whether libavcodec has a decoder called `name` (FFmpeg's names: "mp2", "h264").
SPECTOR_FFMPEG_EXPORT int spector_ffmpeg_has_decoder(const char *name);

/// An open audio decoder. Not thread-safe: one queue uses it at a time.
typedef struct SpectorFFmpegAudioDecoder SpectorFFmpegAudioDecoder;

/// Decoded audio as interleaved 32-bit float.
typedef struct {
    /// `frame_count * channels` values, owned by the decoder and valid until
    /// its next call.
    const float *samples;
    int32_t frame_count;
    int32_t channels;
    int32_t sample_rate;
} SpectorFFmpegPCM;

/// Returns NULL if FFmpeg has no such decoder or it cannot be opened.
SPECTOR_FFMPEG_EXPORT SpectorFFmpegAudioDecoder *spector_ffmpeg_audio_decoder_create(SpectorFFmpegCodec codec);

/// Frees the decoder; NULL is ignored.
SPECTOR_FFMPEG_EXPORT void spector_ffmpeg_audio_decoder_free(SpectorFFmpegAudioDecoder *decoder);

/// Sends one compressed packet. Returns 0 or a negative FFmpeg error.
SPECTOR_FFMPEG_EXPORT int spector_ffmpeg_audio_decoder_send(
    SpectorFFmpegAudioDecoder *decoder, const uint8_t *data, int32_t size);

/// Returns 1 with the next decoded frame in `pcm`, 0 when the decoder needs
/// more input, or a negative FFmpeg error (or `SPECTOR_FFMPEG_ERROR_SAMPLE_FORMAT`).
SPECTOR_FFMPEG_EXPORT int spector_ffmpeg_audio_decoder_receive(SpectorFFmpegAudioDecoder *decoder, SpectorFFmpegPCM *pcm);

/// Drops buffered data, for example after a discontinuity.
SPECTOR_FFMPEG_EXPORT void spector_ffmpeg_audio_decoder_flush(SpectorFFmpegAudioDecoder *decoder);

#endif
