/*
 * Copyright (c) 2012 Marton Balint
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * simple lufs and peak calculation program using libavcodec/libavformat
 */

#include <unistd.h>
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavcodec/avcodec.h"
#include "libavutil/mathematics.h"
#include "libavutil/samplefmt.h"
#include "libavutil/error.h"
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"

#include "bs1770_ctx.h"
#define GATE      (-10.0)
#define BLOCK     400.0
#define PARTITION 4
#define REFERENCE   (-70.0)
#define MODE        BS1770_MODE_H

#define BS1770_CTX_CNT 32
#define MAX_STREAMS 16
#define SAMPLE_RATE 48000
#define BUFSIZE (AVCODEC_MAX_AUDIO_FRAME_SIZE * 4)

#ifdef __GNUC__
#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)
#else
#define likely(x)       (x)
#define unlikely(x)     (x)
#endif

struct OutputContext {
    int initialized;
    struct SwrContext *swr_ctx;
    enum AVSampleFormat src_sample_fmt;
    int src_sample_rate;
    int last_channels;
    double *buffers[SWR_CH_MAX];
    int buffer_pos;
} OutputContext;
    
struct TruePeakContext {
    int initialized;
    struct SwrContext *swr_ctx[SWR_CH_MAX];
    int swr_ctx_initialized[SWR_CH_MAX];
    double *buffers[1];
    double peak;
    double tplimit;
} TruePeakContext;

struct CalcContext {
    bs1770_ctx_t *bs1770_ctx[BS1770_CTX_CNT];
    int nb_channels[BS1770_CTX_CNT];
    struct TruePeakContext peak[BS1770_CTX_CNT];
    int nb_context;
} CalcContext;

struct LufscalcConfig {
    int silent;
    int resilient;
    double tplimit;
    char *track_spec;
    int verbose_peak;
} LufscalcConfig;

static void panic(const char *str) {
    fprintf(stderr, "%s\n", str);
    fflush(stderr);
    exit(1);
}

static void calc_lufs(double* dblbuf[SWR_CH_MAX], int nb_samples, const int tgt_sample_rate, struct CalcContext *calc) {
    int i, j, k = 0;
    double *dblbuf2[SWR_CH_MAX];
    for (i=0; i<calc->nb_context; i++) {
        for (j=0; j<calc->nb_channels[i]; j++)
            dblbuf2[j] = dblbuf[k+j];
        if (calc->nb_channels[i] == 6) {
            dblbuf2[3] = dblbuf2[4];
            dblbuf2[4] = dblbuf2[5];
        }
        for (j=0; j < nb_samples; j++)
            bs1770_ctx_add_sample(calc->bs1770_ctx[i], tgt_sample_rate, calc->nb_channels[i], dblbuf2, j);
        k += calc->nb_channels[i];
    }
}

static double peak_max(double *buf, int nb_samples, double peak) {
    double *bufmax = buf + nb_samples;
    for (; buf < bufmax; buf++)
        if (unlikely((peak < fabs(*buf))))
            peak = fabs(*buf);
    return peak;
}

static void calc_peak_context(double* dblbuf[SWR_CH_MAX], int nb_channels, int nb_samples, const int tgt_sample_rate, struct TruePeakContext *truepeak) {
    int i;
    int nb_resampled_samples;
    double peak;
    if (!truepeak->initialized) {
        for (i=0;i<nb_channels;i++) {
            truepeak->swr_ctx[i] = swr_alloc_set_opts(NULL,
                                         av_get_default_channel_layout(1), AV_SAMPLE_FMT_DBLP, 192000,
                                         av_get_default_channel_layout(1), AV_SAMPLE_FMT_DBLP, tgt_sample_rate,
                                         0, NULL);
            if (!truepeak->swr_ctx[i])
                panic("failed to init resampler");
        }
        if (!(truepeak->buffers[0] = av_malloc(BUFSIZE)))
            panic("malloc error");
        truepeak->initialized = 1;
    }

    for (i=0; i<nb_channels; i++) {
        peak = peak_max(dblbuf[i], nb_samples, 0.0);

        if (peak > truepeak->tplimit) {
            if (!truepeak->swr_ctx_initialized[i])
                if (swr_init(truepeak->swr_ctx[i]) < 0)
                    panic("failed to init resampler");
            truepeak->swr_ctx_initialized[i] = 1;

            nb_resampled_samples = swr_convert(truepeak->swr_ctx[i], (uint8_t**)truepeak->buffers, BUFSIZE / av_get_bytes_per_sample(AV_SAMPLE_FMT_DBLP),
                                               (const uint8_t**)(dblbuf+i), nb_samples);
            if (nb_resampled_samples < 0)
                panic("audio_resample() failed");
            if (nb_resampled_samples == BUFSIZE / av_get_bytes_per_sample(AV_SAMPLE_FMT_DBLP))
                panic("audio buffer is probably too small");
    
            peak = peak_max(truepeak->buffers[0], nb_resampled_samples, peak);
        } else {
            truepeak->swr_ctx_initialized[i] = 0;
        }
        
        truepeak->peak = FFMAX(peak, truepeak->peak);
    }
    if (truepeak->peak / 2.0 > truepeak->tplimit)
        truepeak->tplimit = truepeak->peak / 2.0;
}

static void calc_peak(double* dblbuf[SWR_CH_MAX], int nb_samples, const int tgt_sample_rate, struct CalcContext *calc) {
    int i, k = 0;
    for (i=0; i<calc->nb_context; i++) {
        calc_peak_context(dblbuf + k, calc->nb_channels[i], nb_samples, tgt_sample_rate, &calc->peak[i]);
        k += calc->nb_channels[i];
    }
}

static void output_samples(AVCodecContext *c, AVFrame *decoded_frame, struct OutputContext *out) {
    const int tgt_sample_rate = SAMPLE_RATE;
    const enum AVSampleFormat tgt_sample_fmt = AV_SAMPLE_FMT_DBLP;
    int64_t c_channel_layout;
    int c_channels;
    int nb_samples;
    int i;
    double *buffers2[SWR_CH_MAX];
    
    c_channel_layout = (c->channel_layout && c->channels == av_get_channel_layout_nb_channels(c->channel_layout)) ? c->channel_layout : av_get_default_channel_layout(c->channels);
    c_channels = av_get_channel_layout_nb_channels(c_channel_layout);
    
    if (!out->initialized) {
        out->initialized = 1;
        out->src_sample_rate = tgt_sample_rate;
        out->src_sample_fmt = tgt_sample_fmt;
        out->last_channels = c_channels;
        if (c_channels > SWR_CH_MAX)
            panic("too large number of channels");
    
        for (i=0;i<c_channels;i++)
            if (!(out->buffers[i] = av_malloc(BUFSIZE)))
                panic("malloc error");
    }

    if (c_channels != out->last_channels)
        panic("channel number changed");

    if (!out->swr_ctx || c->sample_fmt != out->src_sample_fmt || c->sample_rate != out->src_sample_rate) {
        if (out->swr_ctx)
            swr_free(&out->swr_ctx);
        out->swr_ctx = swr_alloc_set_opts(NULL,
                                         c_channel_layout,  tgt_sample_fmt, tgt_sample_rate,
                                         c_channel_layout,   c->sample_fmt,  c->sample_rate,
                                         0, NULL);
        if (!out->swr_ctx || swr_init(out->swr_ctx) < 0)
            panic("failed to init resampler");
        out->src_sample_rate = c->sample_rate;
        out->src_sample_fmt = c->sample_fmt;
    }
    
    for (i=0; i<c_channels; i++)
        buffers2[i] = out->buffers[i] + out->buffer_pos;
    nb_samples = swr_convert(out->swr_ctx, (uint8_t**)buffers2, BUFSIZE / av_get_bytes_per_sample(tgt_sample_fmt) - out->buffer_pos,
                                    (const uint8_t**)decoded_frame->data, decoded_frame->nb_samples);
    
    if (nb_samples < 0)
        panic("audio_resample() failed");
    if (nb_samples == BUFSIZE / av_get_bytes_per_sample(tgt_sample_fmt) - out->buffer_pos)
        panic("audio buffer is probably too small");

    out->buffer_pos += nb_samples;

    //fwrite(buf, 1, data_size, stdout);

}

static void print_results(int nb_channel, int track, const char *filename, double lufs, double peak, int silent) {
    if (silent) 
       fprintf(stdout, "%.1f %.1f\n", lufs, peak);
    else
       fprintf(stdout, "%d channel (track %d) LUFS and Peak for %s: %.1f %.1f\n", nb_channel, track, filename, lufs, peak);
}

/*
 * Audio decoding.
 */
static int lufscalc_file(const char *filename, struct LufscalcConfig *conf)
{
    AVCodec *codec[MAX_STREAMS];
    AVCodecContext *c[MAX_STREAMS];
    AVFormatContext *ic = NULL;
    struct OutputContext out[MAX_STREAMS];
    int err, i, j, k, ret = 0;
    AVPacket avpkt, pkt;
    AVFrame *decoded_frame = NULL;
    int eof = 0;
    char codecname[256];
    int nb_audio_streams = 0;
    int audio_streams[MAX_STREAMS];
    struct CalcContext calc;
    int sum_channels = 0;
    int channel_limit = 256;
    char *track_spec_temp;
    char *track_spec = conf->track_spec;

    av_init_packet(&pkt);
    memset(&out, 0, MAX_STREAMS * sizeof(struct OutputContext));
    memset(&calc, 0, sizeof(calc));
    for (i = 0; i < BS1770_CTX_CNT; i++) {
        calc.bs1770_ctx[i] = bs1770_ctx_open(MODE,GATE,BLOCK,PARTITION,REFERENCE);
        calc.peak[i].tplimit = conf->tplimit;
        calc.peak[i].peak = 0.0;
        if (!calc.bs1770_ctx[i])
            panic("failed to initialize bs1770 context\n");
    }
    if (!(decoded_frame = avcodec_alloc_frame()))
        panic("out of memory allocating the frame\n");

    fprintf(stderr, "Staring audio decoding of %s ...\n", filename);
    
    ic = avformat_alloc_context();

    err = avformat_open_input(&ic, filename, NULL, NULL);
    if (err < 0)
        panic("failed to open file\n");

    err = avformat_find_stream_info(ic, NULL);
    if (err < 0)
        panic("could not find codec parameters\n");

    if (conf->track_spec) {
        channel_limit = 0;
        for (track_spec_temp = conf->track_spec; *track_spec_temp; track_spec_temp++) {
            if (*track_spec_temp <= '0' || *track_spec_temp >= '7')
                panic("invalid track specification");
            channel_limit += *track_spec_temp - '0';
        }
    }

    for (i = 0; i < ic->nb_streams; i++) {
        if (ic->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO && sum_channels < channel_limit) {
            if (nb_audio_streams >= MAX_STREAMS)
                panic("cannot handle that many audio streams");
            if (ic->streams[i]->codec->channels <= 0)
                panic("channel count is 0");
            if (sum_channels + ic->streams[i]->codec->channels >= SWR_CH_MAX)
                panic("cannot handle that many audio channels");
            if ((audio_streams[nb_audio_streams] = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, i, -1, codec + nb_audio_streams, 0)) < 0)
                panic("cannot find valid audio stream");
            ic->streams[i]->discard = AVDISCARD_DEFAULT;
            nb_audio_streams++;
            sum_channels += ic->streams[i]->codec->channels;
        } else {
            ic->streams[i]->discard = AVDISCARD_ALL;
        }
    }

    if (nb_audio_streams <= 0)
        panic("audio stream not found");

    for (i = 0; i < nb_audio_streams; i++) {
        int stream_index = audio_streams[i];
        c[i] = ic->streams[stream_index]->codec;
        avcodec_string(codecname, sizeof(codecname), c[i], 0);
        fprintf(stderr, "Stream %d: %s\n", stream_index, codecname);
        if (avcodec_open2(c[i], codec[i], NULL) < 0)
            panic("could not open codec");
    }

    sum_channels = FFMIN(sum_channels, channel_limit);
    while (sum_channels) {
        if (calc.nb_context == BS1770_CTX_CNT)
            panic("not enough bs1770 context");
        if (track_spec && *track_spec) {
            calc.nb_channels[calc.nb_context] = *track_spec - '0';
            if (sum_channels < calc.nb_channels[calc.nb_context])
                panic("channel count is not enough for track specification");
            track_spec++;
        } else {
            calc.nb_channels[calc.nb_context] = (sum_channels == 5 || sum_channels == 6 || sum_channels == 1) ? sum_channels : 2;
        }
        sum_channels -= calc.nb_channels[calc.nb_context];
        calc.nb_context++;
    }

    if (track_spec && *track_spec)
        panic("channel count is not enough for track specification");

    while (ret == 0) {
        int min_nb_samples;

        ret = av_read_frame(ic, &pkt);
        
        if (ret < 0) {
            if (ret == AVERROR_EOF || url_feof(ic->pb))
                eof = 1;
            if (ic->pb && ic->pb->error)
                break;
            break;
        }

        for (i=0; i<nb_audio_streams; i++) {
            if (audio_streams[i] == pkt.stream_index) {
                avpkt = pkt;
                while (avpkt.size > 0) {
                    int got_frame = 0;
                    int len;
        
                    avcodec_get_frame_defaults(decoded_frame);
        
                    len = avcodec_decode_audio4(c[i], decoded_frame, &got_frame, &avpkt);
                    if (len < 0) {
                        fprintf(stderr, "Error while decoding\n"); 
                        if (!conf->resilient)
                            ret = len;
                        break;
                    }
                    if (got_frame)
                        output_samples(c[i], decoded_frame, &out[i]);
                    avpkt.size -= len;
                    avpkt.data += len;
                    avpkt.dts =
                    avpkt.pts = AV_NOPTS_VALUE;
                }
            }
        }

        av_free_packet(&pkt);

        min_nb_samples = out[0].buffer_pos;
        for (i=1; i<nb_audio_streams; i++)
            min_nb_samples = FFMIN(min_nb_samples, out[i].buffer_pos);

        if (min_nb_samples) {
            double *bufs[SWR_CH_MAX];
            k = 0;
            for (i=0; i<nb_audio_streams; i++) {
                for (j=0;j<out[i].last_channels;j++)
                    bufs[k++] = out[i].buffers[j];
                out[i].buffer_pos -= min_nb_samples;
            }

            calc_lufs(bufs, min_nb_samples, SAMPLE_RATE, &calc);
            calc_peak(bufs, min_nb_samples, SAMPLE_RATE, &calc);

            for (i=0; i<nb_audio_streams; i++)
                if (out[i].buffer_pos)
                    for (j=0;j<out[i].last_channels;j++)
                        memmove(out[i].buffers[j], out[i].buffers[j] + min_nb_samples, out[i].buffer_pos * av_get_bytes_per_sample(AV_SAMPLE_FMT_DBLP));
        }
    }

    if (eof) {

        for (i=0; i<nb_audio_streams; i++)
            if (out[i].buffer_pos)
                fprintf(stderr, "Buffer #%d is not empty after eof.\n", i);
    
        fprintf(stderr, "Decoding finished.\n");
        for (i=0; i<calc.nb_context; i++)
            print_results(calc.nb_channels[i], i, filename, bs1770_ctx_track_lufs(calc.bs1770_ctx[i], SAMPLE_RATE, calc.nb_channels[i]), 20*log10(FFMAX(0.00001, calc.peak[i].peak)), conf->silent);

    } else {
        char errbuf[256] = "Unknown error";
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "Decoding failed. %s.\n", errbuf);
    }

    for (i=0; i<nb_audio_streams; i++)
        avcodec_close(c[i]);
    avformat_close_input(&ic);
    av_free(decoded_frame);

    for (i = 0; i < BS1770_CTX_CNT; i++)
        bs1770_ctx_close(calc.bs1770_ctx[i]);
    for (i = 0; i < calc.nb_context; i++) {
        av_free(calc.peak[i].buffers[0]);
        for (j=0; j<calc.nb_channels[i]; j++)
            swr_free(&calc.peak[i].swr_ctx[j]);
    }
    for (i = 0; i < nb_audio_streams; i++) {
        swr_free(&out[i].swr_ctx);
        for (j=0; j<SWR_CH_MAX; j++)
            av_free(out[i].buffers[j]);
    }

    return eof?0:ret;
}

int main(int argc, char **argv)
{
    int ret = 0;
    struct LufscalcConfig conf;
    int filecount = 0;

    /* register all the codecs */
    avcodec_register_all();
    av_register_all();
    avformat_network_init();

    memset(&conf, 0, sizeof(conf));
    conf.tplimit = 999.0;

    while (!ret) {
        argv++;
        argc--;
        if (!argc) {
            if (!filecount) {
                fprintf(stderr, "No input file!\n");
                ret = 1;
            }
            break;
        }
        if (!filecount) {
            if (!strcmp(argv[0], "-s")) {
                conf.silent = 1;
                continue;
            }
            if (!strcmp(argv[0], "-r")) {
                conf.resilient = 1;
                continue;
            }
            if (!strcmp(argv[0], "-tp")) {
                conf.tplimit = 0.0;
                continue;
            }
            if (!strcmp(argv[0], "-tplimit")) {
                if (argc > 1) {
                  conf.tplimit = pow(10, -fabs(strtod(argv[1], NULL)) / 20.0);
                  fprintf(stderr, "Calculating true peak above %.1f dBFS sample peak.\n", 20 * log10(conf.tplimit));
                  argv++;
                  argc--;
                } else {
                  fprintf(stderr, "Invalid tplimit!\n");
                  ret = 1;
                }
                continue;
            }
            if (argv[0][0] == '-') {
                conf.track_spec = argv[0]+1;
                continue;
            }
        }
        filecount++;
        ret = lufscalc_file(argv[0], &conf);
    }
    
    avformat_network_deinit();

    return ret;
}

