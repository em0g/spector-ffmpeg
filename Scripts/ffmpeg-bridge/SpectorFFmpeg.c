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
    if (result->context == NULL || result->packet == NULL || result->frame == NULL
        || set_extradata(result->context, extradata, extradata_size) != 0
        || (id == AV_CODEC_ID_DTS && open_dts_parser(result) != 0)
        || avcodec_open2(result->context, decoder, NULL) != 0) {
        spector_ffmpeg_audio_decoder_free(result);
        return NULL;
    }
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

int spector_ffmpeg_audio_decoder_send(SpectorFFmpegAudioDecoder *decoder, const uint8_t *data, int32_t size) {
    if (decoder->refused) return SPECTOR_FFMPEG_ERROR_REFUSED;
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
}
