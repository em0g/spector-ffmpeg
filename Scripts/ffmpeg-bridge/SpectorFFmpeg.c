// The bridge between the engine and FFmpeg; see SpectorFFmpeg.h.
#include "SpectorFFmpeg.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>

struct SpectorFFmpegAudioDecoder {
    AVCodecContext *context;
    AVPacket *packet;
    AVFrame *frame;
    float *pcm;
    size_t capacity;
};

const char *spector_ffmpeg_license(void) { return avcodec_license(); }

const char *spector_ffmpeg_configuration(void) { return avcodec_configuration(); }

uint32_t spector_ffmpeg_avcodec_version(void) { return avcodec_version(); }

int spector_ffmpeg_has_decoder(const char *name) { return avcodec_find_decoder_by_name(name) != NULL; }

SpectorFFmpegAudioDecoder *spector_ffmpeg_audio_decoder_create(SpectorFFmpegCodec codec) {
    enum AVCodecID id;
    switch (codec) {
    case SpectorFFmpegCodecMP1: id = AV_CODEC_ID_MP1; break;
    case SpectorFFmpegCodecMP2: id = AV_CODEC_ID_MP2; break;
    default: return NULL;
    }
    const AVCodec *decoder = avcodec_find_decoder(id);
    if (decoder == NULL) return NULL;
    SpectorFFmpegAudioDecoder *result = calloc(1, sizeof(*result));
    if (result == NULL) return NULL;
    result->context = avcodec_alloc_context3(decoder);
    result->packet = av_packet_alloc();
    result->frame = av_frame_alloc();
    if (result->context == NULL || result->packet == NULL || result->frame == NULL
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
    free(decoder->pcm);
    free(decoder);
}

int spector_ffmpeg_audio_decoder_send(SpectorFFmpegAudioDecoder *decoder, const uint8_t *data, int32_t size) {
    int status = av_new_packet(decoder->packet, size);
    if (status != 0) return status;
    memcpy(decoder->packet->data, data, (size_t)size);
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
        int channels = frame->ch_layout.nb_channels;
        if (frame->nb_samples <= 0 || channels <= 0 || frame->extended_data == NULL) {
            av_frame_unref(frame);
            continue;
        }
        status = interleave(decoder, frame, channels);
        pcm->samples = decoder->pcm;
        pcm->frame_count = frame->nb_samples;
        pcm->channels = channels;
        pcm->sample_rate = frame->sample_rate;
        av_frame_unref(frame);
        return status == 0 ? 1 : status;
    }
}

void spector_ffmpeg_audio_decoder_flush(SpectorFFmpegAudioDecoder *decoder) {
    avcodec_flush_buffers(decoder->context);
}
