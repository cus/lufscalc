/*
 * Copyright (c) 2012 Marton Balint <cus@passwd.hu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 * simple lufs and peak calculation program using libavcodec/libavformat
 */

#include <unistd.h>
#include <stddef.h>
#include <math.h>
#include "libavcodec/avcodec.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/samplefmt.h"
#include "libavutil/error.h"
#include "libavutil/opt.h"
#include "libavutil/log.h"
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

#ifdef FFMPEG_STATIC_BUILD
#include "cmdutils.h"
const char program_name[] = "lufscalc";
const int program_birth_year = 2012;
void av_noreturn exit_program(int ret)
{
    exit(ret);
}
void show_help_default(const char *opt, const char *arg)
{
}
#endif

typedef struct OutputContext {
    int initialized;
    SwrContext *swr_ctx;
    enum AVSampleFormat src_sample_fmt;
    int src_sample_rate;
    int last_channels;
    double *buffers[SWR_CH_MAX];
    int buffer_pos;
} OutputContext;
    
typedef struct TruePeakContext {
    int initialized;
    SwrContext *swr_ctx[SWR_CH_MAX];
    int swr_ctx_initialized[SWR_CH_MAX];
    double *buffers[1];
    double peak;
    double current_peak;
    double tplimit;
} TruePeakContext;

typedef struct CalcContext {
    bs1770_ctx_t *bs1770_ctx[BS1770_CTX_CNT];
    int nb_channels[BS1770_CTX_CNT];
    TruePeakContext peak[BS1770_CTX_CNT];
    int nb_context;
} CalcContext;

typedef struct LufscalcConfig {
    const AVClass *class;
    int silent;
    int resilient;
    double tplimit;
    char *track_spec;
    double peak_log_limit;
    char *logfile;
    int crlf;
} LufscalcConfig;

static const AVOption lufscalc_config_options[] = {
  { "tracks",       "track specification (2222 means four stereo tracks)",             offsetof(LufscalcConfig, track_spec),     AV_OPT_TYPE_STRING },
  { "logfile",      "set logfile path for peak logging",                               offsetof(LufscalcConfig, logfile),        AV_OPT_TYPE_STRING },
  { "silent",       "only output the measured loudness and peak seperated by a space", offsetof(LufscalcConfig, silent),         AV_OPT_TYPE_INT,    { 0 },   0, 1 },
  { "s",            "same as -silent",                                                 offsetof(LufscalcConfig, silent),         AV_OPT_TYPE_INT,    { 0 },   0, 1 },
  { "resilient",    "continue file processing on decoding errors",                     offsetof(LufscalcConfig, resilient),      AV_OPT_TYPE_INT,    { 0 },   0, 1 },
  { "r",            "same as -resilient",                                              offsetof(LufscalcConfig, resilient),      AV_OPT_TYPE_INT,    { 0 },   0, 1 },
  { "crlf",         "write crlf to the end of logfile lines",                          offsetof(LufscalcConfig, crlf),           AV_OPT_TYPE_INT,    { 0 },   0, 1 },
  { "peakloglimit", "log peaks which are above or equal to the limit",                 offsetof(LufscalcConfig, peak_log_limit), AV_OPT_TYPE_DOUBLE, { 200 }, -INFINITY, INFINITY },
  { "tplimit",      "use true peak processing above this sample peak",                 offsetof(LufscalcConfig, tplimit),        AV_OPT_TYPE_DOUBLE, { 0 },   -INFINITY, INFINITY },
  { NULL },
};

static const AVClass lufscalc_config_class = {
    .class_name = "lufscalc",
    .item_name  = av_default_item_name,
    .option     = lufscalc_config_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static void panic(const char *str) {
    av_log(NULL, AV_LOG_PANIC, "%s\n", str);
    exit(1);
}

static void calc_lufs(double* dblbuf[SWR_CH_MAX], int nb_samples, const int tgt_sample_rate, CalcContext *calc) {
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

static void calc_peak_context(double* dblbuf[SWR_CH_MAX], int nb_channels, int nb_samples, const int tgt_sample_rate, TruePeakContext *truepeak) {
    int i;
    int nb_resampled_samples;
    double channel_peak;
    double peak = 0;
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
        channel_peak = peak_max(dblbuf[i], nb_samples, 0.0);

        if (channel_peak > truepeak->tplimit) {
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
    
            channel_peak = peak_max(truepeak->buffers[0], nb_resampled_samples, channel_peak);
        } else {
            truepeak->swr_ctx_initialized[i] = 0;
        }
        
        peak = FFMAX(channel_peak, peak);
    }

    truepeak->current_peak = peak;
    truepeak->peak = FFMAX(peak, truepeak->peak);
    if (truepeak->peak / 2.0 > truepeak->tplimit)
        truepeak->tplimit = truepeak->peak / 2.0;
}

static void calc_peak(double* dblbuf[SWR_CH_MAX], int nb_samples, const int tgt_sample_rate, CalcContext *calc) {
    int i, k = 0;
    for (i=0; i<calc->nb_context; i++) {
        calc_peak_context(dblbuf + k, calc->nb_channels[i], nb_samples, tgt_sample_rate, &calc->peak[i]);
        k += calc->nb_channels[i];
    }
}

static void output_samples(AVCodecContext *c, AVFrame *decoded_frame, OutputContext *out) {
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
                                    (const uint8_t**)decoded_frame->extended_data, decoded_frame->nb_samples);
    
    if (nb_samples < 0)
        panic("audio_resample() failed");
    if (nb_samples == BUFSIZE / av_get_bytes_per_sample(tgt_sample_fmt) - out->buffer_pos)
        panic("audio buffer is probably too small");

    out->buffer_pos += nb_samples;

    //fwrite(buf, 1, data_size, stdout);

}

static int calc_available_audio_samples(CalcContext *calc, OutputContext out[], int nb_audio_streams, int64_t nb_decoded_samples, double peak_log_limit, FILE *logfile, int crlf) {
    int i, j, k;
    int min_nb_samples = out[0].buffer_pos;
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

        calc_lufs(bufs, min_nb_samples, SAMPLE_RATE, calc);
        calc_peak(bufs, min_nb_samples, SAMPLE_RATE, calc);

        for (i=0; i<calc->nb_context; i++)
            if (peak_log_limit <= calc->peak[i].current_peak)
                fprintf(logfile, "%d %02d:%02d:%02d:%02d %.1f%s\n", i,
                                                          (int)(nb_decoded_samples / SAMPLE_RATE / 60 / 60),
                                                          (int)(nb_decoded_samples / SAMPLE_RATE / 60 % 60),
                                                          (int)(nb_decoded_samples / SAMPLE_RATE % 60),
                                                          (int)(nb_decoded_samples * 25 / SAMPLE_RATE % 25),
                                                          20 * log10(calc->peak[i].current_peak),
                                                          crlf ? "\r" : "");
        for (i=0; i<nb_audio_streams; i++)
            if (out[i].buffer_pos)
                for (j=0;j<out[i].last_channels;j++)
                    memmove(out[i].buffers[j], out[i].buffers[j] + min_nb_samples, out[i].buffer_pos * av_get_bytes_per_sample(AV_SAMPLE_FMT_DBLP));
    }

    return min_nb_samples;
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
static int lufscalc_file(const char *filename, LufscalcConfig *conf)
{
    AVCodec *codec[MAX_STREAMS];
    AVCodecContext *c[MAX_STREAMS];
    AVFormatContext *ic = NULL;
    OutputContext out[MAX_STREAMS];
    int err, i, j, ret = 0;
    AVPacket avpkt, pkt;
    AVFrame *decoded_frame = NULL;
    int eof = 0;
    char codecname[256];
    int nb_audio_streams = 0;
    int audio_streams[MAX_STREAMS];
    CalcContext calc;
    int sum_channels = 0;
    int channel_limit = 256;
    char *track_spec_temp;
    char *track_spec = conf->track_spec;
    int64_t nb_decoded_samples = 0;
    double peak_log_limit = pow(10, conf->peak_log_limit / 20.0);
    FILE *logfile = NULL;

    if (conf->logfile)
        logfile = fopen(conf->logfile, "wx");
    else
        logfile = stdout;
    if (!logfile)
        panic("failed to open or create logfile");

    if (peak_log_limit < 100)
        av_log(conf, AV_LOG_INFO, "Logging peaks above %.1f dBFS peak.\n",  20 * log10(peak_log_limit));

    av_init_packet(&pkt);
    memset(&out, 0, MAX_STREAMS * sizeof(OutputContext));
    memset(&calc, 0, sizeof(calc));
    for (i = 0; i < BS1770_CTX_CNT; i++) {
        calc.bs1770_ctx[i] = bs1770_ctx_open(MODE,GATE,BLOCK,PARTITION,REFERENCE);
        calc.peak[i].tplimit = pow(10, -fabs(conf->tplimit) / 20.0);
        calc.peak[i].peak = 0.0;
        if (!calc.bs1770_ctx[i])
            panic("failed to initialize bs1770 context");
    }
    if (!(decoded_frame = avcodec_alloc_frame()))
        panic("out of memory allocating the frame");

    if (fabs(conf->tplimit) != 0)
        av_log(conf, AV_LOG_INFO, "Calculating true peak above %.1f dBFS (%.2f) sample peak.\n", -fabs(conf->tplimit), pow(10, -fabs(conf->tplimit) / 20.0));
    else
        av_log(conf, AV_LOG_INFO, "Calculating sample peak.\n");
    av_log(conf, AV_LOG_INFO, "Staring audio decoding of %s ...\n", filename);
    
    ic = avformat_alloc_context();

    err = avformat_open_input(&ic, filename, NULL, NULL);
    if (err < 0)
        panic("failed to open file");

    err = avformat_find_stream_info(ic, NULL);
    if (err < 0)
        panic("could not find codec parameters");

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
        av_log(conf, AV_LOG_INFO, "Stream %d: %s\n", stream_index, codecname);
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
                        av_log(conf, AV_LOG_ERROR, "Error while decoding.\n");
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

        nb_decoded_samples += calc_available_audio_samples(&calc, out, nb_audio_streams, nb_decoded_samples, peak_log_limit, logfile, conf->crlf);
    }

    if (eof) {

        for (i=0; i<nb_audio_streams; i++)
            if (out[i].buffer_pos)
                av_log(conf, AV_LOG_WARNING, "Buffer #%d is not empty after eof.\n", i);
    
        av_log(conf, AV_LOG_INFO, "Decoding finished.\n");
        for (i=0; i<calc.nb_context; i++)
            print_results(calc.nb_channels[i], i, filename, bs1770_ctx_track_lufs(calc.bs1770_ctx[i], SAMPLE_RATE, calc.nb_channels[i]), 20*log10(FFMAX(0.00001, calc.peak[i].peak)), conf->silent);

    } else {
        char errbuf[256] = "Unknown error";
        av_strerror(ret, errbuf, sizeof(errbuf));
        av_log(conf, AV_LOG_ERROR, "Decoding failed. %s.\n", errbuf);
    }

    if (logfile != stdout)
        fclose(logfile);

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
    LufscalcConfig conf;
    int filecount = 0;

    /* register all the codecs */
    avcodec_register_all();
    av_register_all();
    avformat_network_init();

    memset(&conf, 0, sizeof(conf));
    conf.class = &lufscalc_config_class;
    av_opt_set_defaults(&conf);

    while (!ret) {
        argv++;
        argc--;
        if (!argc) {
            if (!filecount) {
                av_log(&conf, AV_LOG_FATAL, "No input file!\n");
                ret = 1;
            }
            break;
        }
        if (!filecount) {
            if (argv[0][0] == '-') {
                const AVOption *option = av_opt_find2(&conf, (const char*)(argv[0]+1), NULL, 0, 0, NULL);
                char *value;
                if (!option) {
                    if (!strcmp(argv[0], "-h")) {
                        fprintf(stderr, "Lufscalc, built at %s %s\nCommand line parameters:\n", __DATE__, __TIME__);
                        for (option = lufscalc_config_options; option->name; option++)
                            fprintf(stderr, " -%-13s %3s %s\n", option->name, (option->type == AV_OPT_TYPE_INT && option->min == 0 && option->max == 1) ? "": (option->type == AV_OPT_TYPE_STRING) ? "<s>" : "<d>", option->help);
                        break;
                    }
                    if (!strcmp(argv[0], "-tp")) {
                        conf.tplimit = INFINITY;
                        continue;
                    }
                    if (conf.track_spec)
                        panic("tracks option is already set!");
                    option = lufscalc_config_options;
                    value = argv[0]+1;
                } else if (option->type == AV_OPT_TYPE_INT && option->min == 0 && option->max == 1) {
                    value = "1";
                } else {
                    if (argc > 1) {
                        value = argv[1];
                        argv++;
                        argc--;
                    } else {
                        av_log(&conf, AV_LOG_FATAL, "Missing parameter to option %s!\n", option->name);
                        ret = 1;
                        break;
                    }
                }
                if (av_opt_set(&conf, option->name, (const char*)value, 0) < 0) {
                    ret = 1;
                    break;
                }
                continue;
            }
        }
        filecount++;
        ret = lufscalc_file(argv[0], &conf);
    }
    
    avformat_network_deinit();
    av_opt_free(&conf);

    return ret;
}

