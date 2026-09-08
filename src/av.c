/* SPDX-License-Identifier: GPL-2.0-only */
#include "av.h"
#include "fs.h"
#include "media.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if SNAJPAGENT_AV
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <pthread.h>

struct input {
    int fd, cancelled;
    uint64_t started, read_bytes;
    int64_t size;
    int (*pump)(void *, unsigned int);
    void *opaque;
    AVIOContext *io;
    AVFormatContext *format;
    AVCodecContext *decoder;
    AVPacket *packet;
    AVFrame *frame;
    int stream;
    bool draining, still;
};

static pthread_once_t log_once = PTHREAD_ONCE_INIT;
static void quiet_log(void) { av_log_set_level(AV_LOG_QUIET); }

static int
interrupted(void *opaque)
{
    struct input *in = opaque;
    if (!in->cancelled && in->pump) in->cancelled = in->pump(in->opaque, 0u);
    if (!in->cancelled && snag_monotonic_ms() - in->started >= 60000u) in->cancelled = -1;
    return in->cancelled != 0;
}

static int
input_read(void *opaque, uint8_t *data, int size)
{
    struct input *in = opaque;
    if (interrupted(in)) return AVERROR_EXIT;
    if (size < 0 || (uint64_t)size > 2u * SNAG_MEDIA_FILE_MAX - in->read_bytes) return AVERROR(EFBIG);
    ssize_t n;
    do { n = read(in->fd, data, (size_t)size); } while (n < 0 && errno == EINTR);
    if (n < 0) return AVERROR(errno);
    if (!n) return AVERROR_EOF;
    in->read_bytes += (uint64_t)n;
    return (int)n;
}

static int64_t
input_seek(void *opaque, int64_t offset, int whence)
{
    struct input *in = opaque;
    if (interrupted(in)) return AVERROR_EXIT;
    if (whence == AVSEEK_SIZE) return in->size;
    whence &= ~AVSEEK_FORCE;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_END ? in->size :
        whence == SEEK_CUR ? snag_seek(in->fd, 0, SEEK_CUR) : -1;
    if (base < 0 || offset < -base || offset > in->size - base) return AVERROR(EINVAL);
    int64_t result = snag_seek(in->fd, base + offset, SEEK_SET);
    return result < 0 ? AVERROR(errno) : result;
}

static int
deny_open(AVFormatContext *format, AVIOContext **io, const char *url, int flags, AVDictionary **options)
{
    (void)format; (void)io; (void)url; (void)flags; (void)options;
    return AVERROR(EACCES);
}

static void
input_close(struct input *in)
{
    av_frame_free(&in->frame); av_packet_free(&in->packet);
    avcodec_free_context(&in->decoder); avformat_close_input(&in->format);
    if (in->io) av_freep(&in->io->buffer);
    avio_context_free(&in->io);
    if (in->fd >= 0) close(in->fd);
}

static int
input_open(struct input *in, const char *path, enum AVMediaType type)
{
    snag_file_info st;
    AVDictionary *options = NULL;
    pthread_once(&log_once, quiet_log);
    in->started = snag_monotonic_ms();
    in->fd = snag_open_inspect_path("/", path);
    if (in->fd < 0 || snag_fstat(in->fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || (uint64_t)st.st_size > SNAG_MEDIA_FILE_MAX) return AVERROR(EINVAL);
    in->size = st.st_size;
    uint8_t *buffer = av_malloc(32768u);
    if (!buffer) return AVERROR(ENOMEM);
    in->io = avio_alloc_context(buffer, 32768, 0, in, input_read, NULL, input_seek);
    if (!in->io) { av_free(buffer); return AVERROR(ENOMEM); }
    in->format = avformat_alloc_context();
    if (!in->format) return AVERROR(ENOMEM);
    in->format->pb = in->io;
    in->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    in->format->io_open = deny_open;
    in->format->interrupt_callback = (AVIOInterruptCB){interrupted, in};
    /* No playlists, devices, URLs, concatenation or nested file protocols. */
    av_dict_set(&options, "format_whitelist", in->still ?
        "png_pipe,jpeg_pipe,gif,webp_pipe,bmp_pipe,tiff_pipe,apng" : "wav,mp3,mov,ogg,flac,matroska,webm", 0);
    av_dict_set(&options, "protocol_whitelist", "", 0);
    av_dict_set(&options, "probesize", "4194304", 0);
    av_dict_set(&options, "analyzeduration", "1000000", 0);
    av_dict_set(&options, "max_streams", "16", 0);
    int rc = avformat_open_input(&in->format, NULL, NULL, &options);
    av_dict_free(&options);
    if (rc < 0) return rc;
    /* Header parameters suffice for selecting a stream; avoid speculative
     * decoder allocation by avformat_find_stream_info before our limits. */
    in->stream = -1;
    for (unsigned int i = 0; i < in->format->nb_streams; ++i)
        if (in->format->streams[i]->codecpar->codec_type == type) { in->stream = (int)i; break; }
    if (in->stream < 0) return AVERROR_STREAM_NOT_FOUND;
    AVCodecParameters *par = in->format->streams[in->stream]->codecpar;
    if (type == AVMEDIA_TYPE_AUDIO && (par->ch_layout.nb_channels > 2 || par->sample_rate > 192000))
        return AVERROR(ENOTSUP);
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) return AVERROR_DECODER_NOT_FOUND;
    in->decoder = avcodec_alloc_context3(codec);
    if (!in->decoder) return AVERROR(ENOMEM);
    rc = avcodec_parameters_to_context(in->decoder, par);
    if (rc < 0) return rc;
    in->decoder->thread_count = 1;
    in->decoder->max_pixels = 16777216;
    in->decoder->max_samples = 1048576;
    rc = avcodec_open2(in->decoder, codec, NULL);
    if (rc < 0) return rc;
    in->frame = av_frame_alloc(); in->packet = av_packet_alloc();
    return in->frame && in->packet ? 0 : AVERROR(ENOMEM);
}

/* Pull one decoded frame; drain each submitted packet before reading another. */
static int
next_frame(struct input *in)
{
    av_frame_unref(in->frame);
    for (;;) {
        if (interrupted(in)) return AVERROR_EXIT;
        int rc = avcodec_receive_frame(in->decoder, in->frame);
        if (rc != AVERROR(EAGAIN)) return rc;
        if (in->draining) return AVERROR_EOF;
        do {
            av_packet_unref(in->packet);
            rc = av_read_frame(in->format, in->packet);
        } while (rc >= 0 && in->packet->stream_index != in->stream && !interrupted(in));
        if (rc == AVERROR_EOF) in->draining = true;
        else if (rc < 0) return rc;
        else if (in->packet->size > 16 * 1024 * 1024) return AVERROR(EFBIG);
        rc = avcodec_send_packet(in->decoder, in->draining ? NULL : in->packet);
        if (rc < 0) return rc;
    }
}

static int
failure(struct input *in, int code, char *error, size_t size)
{
    char detail[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(code, detail, sizeof(detail));
    snag_errorf(error, size, "Linked media decoding %s: %s",
        in->cancelled ? "interrupted or timed out" : "failed", detail);
    return in->cancelled == 2 ? 2 : -1;
}

struct snag_av_audio {
    struct input in;
    SwrContext *resample;
    uint64_t start, end, last_return;
    int rate, channels, format, skip, count;
    int64_t prior_end;
    bool eof, finished, produced;
};

void
snag_av_audio_close(struct snag_av_audio *audio)
{
    if (!audio) return;
    swr_free(&audio->resample); input_close(&audio->in); free(audio);
}

int
snag_av_audio_open(const char *path, uint64_t start, uint64_t end,
                   int (*pump)(void *, unsigned int), void *opaque,
                   struct snag_av_audio **out, char *error, size_t size)
{
    *out = NULL;
    if (start > 86400u || end <= start || end - start > 60u) {
        snag_errorf(error, size, "Select an audio interval up to 60 seconds (start 0..86400)"); return -1;
    }
    struct snag_av_audio *audio = calloc(1u, sizeof(*audio));
    if (!audio) return -1;
    audio->in = (struct input){.fd = -1, .pump = pump, .opaque = opaque};
    audio->start = start; audio->end = end; audio->prior_end = AV_NOPTS_VALUE;
    int rc = input_open(&audio->in, path, AVMEDIA_TYPE_AUDIO);
    if (rc >= 0 && start) {
        AVStream *stream = audio->in.format->streams[audio->in.stream];
        int64_t target = av_rescale_q((int64_t)start, (AVRational){1, 1}, stream->time_base);
        rc = av_seek_frame(audio->in.format, audio->in.stream, target, AVSEEK_FLAG_BACKWARD);
    }
    if (rc < 0) { rc = failure(&audio->in, rc, error, size); snag_av_audio_close(audio); return rc; }
    audio->last_return = snag_monotonic_ms();
    *out = audio; return 0;
}

/* The owner pulls at most 8192 stereo frames. Its wait between reads does not
 * consume the cumulative 60s decoder budget. No selected-interval PCM copy. */
int
snag_av_audio_read(struct snag_av_audio *audio, int16_t *pcm, uint32_t *frames,
                   char *error, size_t size)
{
    struct input *in = &audio->in;
    AVStream *stream = in->format->streams[in->stream];
    int rc = 0;
    *frames = 0;
    if (audio->finished) return 0;
    in->started += snag_monotonic_ms() - audio->last_return;
    for (;;) {
        if (interrupted(in)) { rc = AVERROR_EXIT; break; }
        if (!audio->count && !audio->eof) {
            rc = next_frame(in);
            if (rc == AVERROR_EOF) audio->eof = true;
            else if (rc < 0) break;
            else {
                AVFrame *f = in->frame;
                if ((f->flags & AV_FRAME_FLAG_CORRUPT) || f->best_effort_timestamp == AV_NOPTS_VALUE ||
                    f->sample_rate < 8000 || f->sample_rate > 192000 || f->ch_layout.nb_channels < 1 ||
                    f->ch_layout.nb_channels > 2 || f->nb_samples <= 0 || f->nb_samples > 1048576) {
                    rc = AVERROR_INVALIDDATA; break;
                }
                int64_t at = av_rescale_q(f->best_effort_timestamp, stream->time_base, (AVRational){1, f->sample_rate});
                if (at > INT64_MAX - f->nb_samples) { rc = AVERROR_INVALIDDATA; break; }
                int64_t first = (int64_t)audio->start * f->sample_rate, last = (int64_t)audio->end * f->sample_rate;
                if (at >= last) audio->eof = true;
                else if (at + f->nb_samples <= first) continue;
                else {
                    audio->skip = at < first ? (int)(first - at) : 0;
                    audio->count = f->nb_samples - audio->skip;
                    if (audio->count > last - at - audio->skip) audio->count = (int)(last - at - audio->skip);
                    if (!audio->resample) {
                        const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                        audio->rate = f->sample_rate; audio->channels = f->ch_layout.nb_channels; audio->format = f->format;
                        rc = swr_alloc_set_opts2(&audio->resample, &stereo, AV_SAMPLE_FMT_S16, 24000,
                            &f->ch_layout, (enum AVSampleFormat)f->format, f->sample_rate, 0, NULL);
                        if (rc < 0 || (rc = swr_init(audio->resample)) < 0) break;
                    } else if (audio->rate != f->sample_rate || audio->channels != f->ch_layout.nb_channels ||
                        audio->format != f->format || (audio->prior_end != AV_NOPTS_VALUE &&
                        llabs(at + audio->skip - audio->prior_end) > 2)) { rc = AVERROR_INVALIDDATA; break; }
                    audio->prior_end = at + audio->skip + audio->count;
                }
            }
        }
        if (!audio->resample) { rc = AVERROR_INVALIDDATA; break; }
        const uint8_t *samples[2] = {NULL, NULL};
        int n = audio->count > 2048 ? 2048 : audio->count;
        if (n) {
            int bytes = av_get_bytes_per_sample((enum AVSampleFormat)audio->format);
            bool planar = av_sample_fmt_is_planar((enum AVSampleFormat)audio->format);
            if (bytes <= 0) { rc = AVERROR_INVALIDDATA; break; }
            for (int c = 0; c < (planar ? audio->channels : 1); ++c)
                samples[c] = in->frame->extended_data[c] + (size_t)audio->skip * (size_t)bytes *
                             (planar ? 1u : (size_t)audio->channels);
        }
        int bound = swr_get_out_samples(audio->resample, n);
        if (bound < 0 || bound > 8192) { rc = AVERROR(EFBIG); break; }
        uint8_t *dst = (uint8_t *)pcm;
        rc = swr_convert(audio->resample, &dst, 8192, n ? samples : NULL, n);
        if (rc < 0) break;
        audio->skip += n; audio->count -= n;
        if (rc) { *frames = (uint32_t)rc; audio->produced = true; rc = 0; break; }
        if (audio->eof) {
            audio->finished = true;
            rc = audio->produced ? 0 : AVERROR_INVALIDDATA;
            break;
        }
    }
    audio->last_return = snag_monotonic_ms();
    if (rc < 0) { audio->finished = true; return failure(in, rc, error, size); }
    return 0;
}

int
snag_av_pcm(const char *path, uint64_t start, uint64_t end, struct snag_buf *out,
             int (*pump)(void *, unsigned int), void *opaque, char *error, size_t size)
{
    struct snag_av_audio *audio = NULL;
    size_t original = out->len;
    int rc = snag_av_audio_open(path, start, end, pump, opaque, &audio, error, size);
    if (rc) return rc;
    int16_t pcm[16384]; uint32_t frames;
    for (;;) {
        rc = snag_av_audio_read(audio, pcm, &frames, error, size);
        if (rc || !frames) break;
        unsigned char *bytes = (unsigned char *)pcm;
        for (uint32_t i = 0; i < frames * 2u; ++i) {
            uint16_t value = (uint16_t)pcm[i];
            bytes[2u * i] = (unsigned char)value; bytes[2u * i + 1u] = (unsigned char)(value >> 8);
        }
        if (snag_buf_append(out, bytes, (size_t)frames * 4u) < 0) {
            snag_errorf(error, size, "Decoded audio exceeds output buffer limit"); rc = -1; break;
        }
    }
    if (rc) out->len = original;
    snag_av_audio_close(audio);
    return rc;
}
struct snag_av_video { struct input in; };

void
snag_av_video_close(struct snag_av_video *video)
{
    if (video) { input_close(&video->in); free(video); }
}

int
snag_av_video_open(const char *path, int (*pump)(void *, unsigned int), void *opaque,
                   struct snag_av_video **out, struct snag_av_video_info *info, char *error, size_t size)
{
    *out = NULL;
    struct snag_av_video *video = calloc(1u, sizeof(*video));
    if (!video) return -1;
    video->in = (struct input){.fd = -1, .pump = pump, .opaque = opaque};
    int rc = input_open(&video->in, path, AVMEDIA_TYPE_VIDEO);
    if (rc >= 0) {
        AVFormatContext *format = video->in.format;
        AVStream *stream = format->streams[video->in.stream];
        info->duration = stream->duration != AV_NOPTS_VALUE ?
            (double)stream->duration * av_q2d(stream->time_base) :
            format->duration != AV_NOPTS_VALUE ? (double)format->duration / AV_TIME_BASE : 0;
        info->has_audio = false;
        for (unsigned int i = 0; i < format->nb_streams; ++i)
            if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) info->has_audio = true;
        if (!isfinite(info->duration) || info->duration <= 0 || info->duration > 172800.0) rc = AVERROR_INVALIDDATA;
    }
    if (rc < 0) { rc = failure(&video->in, rc, error, size); snag_av_video_close(video); }
    else *out = video;
    return rc;
}

static int
frame_png(struct input *in, AVFrame *source, struct snag_buf *out, char *transform, size_t transform_size)
{
    AVCodecContext *encoder = NULL;
    AVPacket *packet = NULL;
    AVFrame *scaled = av_frame_alloc(), *oriented = av_frame_alloc();
    struct SwsContext *scale = NULL;
    int rc = AVERROR(ENOMEM);
    if (!scaled || !oriented) goto done;
    if (source->width <= 0 || source->height <= 0 ||
        (int64_t)source->width * source->height > 16777216 || (source->flags & AV_FRAME_FLAG_CORRUPT)) {
        rc = AVERROR_INVALIDDATA; goto done;
    }
    AVStream *stream = in->format->streams[in->stream];
    AVRational sar = av_guess_sample_aspect_ratio(in->format, stream, source);
    if (sar.num <= 0 || sar.den <= 0) sar = (AVRational){1, 1};
    double width = (double)source->width * av_q2d(sar), height = source->height;
    double divisor = width > height ? width / 1600.0 : height / 1600.0;
    if (divisor < 1.0) divisor = 1.0;
    if (!isfinite(width) || width <= 0) { rc = AVERROR_INVALIDDATA; goto done; }
    int w = (int)(width / divisor), h = (int)(height / divisor);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    scaled->width = w; scaled->height = h; scaled->format = AV_PIX_FMT_RGBA;
    rc = av_frame_get_buffer(scaled, 32);
    if (rc < 0) goto done;
    scale = sws_getContext(source->width, source->height, (enum AVPixelFormat)source->format,
        w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    if (!scale) { rc = AVERROR(ENOMEM); goto done; }
    rc = sws_scale(scale, (const uint8_t *const *)source->data, source->linesize,
                    0, source->height, scaled->data, scaled->linesize);
    if (rc != h) { rc = AVERROR_INVALIDDATA; goto done; }
    /* Matrix maps (x,y) to (a*x+c*y,b*x+d*y). Accept rotations/reflections
     * without resampling arbitrary perspective or silently ignoring metadata. */
    int a = 1, b = 0, c = 0, d = 1;
    const uint8_t *data = NULL; size_t data_size = 0;
    AVFrameSideData *frame_matrix = av_frame_get_side_data(source, AV_FRAME_DATA_DISPLAYMATRIX);
    const AVPacketSideData *stream_matrix = av_packet_side_data_get(stream->codecpar->coded_side_data,
        stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (frame_matrix) { data = frame_matrix->data; data_size = frame_matrix->size; }
    else if (stream_matrix) { data = stream_matrix->data; data_size = stream_matrix->size; }
    if (data) {
        int32_t matrix[9];
        if (data_size != sizeof(matrix)) { rc = AVERROR_INVALIDDATA; goto done; }
        memcpy(matrix, data, sizeof(matrix));
        const size_t positions[] = {0, 1, 3, 4};
        for (size_t i = 0; i < 4u; ++i) {
            int32_t v = matrix[positions[i]];
            if (v != -65536 && v != 0 && v != 65536) { rc = AVERROR(ENOTSUP); goto done; }
        }
        a = matrix[0] / 65536; b = matrix[1] / 65536;
        c = matrix[3] / 65536; d = matrix[4] / 65536;
        if (matrix[2] || matrix[5] || matrix[8] != (1 << 30) ||
            abs(a) + abs(c) != 1 || abs(b) + abs(d) != 1 || a*b + c*d != 0) {
            rc = AVERROR(ENOTSUP); goto done;
        }
    }
    (void)snprintf(transform, transform_size, "display [%d %d; %d %d], SAR %d:%d normalized; PNG %dx%d", a, c, b, d, sar.num, sar.den,
        abs(a) * w + abs(c) * h, abs(b) * w + abs(d) * h);
    oriented->width = abs(a) * w + abs(c) * h;
    oriented->height = abs(b) * w + abs(d) * h;
    oriented->format = AV_PIX_FMT_RGBA;
    rc = av_frame_get_buffer(oriented, 32);
    if (rc < 0) goto done;
    int dx = (a < 0 ? w - 1 : 0) + (c < 0 ? h - 1 : 0);
    int dy = (b < 0 ? w - 1 : 0) + (d < 0 ? h - 1 : 0);
    for (int y = 0; y < h; ++y) {
        if (interrupted(in)) { rc = AVERROR_EXIT; goto done; }
        for (int x = 0; x < w; ++x) {
            int ox = a*x + c*y + dx, oy = b*x + d*y + dy;
            memcpy(oriented->data[0] + oy * oriented->linesize[0] + ox * 4,
                   scaled->data[0] + y * scaled->linesize[0] + x * 4, 4u);
        }
    }
    encoder = avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_PNG));
    packet = av_packet_alloc();
    if (!encoder || !packet) { rc = AVERROR(ENOMEM); goto done; }
    encoder->width = oriented->width; encoder->height = oriented->height;
    encoder->pix_fmt = AV_PIX_FMT_RGBA; encoder->time_base = (AVRational){1, 1};
    encoder->thread_count = 1;
    rc = avcodec_open2(encoder, avcodec_find_encoder(AV_CODEC_ID_PNG), NULL);
    if (rc < 0 || (rc = avcodec_send_frame(encoder, oriented)) < 0 ||
        (rc = avcodec_receive_packet(encoder, packet)) < 0) goto done;
    rc = snag_buf_append(out, packet->data, (size_t)packet->size) < 0 ? AVERROR(EFBIG) : 0;
done:
    avcodec_free_context(&encoder); av_packet_free(&packet);
    av_frame_free(&scaled); av_frame_free(&oriented); sws_freeContext(scale);
    return rc;
}

int
snag_av_image(const char *path, uint32_t selected, const struct snag_image_crop *crop,
               struct snag_buf *out, char *label, size_t label_size,
               int (*pump)(void *, unsigned int), void *opaque, char *error, size_t size)
{
    struct input in = {.fd = -1, .pump = pump, .opaque = opaque, .still = true};
    size_t original = out->len;
    AVFrame *frame = NULL;
    int rc = AVERROR(EINVAL);
    if (selected > 999u) goto done;
    rc = input_open(&in, path, AVMEDIA_TYPE_VIDEO);
    if (rc < 0) goto done;
    for (uint32_t i = 0; i <= selected; ++i) {
        rc = next_frame(&in);
        if (rc < 0) goto done;
    }
    frame = av_frame_clone(in.frame);
    if (!frame) { rc = AVERROR(ENOMEM); goto done; }
    int width = frame->width, height = frame->height;
    if ((frame->flags & AV_FRAME_FLAG_CORRUPT) || width <= 0 || height <= 0 ||
        (int64_t)width * height > 16777216) { rc = AVERROR_INVALIDDATA; goto done; }
    if (crop) {
        if (width <= 0 || height <= 0 || !crop->width || !crop->height ||
            crop->width > (uint32_t)width || crop->height > (uint32_t)height ||
            crop->x > (uint32_t)width - crop->width || crop->y > (uint32_t)height - crop->height) {
            rc = AVERROR(EINVAL); goto done;
        }
        /* Convert before pixel-exact crop: subsampled/unaligned AVFrame crops
         * may round coordinates or expose unsupported pointers to the scaler. */
        AVFrame *rgba = av_frame_alloc(), *region = av_frame_alloc();
        struct SwsContext *scale = NULL;
        if (!rgba || !region || (int64_t)width * height > 16777216) {
            av_frame_free(&rgba); av_frame_free(&region); rc = AVERROR(ENOMEM); goto done;
        }
        rgba->width = width; rgba->height = height; rgba->format = AV_PIX_FMT_RGBA;
        region->width = (int)crop->width; region->height = (int)crop->height; region->format = AV_PIX_FMT_RGBA;
        rc = av_frame_get_buffer(rgba, 32);
        if (rc >= 0) rc = av_frame_get_buffer(region, 32);
        if (rc >= 0) rc = av_frame_copy_props(region, frame);
        if (rc >= 0) scale = sws_getContext(width, height, (enum AVPixelFormat)frame->format,
            width, height, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
        if (rc >= 0 && !scale) rc = AVERROR(ENOMEM);
        if (rc >= 0 && sws_scale(scale, (const uint8_t *const *)frame->data, frame->linesize,
            0, height, rgba->data, rgba->linesize) != height) rc = AVERROR_INVALIDDATA;
        for (uint32_t y = 0; rc >= 0 && y < crop->height; ++y) {
            if (interrupted(&in)) { rc = AVERROR_EXIT; break; }
            memcpy(region->data[0] + (size_t)y * region->linesize[0],
                rgba->data[0] + (size_t)(crop->y + y) * rgba->linesize[0] + 4u * crop->x, (size_t)crop->width * 4u);
        }
        av_frame_free(&rgba); sws_freeContext(scale);
        if (rc < 0) { av_frame_free(&region); goto done; }
        av_frame_free(&frame); frame = region;
    }
    char transform[192];
    rc = frame_png(&in, frame, out, transform, sizeof(transform));
    if (rc >= 0 && interrupted(&in)) rc = AVERROR_EXIT;
    if (rc >= 0) (void)snprintf(label, label_size,
        "Source %dx%d; frame %u only (zero-based); crop [%u,%u,%u,%u] before orientation; %s. "
        "Other frames/regions uninspected; color profile not applied; alpha preserved.",
        width, height, selected, crop ? crop->x : 0u, crop ? crop->y : 0u,
        crop ? crop->width : (uint32_t)width, crop ? crop->height : (uint32_t)height, transform);
done:
    if (rc < 0) { out->len = original; rc = failure(&in, rc, error, size); }
    av_frame_free(&frame); input_close(&in);
    return rc;
}

int
snag_av_video_frame(struct snag_av_video *video, double target, double start, double end,
                    struct snag_buf *out, double *actual, char *transform, size_t transform_size,
                    char *error, size_t size)
{
    struct input *in = &video->in;
    AVStream *stream = in->format->streams[in->stream];
    AVFrame *selected = av_frame_alloc();
    size_t original = out->len;
    int rc = AVERROR(EINVAL);
    bool found = false;
    if (!selected || !isfinite(target) || target < start || target >= end || start < 0 || end > 172800.0) goto done;
    int64_t tick = (int64_t)(target / av_q2d(stream->time_base));
    rc = av_seek_frame(in->format, in->stream, tick, AVSEEK_FLAG_BACKWARD);
    if (rc < 0) goto done;
    avcodec_flush_buffers(in->decoder); in->draining = false;
    while ((rc = next_frame(in)) >= 0) {
        if (in->frame->best_effort_timestamp == AV_NOPTS_VALUE) { rc = AVERROR_INVALIDDATA; goto done; }
        double pts = in->frame->best_effort_timestamp * av_q2d(stream->time_base);
        if (pts < start) continue;
        if (pts >= end) break;
        av_frame_unref(selected);
        if ((rc = av_frame_ref(selected, in->frame)) < 0) goto done;
        found = true; *actual = pts;
        if (pts >= target) break;
    }
    if (rc < 0 && rc != AVERROR_EOF) goto done;
    if (!found) { rc = AVERROR_INVALIDDATA; goto done; }
    rc = frame_png(in, selected, out, transform, transform_size);
done:
    if (rc < 0) { out->len = original; rc = failure(in, rc, error, size); }
    av_frame_free(&selected);
    return rc;
}

#else
int
snag_av_image(const char *path, uint32_t frame, const struct snag_image_crop *crop,
               struct snag_buf *out, char *label, size_t label_size,
               int (*pump)(void *, unsigned int), void *opaque, char *error, size_t size)
{
    (void)path; (void)frame; (void)crop; (void)out; (void)label; (void)label_size; (void)pump; (void)opaque;
    snag_errorf(error, size, "This custom build excludes image normalization (WITH_AV=0)"); return -1;
}
void snag_av_audio_close(struct snag_av_audio *audio) { (void)audio; }
int
snag_av_audio_open(const char *path, uint64_t start, uint64_t end,
                   int (*pump)(void *, unsigned int), void *opaque,
                   struct snag_av_audio **out, char *error, size_t size)
{
    (void)path; (void)start; (void)end; (void)pump; (void)opaque; *out = NULL;
    snag_errorf(error, size, "This custom build excludes linked audio/video decoding (WITH_AV=0)"); return -1;
}
int
snag_av_audio_read(struct snag_av_audio *audio, int16_t *pcm, uint32_t *frames, char *error, size_t size)
{
    (void)audio; (void)pcm; (void)error; (void)size; *frames = 0; return -1;
}
int
snag_av_pcm(const char *path, uint64_t start, uint64_t end, struct snag_buf *out,
             int (*pump)(void *, unsigned int), void *opaque, char *error, size_t size)
{
    (void)path; (void)start; (void)end; (void)out; (void)pump; (void)opaque;
    snag_errorf(error, size, "This custom build excludes linked audio/video decoding (WITH_AV=0)");
    return -1;
}
void snag_av_video_close(struct snag_av_video *video) { (void)video; }
int
snag_av_video_open(const char *path, int (*pump)(void *, unsigned int), void *opaque,
                   struct snag_av_video **out, struct snag_av_video_info *info, char *error, size_t size)
{
    (void)path; (void)pump; (void)opaque; (void)info; *out = NULL;
    snag_errorf(error, size, "This custom build excludes linked audio/video decoding (WITH_AV=0)");
    return -1;
}
int
snag_av_video_frame(struct snag_av_video *video, double target, double start, double end,
                    struct snag_buf *out, double *actual, char *transform, size_t transform_size,
                    char *error, size_t size)
{
    (void)video; (void)target; (void)start; (void)end; (void)out;
    (void)actual; (void)transform; (void)transform_size; (void)error; (void)size;
    return -1;
}

#endif
