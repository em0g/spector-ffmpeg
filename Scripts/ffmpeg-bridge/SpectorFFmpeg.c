// The bridge between the engine and FFmpeg; see SpectorFFmpeg.h.
#include "SpectorFFmpeg.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>

// The header's channel bits are FFmpeg's.
_Static_assert(SPECTOR_FFMPEG_CHANNEL_FRONT_LEFT == AV_CH_FRONT_LEFT, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_FRONT_RIGHT == AV_CH_FRONT_RIGHT, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_FRONT_CENTER == AV_CH_FRONT_CENTER, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_LOW_FREQUENCY == AV_CH_LOW_FREQUENCY, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_BACK_LEFT == AV_CH_BACK_LEFT, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_BACK_RIGHT == AV_CH_BACK_RIGHT, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_FRONT_LEFT_OF_CENTER == AV_CH_FRONT_LEFT_OF_CENTER, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_FRONT_RIGHT_OF_CENTER == AV_CH_FRONT_RIGHT_OF_CENTER, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_BACK_CENTER == AV_CH_BACK_CENTER, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_SIDE_LEFT == AV_CH_SIDE_LEFT, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_SIDE_RIGHT == AV_CH_SIDE_RIGHT, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_TOP_CENTER == AV_CH_TOP_CENTER, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_TOP_FRONT_LEFT == AV_CH_TOP_FRONT_LEFT, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_TOP_FRONT_CENTER == AV_CH_TOP_FRONT_CENTER, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_TOP_FRONT_RIGHT == AV_CH_TOP_FRONT_RIGHT, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_TOP_BACK_LEFT == AV_CH_TOP_BACK_LEFT, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_TOP_BACK_CENTER == AV_CH_TOP_BACK_CENTER, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_TOP_BACK_RIGHT == AV_CH_TOP_BACK_RIGHT, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_WIDE_LEFT == AV_CH_WIDE_LEFT, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_WIDE_RIGHT == AV_CH_WIDE_RIGHT, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_SURROUND_DIRECT_LEFT == AV_CH_SURROUND_DIRECT_LEFT, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_SURROUND_DIRECT_RIGHT == AV_CH_SURROUND_DIRECT_RIGHT, "channel bit");
_Static_assert(SPECTOR_FFMPEG_CHANNEL_LOW_FREQUENCY_2 == AV_CH_LOW_FREQUENCY_2, "channel bit");

struct SpectorFFmpegAudioDecoder {
    AVCodecContext *context;
    AVPacket *packet;
    AVFrame *frame;
    float *pcm;
    size_t capacity;
    // DTS only: FFmpeg's parser, which names a frame's profile from its
    // headers without decoding it, with a context of its own.
    AVCodecParserContext *parser;
    AVCodecContext *parser_context;
    // Set once the stream turned out to be audio the engine does not decode.
    int refused;
    // TrueHD only: set until the first major sync after the start or a
    // flush. The decoder cannot decode an access unit before it, and logs a
    // warning for each, so they are not sent.
    int awaits_major_sync;
};

const char *spector_ffmpeg_license(void) { return avcodec_license(); }

const char *spector_ffmpeg_configuration(void) { return avcodec_configuration(); }

uint32_t spector_ffmpeg_avcodec_version(void) { return avcodec_version(); }

int spector_ffmpeg_has_decoder(const char *name) { return avcodec_find_decoder_by_name(name) != NULL; }

// Copies the container's codec configuration into the context, padded as
// FFmpeg requires.
static int set_extradata(AVCodecContext *context, const uint8_t *data, int32_t size) {
    if (size == 0) return 0;
    context->extradata = av_mallocz((size_t)size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (context->extradata == NULL) return AVERROR(ENOMEM);
    memcpy(context->extradata, data, (size_t)size);
    context->extradata_size = size;
    return 0;
}

static int open_dts_parser(SpectorFFmpegAudioDecoder *decoder) {
    decoder->parser = av_parser_init(AV_CODEC_ID_DTS);
    decoder->parser_context = avcodec_alloc_context3(NULL);
    if (decoder->parser == NULL || decoder->parser_context == NULL) return AVERROR(ENOMEM);
    // Each packet is one whole frame, as Matroska stores DTS.
    decoder->parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
    decoder->parser_context->codec_type = AVMEDIA_TYPE_AUDIO;
    decoder->parser_context->codec_id = AV_CODEC_ID_DTS;
    return 0;
}

SpectorFFmpegAudioDecoder *spector_ffmpeg_audio_decoder_create(
    SpectorFFmpegCodec codec, const uint8_t *extradata, int32_t extradata_size) {
    enum AVCodecID id;
    switch (codec) {
    case SpectorFFmpegCodecMP1: id = AV_CODEC_ID_MP1; break;
    case SpectorFFmpegCodecMP2: id = AV_CODEC_ID_MP2; break;
    case SpectorFFmpegCodecDTS: id = AV_CODEC_ID_DTS; break;
    case SpectorFFmpegCodecTrueHD: id = AV_CODEC_ID_TRUEHD; break;
    case SpectorFFmpegCodecVorbis: id = AV_CODEC_ID_VORBIS; break;
    case SpectorFFmpegCodecOpus: id = AV_CODEC_ID_OPUS; break;
    default: return NULL;
    }
    if (extradata_size < 0 || (extradata_size > 0 && extradata == NULL)) return NULL;
    const AVCodec *decoder = avcodec_find_decoder(id);
    if (decoder == NULL) return NULL;
    SpectorFFmpegAudioDecoder *result = calloc(1, sizeof(*result));
    if (result == NULL) return NULL;
    result->context = avcodec_alloc_context3(decoder);
    result->packet = av_packet_alloc();
    result->frame = av_frame_alloc();
    // Without the parser DTS Express could not be refused, so DTS is not
    // decoded at all.
    // FFmpeg would drop Opus's pre-skip (from its OpusHead) from the first
    // output; the engine places it before the clock's start itself
    // (Matroska's CodecDelay), as it does for Apple's Opus decoder, so every
    // sample is decoded. (Vorbis keeps FFmpeg's skip: its first packet's
    // samples, which only overlap the next block, are dropped.)
    if (result->context != NULL && id == AV_CODEC_ID_OPUS) result->context->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
    if (result->context == NULL || result->packet == NULL || result->frame == NULL
        || set_extradata(result->context, extradata, extradata_size) != 0
        || (id == AV_CODEC_ID_DTS && open_dts_parser(result) != 0)
        || avcodec_open2(result->context, decoder, NULL) != 0) {
        spector_ffmpeg_audio_decoder_free(result);
        return NULL;
    }
    result->awaits_major_sync = id == AV_CODEC_ID_TRUEHD;
    return result;
}

void spector_ffmpeg_audio_decoder_free(SpectorFFmpegAudioDecoder *decoder) {
    if (decoder == NULL) return;
    avcodec_free_context(&decoder->context);
    av_packet_free(&decoder->packet);
    av_frame_free(&decoder->frame);
    av_parser_close(decoder->parser);
    avcodec_free_context(&decoder->parser_context);
    free(decoder->pcm);
    free(decoder);
}

// Whether `packet` is DTS Express, the low bit rate coder (LBR), which the
// engine does not decode (ADR 0034): FFmpeg's parser names the profile from
// the frame's headers before the decoder sees it. The packet is padded, as
// the parser may read past its end.
static int is_dts_express(SpectorFFmpegAudioDecoder *decoder, const AVPacket *packet) {
    uint8_t *output = NULL;
    int output_size = 0;
    decoder->parser_context->profile = AV_PROFILE_UNKNOWN;
    av_parser_parse2(decoder->parser, decoder->parser_context, &output, &output_size, packet->data, packet->size,
                     AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    return decoder->parser_context->profile == AV_PROFILE_DTS_EXPRESS;
}

// The offset of the first TrueHD access unit in `data` that starts with a
// major sync, walking the units by their lengths; -1 if none does.
static int32_t first_major_sync(const uint8_t *data, int32_t size) {
    int32_t offset = 0;
    while (size - offset >= 8) {
        int32_t length = ((data[offset] & 0x0F) << 8 | data[offset + 1]) * 2;
        if (length < 4 || length > size - offset) return -1;
        // The sync word of TrueHD (0xBA) or MLP (0xBB), as FFmpeg reads it.
        uint32_t sync = (uint32_t)data[offset + 4] << 24 | (uint32_t)data[offset + 5] << 16
                        | (uint32_t)data[offset + 6] << 8 | data[offset + 7];
        if ((sync & 0xFFFFFFFE) == 0xF8726FBA) return offset;
        offset += length;
    }
    return -1;
}

int spector_ffmpeg_audio_decoder_send(SpectorFFmpegAudioDecoder *decoder, const uint8_t *data, int32_t size) {
    if (decoder->refused) return SPECTOR_FFMPEG_ERROR_REFUSED;
    if (decoder->awaits_major_sync) {
        int32_t start = first_major_sync(data, size);
        // Nothing to decode yet: as if the decoder took the packet and made
        // nothing of it.
        if (start < 0) return 0;
        decoder->awaits_major_sync = 0;
        data += start;
        size -= start;
    }
    int status = av_new_packet(decoder->packet, size);
    if (status != 0) return status;
    memcpy(decoder->packet->data, data, (size_t)size);
    if (decoder->parser != NULL && is_dts_express(decoder, decoder->packet)) {
        av_packet_unref(decoder->packet);
        return SPECTOR_FFMPEG_ERROR_REFUSED;
    }
    status = avcodec_send_packet(decoder->context, decoder->packet);
    av_packet_unref(decoder->packet);
    return status;
}

// Writes the frame as interleaved float into the decoder's buffer.
static int interleave(SpectorFFmpegAudioDecoder *decoder, const AVFrame *frame, int channels) {
    int count = frame->nb_samples;
    size_t needed = (size_t)count * (size_t)channels;
    if (needed > decoder->capacity) {
        float *grown = realloc(decoder->pcm, needed * sizeof(float));
        if (grown == NULL) return AVERROR(ENOMEM);
        decoder->pcm = grown;
        decoder->capacity = needed;
    }
    float *out = decoder->pcm;
    uint8_t *const *planes = frame->extended_data;
    switch (frame->format) {
    case AV_SAMPLE_FMT_FLTP:
        for (int index = 0; index < count; index++)
            for (int channel = 0; channel < channels; channel++)
                *out++ = ((const float *)planes[channel])[index];
        break;
    case AV_SAMPLE_FMT_FLT:
        memcpy(out, planes[0], needed * sizeof(float));
        break;
    case AV_SAMPLE_FMT_S16P:
        for (int index = 0; index < count; index++)
            for (int channel = 0; channel < channels; channel++)
                *out++ = ((const int16_t *)planes[channel])[index] / 32768.0f;
        break;
    case AV_SAMPLE_FMT_S16:
        for (size_t index = 0; index < needed; index++) out[index] = ((const int16_t *)planes[0])[index] / 32768.0f;
        break;
    // 24-bit audio, such as DTS-HD Master Audio's, in the top bits.
    case AV_SAMPLE_FMT_S32P:
        for (int index = 0; index < count; index++)
            for (int channel = 0; channel < channels; channel++)
                *out++ = ((const int32_t *)planes[channel])[index] / 2147483648.0f;
        break;
    case AV_SAMPLE_FMT_S32:
        for (size_t index = 0; index < needed; index++)
            out[index] = ((const int32_t *)planes[0])[index] / 2147483648.0f;
        break;
    default:
        return SPECTOR_FFMPEG_ERROR_SAMPLE_FORMAT;
    }
    return 0;
}

int spector_ffmpeg_audio_decoder_receive(SpectorFFmpegAudioDecoder *decoder, SpectorFFmpegPCM *pcm) {
    for (;;) {
        int status = avcodec_receive_frame(decoder->context, decoder->frame);
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return 0;
        if (status != 0) return status;
        AVFrame *frame = decoder->frame;
        // A DTS Express asset beside a core, which the parser does not name:
        // nothing more of the stream is decoded.
        if (decoder->parser != NULL && decoder->context->profile == AV_PROFILE_DTS_EXPRESS) {
            decoder->refused = 1;
            av_frame_unref(frame);
            return SPECTOR_FFMPEG_ERROR_REFUSED;
        }
        int channels = frame->ch_layout.nb_channels;
        if (frame->nb_samples <= 0 || channels <= 0 || frame->sample_rate <= 0 || frame->extended_data == NULL) {
            av_frame_unref(frame);
            continue;
        }
        status = interleave(decoder, frame, channels);
        pcm->samples = decoder->pcm;
        pcm->frame_count = frame->nb_samples;
        pcm->channels = channels;
        pcm->sample_rate = frame->sample_rate;
        pcm->channel_mask = frame->ch_layout.order == AV_CHANNEL_ORDER_NATIVE ? frame->ch_layout.u.mask : 0;
        av_frame_unref(frame);
        return status == 0 ? 1 : status;
    }
}

void spector_ffmpeg_audio_decoder_flush(SpectorFFmpegAudioDecoder *decoder) {
    avcodec_flush_buffers(decoder->context);
    decoder->awaits_major_sync = decoder->context->codec_id == AV_CODEC_ID_TRUEHD;
}

// MARK: Video (ADR 0068)

_Static_assert(SPECTOR_FFMPEG_NO_TIME == AV_NOPTS_VALUE, "no time");

struct SpectorFFmpegVideoDecoder {
    AVCodecContext *context;
    AVPacket *packet;
    // The picture `_receive` returned last, kept until the next call so that
    // its planes stay valid.
    AVFrame *frame;
};

SpectorFFmpegVideoDecoder *spector_ffmpeg_video_decoder_create(
    SpectorFFmpegVideoCodec codec, const uint8_t *extradata, int32_t extradata_size, int32_t threads) {
    enum AVCodecID id;
    switch (codec) {
    case SpectorFFmpegVideoCodecMPEG1: id = AV_CODEC_ID_MPEG1VIDEO; break;
    case SpectorFFmpegVideoCodecMPEG2: id = AV_CODEC_ID_MPEG2VIDEO; break;
    default: return NULL;
    }
    if (extradata_size < 0 || (extradata_size > 0 && extradata == NULL) || threads < 1) return NULL;
    const AVCodec *decoder = avcodec_find_decoder(id);
    if (decoder == NULL) return NULL;
    SpectorFFmpegVideoDecoder *result = calloc(1, sizeof(*result));
    if (result == NULL) return NULL;
    result->context = avcodec_alloc_context3(decoder);
    result->packet = av_packet_alloc();
    result->frame = av_frame_alloc();
    if (result->context != NULL) {
        // Slice threads only: a frame thread holds a picture back, which live
        // cannot afford (ADR 0068).
        result->context->thread_type = FF_THREAD_SLICE;
        result->context->thread_count = threads;
    }
    if (result->context == NULL || result->packet == NULL || result->frame == NULL
        || set_extradata(result->context, extradata, extradata_size) != 0
        || avcodec_open2(result->context, decoder, NULL) != 0) {
        spector_ffmpeg_video_decoder_free(result);
        return NULL;
    }
    return result;
}

void spector_ffmpeg_video_decoder_free(SpectorFFmpegVideoDecoder *decoder) {
    if (decoder == NULL) return;
    avcodec_free_context(&decoder->context);
    av_packet_free(&decoder->packet);
    av_frame_free(&decoder->frame);
    free(decoder);
}

int spector_ffmpeg_video_decoder_send(
    SpectorFFmpegVideoDecoder *decoder, const uint8_t *data, int32_t size, int64_t time) {
    if (size <= 0 || data == NULL) return AVERROR(EINVAL);
    int status = av_new_packet(decoder->packet, size);
    if (status != 0) return status;
    memcpy(decoder->packet->data, data, (size_t)size);
    decoder->packet->pts = time;
    status = avcodec_send_packet(decoder->context, decoder->packet);
    av_packet_unref(decoder->packet);
    return status;
}

int spector_ffmpeg_video_decoder_send_end(SpectorFFmpegVideoDecoder *decoder) {
    int status = avcodec_send_packet(decoder->context, NULL);
    return status == AVERROR_EOF ? 0 : status;
}

int spector_ffmpeg_video_decoder_receive(SpectorFFmpegVideoDecoder *decoder, SpectorFFmpegPicture *picture) {
    av_frame_unref(decoder->frame);
    int status = avcodec_receive_frame(decoder->context, decoder->frame);
    if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return 0;
    if (status != 0) return status;
    const AVFrame *frame = decoder->frame;
    const AVPixFmtDescriptor *format = av_pix_fmt_desc_get(frame->format);
    if (format == NULL || format->nb_components != 3 || (format->flags & AV_PIX_FMT_FLAG_PLANAR) == 0
        || (format->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) != 0 || format->comp[0].depth != 8
        || format->log2_chroma_w > 1 || format->log2_chroma_h > 1 || frame->width <= 0 || frame->height <= 0) {
        av_frame_unref(decoder->frame);
        return SPECTOR_FFMPEG_ERROR_PIXEL_FORMAT;
    }
    *picture = (SpectorFFmpegPicture){0};
    for (int plane = 0; plane < 3; plane++) {
        picture->planes[plane] = frame->data[plane];
        picture->strides[plane] = frame->linesize[plane];
    }
    picture->width = frame->width;
    picture->height = frame->height;
    picture->chroma_shift_x = format->log2_chroma_w;
    picture->chroma_shift_y = format->log2_chroma_h;
    picture->time = frame->pts;
    picture->rate_numerator = decoder->context->framerate.num;
    picture->rate_denominator = decoder->context->framerate.den;
    picture->repeated_half_periods = frame->repeat_pict;
    picture->aspect_numerator = frame->sample_aspect_ratio.num;
    picture->aspect_denominator = frame->sample_aspect_ratio.den;
    picture->colour_primaries = frame->color_primaries;
    picture->transfer = frame->color_trc;
    picture->matrix = frame->colorspace;
    picture->full_range = frame->color_range == AVCOL_RANGE_JPEG;
    picture->interlaced = (frame->flags & AV_FRAME_FLAG_INTERLACED) != 0;
    picture->top_field_first = (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0;
    picture->key_frame = frame->pict_type == AV_PICTURE_TYPE_I;
    return 1;
}

void spector_ffmpeg_video_decoder_flush(SpectorFFmpegVideoDecoder *decoder) {
    av_frame_unref(decoder->frame);
    avcodec_flush_buffers(decoder->context);
}

// Averages two rows of `count` bytes, rounding up as MPEG's chroma filters do.
static void average_rows(const uint8_t *first, const uint8_t *second, uint8_t *output, int32_t count) {
    for (int32_t index = 0; index < count; index++) output[index] = (uint8_t)((first[index] + second[index] + 1) >> 1);
}

// The source chroma row for 4:2:0 row `row` and its partner, where chroma
// has as many rows as luma (4:2:2, 4:4:4): rows 2r and 2r + 1 for a frame;
// within each field for an interlaced picture, whose 4:2:0 rows alternate
// between the fields as its luma rows do.
static void source_rows(int32_t row, int32_t rows, int interlaced, int32_t *first, int32_t *second) {
    if (interlaced) {
        int32_t field = row & 1, pair = row >> 1;
        *first = pair * 4 + field;
        *second = *first + 2;
    } else {
        *first = row * 2;
        *second = *first + 1;
    }
    if (*first >= rows) *first = rows - 1;
    if (*second >= rows) *second = *first;
}

int spector_ffmpeg_picture_copy_biplanar(
    const SpectorFFmpegPicture *picture, uint8_t *luma, int32_t luma_stride, uint8_t *chroma, int32_t chroma_stride) {
    int32_t shift_x = picture->chroma_shift_x, shift_y = picture->chroma_shift_y;
    if (shift_x < 0 || shift_x > 1 || shift_y < 0 || shift_y > 1 || picture->width <= 0 || picture->height <= 0)
        return SPECTOR_FFMPEG_ERROR_PIXEL_FORMAT;
    int32_t width = picture->width, height = picture->height;
    for (int32_t row = 0; row < height; row++)
        memcpy(luma + (size_t)row * (size_t)luma_stride, picture->planes[0] + (ptrdiff_t)row * picture->strides[0],
               (size_t)width);

    int32_t pairs = (width + 1) / 2, rows = (height + 1) / 2;
    int32_t source_width = (width + (1 << shift_x) - 1) >> shift_x;
    int32_t source_rows_count = (height + (1 << shift_y) - 1) >> shift_y;
    // A chroma row of each plane, averaged vertically or horizontally where
    // needed, on the stack (24 KB). MPEG-2's widest level is 1,920 pixels.
    if (width > 8192) return SPECTOR_FFMPEG_ERROR_PIXEL_FORMAT;
    uint8_t cb[4096], cr[4096], cb_wide[8192], cr_wide[8192];
    for (int32_t row = 0; row < rows; row++) {
        const uint8_t *cb_source, *cr_source;
        if (shift_y == 1) {
            cb_source = picture->planes[1] + (ptrdiff_t)row * picture->strides[1];
            cr_source = picture->planes[2] + (ptrdiff_t)row * picture->strides[2];
        } else {
            int32_t first, second;
            source_rows(row, source_rows_count, picture->interlaced, &first, &second);
            average_rows(picture->planes[1] + (ptrdiff_t)first * picture->strides[1],
                         picture->planes[1] + (ptrdiff_t)second * picture->strides[1], cb_wide, source_width);
            average_rows(picture->planes[2] + (ptrdiff_t)first * picture->strides[2],
                         picture->planes[2] + (ptrdiff_t)second * picture->strides[2], cr_wide, source_width);
            cb_source = cb_wide;
            cr_source = cr_wide;
        }
        if (shift_x == 0) {
            // 4:4:4: average the columns in pairs, the last alone.
            for (int32_t pair = 0; pair < pairs; pair++) {
                int32_t left = pair * 2, right = left + 1 < source_width ? left + 1 : left;
                cb[pair] = (uint8_t)((cb_source[left] + cb_source[right] + 1) >> 1);
                cr[pair] = (uint8_t)((cr_source[left] + cr_source[right] + 1) >> 1);
            }
            cb_source = cb;
            cr_source = cr;
        }
        uint8_t *output = chroma + (size_t)row * (size_t)chroma_stride;
        for (int32_t pair = 0; pair < pairs; pair++) {
            output[2 * pair] = cb_source[pair];
            output[2 * pair + 1] = cr_source[pair];
        }
    }
    return 0;
}
