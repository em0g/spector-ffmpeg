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
    /// DTS Coherent Acoustics: the core, ES, 96/24 and DTS-HD (High
    /// Resolution, Master Audio). DTS Express is refused
    /// (`SPECTOR_FFMPEG_ERROR_REFUSED`, ADR 0034).
    SpectorFFmpegCodecDTS = 3,
} SpectorFFmpegCodec;

/// `spector_ffmpeg_audio_decoder_receive` result for a sample format the
/// bridge does not convert. Negative, like FFmpeg's own errors.
#define SPECTOR_FFMPEG_ERROR_SAMPLE_FORMAT (-0x46535053)

/// `spector_ffmpeg_audio_decoder_send` and `_receive` result for audio the
/// engine does not decode although FFmpeg could, such as DTS Express.
#define SPECTOR_FFMPEG_ERROR_REFUSED (-0x46535052)

/// The channels of decoded audio, as bits of `SpectorFFmpegPCM.channel_mask`.
/// They are FFmpeg's own (`AV_CH_*`; the bridge checks that at compile time).
#define SPECTOR_FFMPEG_CHANNEL_FRONT_LEFT (1ULL << 0)
#define SPECTOR_FFMPEG_CHANNEL_FRONT_RIGHT (1ULL << 1)
#define SPECTOR_FFMPEG_CHANNEL_FRONT_CENTER (1ULL << 2)
#define SPECTOR_FFMPEG_CHANNEL_LOW_FREQUENCY (1ULL << 3)
#define SPECTOR_FFMPEG_CHANNEL_BACK_LEFT (1ULL << 4)
#define SPECTOR_FFMPEG_CHANNEL_BACK_RIGHT (1ULL << 5)
#define SPECTOR_FFMPEG_CHANNEL_FRONT_LEFT_OF_CENTER (1ULL << 6)
#define SPECTOR_FFMPEG_CHANNEL_FRONT_RIGHT_OF_CENTER (1ULL << 7)
#define SPECTOR_FFMPEG_CHANNEL_BACK_CENTER (1ULL << 8)
#define SPECTOR_FFMPEG_CHANNEL_SIDE_LEFT (1ULL << 9)
#define SPECTOR_FFMPEG_CHANNEL_SIDE_RIGHT (1ULL << 10)
#define SPECTOR_FFMPEG_CHANNEL_TOP_CENTER (1ULL << 11)
#define SPECTOR_FFMPEG_CHANNEL_TOP_FRONT_LEFT (1ULL << 12)
#define SPECTOR_FFMPEG_CHANNEL_TOP_FRONT_CENTER (1ULL << 13)
#define SPECTOR_FFMPEG_CHANNEL_TOP_FRONT_RIGHT (1ULL << 14)
#define SPECTOR_FFMPEG_CHANNEL_TOP_BACK_LEFT (1ULL << 15)
#define SPECTOR_FFMPEG_CHANNEL_TOP_BACK_CENTER (1ULL << 16)
#define SPECTOR_FFMPEG_CHANNEL_TOP_BACK_RIGHT (1ULL << 17)
#define SPECTOR_FFMPEG_CHANNEL_WIDE_LEFT (1ULL << 31)
#define SPECTOR_FFMPEG_CHANNEL_WIDE_RIGHT (1ULL << 32)
#define SPECTOR_FFMPEG_CHANNEL_SURROUND_DIRECT_LEFT (1ULL << 33)
#define SPECTOR_FFMPEG_CHANNEL_SURROUND_DIRECT_RIGHT (1ULL << 34)
#define SPECTOR_FFMPEG_CHANNEL_LOW_FREQUENCY_2 (1ULL << 35)

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
    /// The channels in the order of `samples`: one `SPECTOR_FFMPEG_CHANNEL_*`
    /// bit each, in ascending order of the bits (FFmpeg's native order).
    /// 0 when the decoder names no such layout; the channels are then only
    /// counted.
    uint64_t channel_mask;
} SpectorFFmpegPCM;

/// Returns NULL if FFmpeg has no such decoder or it cannot be opened.
/// `extradata` is the codec's configuration from the container (such as
/// Vorbis's headers), copied; NULL with size 0 when the codec needs none.
SPECTOR_FFMPEG_EXPORT SpectorFFmpegAudioDecoder *spector_ffmpeg_audio_decoder_create(
    SpectorFFmpegCodec codec, const uint8_t *extradata, int32_t extradata_size);

/// Frees the decoder; NULL is ignored.
SPECTOR_FFMPEG_EXPORT void spector_ffmpeg_audio_decoder_free(SpectorFFmpegAudioDecoder *decoder);

/// Sends one compressed packet. Returns 0, a negative FFmpeg error, or
/// `SPECTOR_FFMPEG_ERROR_REFUSED` for a packet the engine does not decode,
/// which the decoder never sees.
SPECTOR_FFMPEG_EXPORT int spector_ffmpeg_audio_decoder_send(
    SpectorFFmpegAudioDecoder *decoder, const uint8_t *data, int32_t size);

/// Returns 1 with the next decoded frame in `pcm`, 0 when the decoder needs
/// more input, or a negative FFmpeg error (or `SPECTOR_FFMPEG_ERROR_SAMPLE_FORMAT`,
/// `SPECTOR_FFMPEG_ERROR_REFUSED`).
SPECTOR_FFMPEG_EXPORT int spector_ffmpeg_audio_decoder_receive(SpectorFFmpegAudioDecoder *decoder, SpectorFFmpegPCM *pcm);

/// Drops buffered data, for example after a discontinuity.
SPECTOR_FFMPEG_EXPORT void spector_ffmpeg_audio_decoder_flush(SpectorFFmpegAudioDecoder *decoder);

#endif
