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
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"

#include "bs1770/bs1770_ctx.h"
#define REFERENCE   (-70.0)
#define MODE        BS1770_MODE_H

#define MAX_STREAMS 32
#define SAMPLE_RATE 48000
#define BUFSIZE (192000 * 4)
#define CH_MAX 32

#ifdef __GNUC__
#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)
#else
#define likely(x)       (x)
#define unlikely(x)     (x)
#endif

#ifdef FFMPEG_STATIC_BUILD
#define CONFIG_OPENCL 0
#include "cmdutils.h"
const char program_name[] = "lufscalc";
const int program_birth_year = 2012;
void show_help_default(const char *opt, const char *arg)
{
}
#endif

typedef struct OutputContext {
    int initialized;
    SwrContext *swr_ctx;
    enum AVSampleFormat src_sample_fmt;
    int src_sample_rate;
    int src_channels;
    int last_channels;
    double *buffers[CH_MAX];
    int buffer_pos;
    int nb_offsets;
    int64_t *offsets;
} OutputContext;
    
typedef struct TruePeakContext {
    int initialized;
    SwrContext *swr_ctx[CH_MAX];
    int swr_ctx_initialized[CH_MAX];
    double *buffers[1];
    double peak;
    double current_peak;
    double tplimit;
} TruePeakContext;

typedef struct CalcContext {
    bs1770_ctx_t *bs1770_ctx;
    int nb_channels;
    TruePeakContext peak;
    double lufs;
    double lra;
    double gain;
    struct CalcContext *next;
} CalcContext;

typedef struct LufscalcConfig {
    const AVClass *class;
    int silent;
    int json;
    int resilient;
    int track_limit;
    double tplimit;
    char *track_spec;
    double peak_log_limit;
    char *logfile;
    int crlf;
    int speedlimit;
    int status;
    int downmix;
    int lra;
    int fix;
    double fix_target;
    double max_clip;
} LufscalcConfig;

static const AVOption lufscalc_config_options[] = {
  { "tracks",       "track specification (2222 means four stereo tracks)",             offsetof(LufscalcConfig, track_spec),     AV_OPT_TYPE_STRING },
  { "logfile",      "set logfile path for peak logging",                               offsetof(LufscalcConfig, logfile),        AV_OPT_TYPE_STRING },
  { "tracklimit",   "limit the number of input tracks",                                offsetof(LufscalcConfig, track_limit) ,   AV_OPT_TYPE_INT,    { 256 }, 0, 256 },
  { "silent",       "only output the measured loudness and peak seperated by a space", offsetof(LufscalcConfig, silent),         AV_OPT_TYPE_INT,    { 0 },   0, 1 },
  { "s",            "same as -silent",                                                 offsetof(LufscalcConfig, silent),         AV_OPT_TYPE_INT,    { 0 },   0, 1 },
  { "status",       "print parse progress percent to stderr",                          offsetof(LufscalcConfig, status),         AV_OPT_TYPE_INT,    { 0 },   0, 1 },
  { "S",            "same as -status",                                                 offsetof(LufscalcConfig, status),         AV_OPT_TYPE_INT,    { 0 },   0, 1 },
  { "json",         "use json output",                                                 offsetof(LufscalcConfig, json),           AV_OPT_TYPE_INT,    { 0 },   0, 1 },
  { "j",            "same as -json",                                                   offsetof(LufscalcConfig, json),           AV_OPT_TYPE_INT,    { 0 },   0, 1 },
  { "downmix",      "downmix input audio streams to this number of channels",          offsetof(LufscalcConfig, downmix),        AV_OPT_TYPE_INT,    { 0 },   0, 6 },
  { "d",            "same as -downmix",                                                offsetof(LufscalcConfig, downmix),        AV_OPT_TYPE_INT,    { 0 },   0, 6 },
  { "lra",          "calculate loudenss range",                                        offsetof(LufscalcConfig, lra),            AV_OPT_TYPE_INT,    { 0 },   0, 1 },
  { "resilient",    "continue file processing on decoding errors",                     offsetof(LufscalcConfig, resilient),      AV_OPT_TYPE_INT,    { 0 },   0, 1 },
  { "r",            "same as -resilient",                                              offsetof(LufscalcConfig, resilient),      AV_OPT_TYPE_INT,    { 0 },   0, 1 },
  { "crlf",         "write crlf to the end of logfile lines",                          offsetof(LufscalcConfig, crlf),           AV_OPT_TYPE_INT,    { 0 },   0, 1 },
  { "peakloglimit", "log peaks which are above or equal to the limit",                 offsetof(LufscalcConfig, peak_log_limit), AV_OPT_TYPE_DOUBLE, { .dbl = 200.0 }, -INFINITY, INFINITY },
  { "tplimit",      "use true peak processing above this sample peak",                 offsetof(LufscalcConfig, tplimit),        AV_OPT_TYPE_DOUBLE, { .dbl = 0.0   }, -INFINITY, INFINITY },
  { "speedlimit",   "set processing speed limit",                                      offsetof(LufscalcConfig, speedlimit),     AV_OPT_TYPE_INT,    { 0 },   0, INT_MAX },
  { "fix",          "fix loudness of xdcam hd or imx 50",                              offsetof(LufscalcConfig, fix),            AV_OPT_TYPE_INT,    { 0 },   0, 1 },
  { "fixtarget",    "target loudness for fixing",                                      offsetof(LufscalcConfig, fix_target),     AV_OPT_TYPE_DOUBLE, { .dbl = -23.0 }, -INFINITY, INFINITY },
  { "maxclip",      "maximum allowed clipping in dB when fixing",                      offsetof(LufscalcConfig, max_clip),       AV_OPT_TYPE_DOUBLE, { .dbl = 0.0   }, -INFINITY, INFINITY },
  { NULL },
};

static const AVClass lufscalc_config_class = {
    .class_name = "lufscalc",
    .item_name  = av_default_item_name,
    .option     = lufscalc_config_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static void panic(const char *msg, ...)
{
    va_list argument_list;
    va_start(argument_list, msg);
    av_vlog(NULL, AV_LOG_PANIC, msg, argument_list);
    av_log(NULL, AV_LOG_PANIC, "\n");
    exit(1);
}

static void calc_lufs(double* dblbuf[CH_MAX], int nb_samples, const int tgt_sample_rate, CalcContext *calc) {
    int j, k = 0;
    double *dblbuf2[CH_MAX];
    for (; calc; calc = calc->next) {
        for (j=0; j<calc->nb_channels; j++)
            dblbuf2[j] = dblbuf[k+j];
        if (calc->nb_channels == 6) {
            dblbuf2[3] = dblbuf2[4];
            dblbuf2[4] = dblbuf2[5];
        }
        bs1770_ctx_add_samples_p_f64(calc->bs1770_ctx, 0, tgt_sample_rate, calc->nb_channels, dblbuf2, nb_samples);
        k += calc->nb_channels;
    }
}

static double peak_max(double *buf, int nb_samples, double peak) {
    double *bufmax = buf + nb_samples;
    for (; buf < bufmax; buf++)
        if (unlikely((peak < fabs(*buf))))
            peak = fabs(*buf);
    return peak;
}

static void calc_peak_context(double* dblbuf[CH_MAX], int nb_channels, int nb_samples, const int tgt_sample_rate, TruePeakContext *truepeak) {
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

static void calc_peak(double* dblbuf[CH_MAX], int nb_samples, const int tgt_sample_rate, CalcContext *calc) {
    int k = 0;
    for (; calc; calc = calc->next) {
        calc_peak_context(dblbuf + k, calc->nb_channels, nb_samples, tgt_sample_rate, &calc->peak);
        k += calc->nb_channels;
    }
}

static void output_samples(AVFrame *frame, OutputContext *out, int downmix, int store_offsets) {
    const int tgt_sample_rate = SAMPLE_RATE;
    const enum AVSampleFormat tgt_sample_fmt = AV_SAMPLE_FMT_DBLP;
    int64_t tgt_channel_layout;
    int tgt_channels;
    int64_t c_channel_layout;
    int nb_samples;
    int i;
    double *buffers2[CH_MAX];
    
    c_channel_layout = (frame->channel_layout && frame->channels == av_get_channel_layout_nb_channels(frame->channel_layout)) ? frame->channel_layout : av_get_default_channel_layout(frame->channels);

    if (downmix)
        tgt_channel_layout = av_get_default_channel_layout(downmix);
    else
        tgt_channel_layout = c_channel_layout;
    tgt_channels = av_get_channel_layout_nb_channels(tgt_channel_layout);
    
    if (!out->initialized) {
        out->initialized = 1;
        out->src_sample_rate = tgt_sample_rate;
        out->src_sample_fmt = tgt_sample_fmt;
        out->src_channels = tgt_channels;
        out->last_channels = tgt_channels;
        if (tgt_channels > CH_MAX)
            panic("too large number of channels");
    
        for (i=0;i<tgt_channels;i++)
            if (!(out->buffers[i] = av_malloc(BUFSIZE)))
                panic("malloc error");
    }

    if (tgt_channels != out->last_channels)
        panic("channel number changed");

    if (!out->swr_ctx || frame->format != out->src_sample_fmt || frame->sample_rate != out->src_sample_rate || frame->channels != out->src_channels) {
        if (out->swr_ctx)
            swr_free(&out->swr_ctx);
        out->swr_ctx = swr_alloc_set_opts(NULL,
                                         tgt_channel_layout,     tgt_sample_fmt, tgt_sample_rate,
                                         c_channel_layout,       frame->format,  frame->sample_rate,
                                         0, NULL);
        if (!out->swr_ctx || swr_init(out->swr_ctx) < 0)
            panic("failed to init resampler");
        out->src_sample_rate = frame->sample_rate;
        out->src_sample_fmt = frame->format;
        out->src_channels = frame->channels;
    }
    
    if (store_offsets) {
        if (!(out->nb_offsets & (out->nb_offsets + 1))) {
            out->offsets = av_realloc_array(out->offsets, (out->nb_offsets + 1) * 2, sizeof(out->offsets[0]));
            if (!out->offsets)
                panic("not enough memory allocating offset chunk");
        }
        out->offsets[out->nb_offsets] = frame->pkt_pos;
        out->nb_offsets++;
        if (frame->nb_samples != 1920)
            panic("cannot fix: invalid number of samples %d at offset %"PRId64, frame->nb_samples, frame->pkt_pos);
        if (frame->format == AV_SAMPLE_FMT_S16 && (frame->pkt_size != 1920 * 8 * 2 || frame->channels != 8))
            panic("cannot fix: invalid packet size %d at offset %"PRId64, frame->pkt_size, frame->pkt_pos);
        if (frame->format == AV_SAMPLE_FMT_S32 && (frame->pkt_size != 1920 * 3 || frame->channels != 1))
            panic("cannot fix: invalid packet size %d at offset %"PRId64, frame->pkt_size, frame->pkt_pos);
    }
    for (i=0; i<tgt_channels; i++)
        buffers2[i] = out->buffers[i] + out->buffer_pos;
    nb_samples = swr_convert(out->swr_ctx, (uint8_t**)buffers2, BUFSIZE / av_get_bytes_per_sample(tgt_sample_fmt) - out->buffer_pos,
                                    (const uint8_t**)frame->extended_data, frame->nb_samples);
    
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
        double *bufs[CH_MAX];
        k = 0;
        for (i=0; i<nb_audio_streams; i++) {
            for (j=0;j<out[i].last_channels;j++)
                bufs[k++] = out[i].buffers[j];
            out[i].buffer_pos -= min_nb_samples;
        }

        calc_lufs(bufs, min_nb_samples, SAMPLE_RATE, calc);
        calc_peak(bufs, min_nb_samples, SAMPLE_RATE, calc);

        for (i=0; calc; calc = calc->next, i++)
            if (peak_log_limit <= calc->peak.current_peak)
                fprintf(logfile, "%d %02d:%02d:%02d:%02d %.1f%s\n", i,
                                                          (int)(nb_decoded_samples / SAMPLE_RATE / 60 / 60),
                                                          (int)(nb_decoded_samples / SAMPLE_RATE / 60 % 60),
                                                          (int)(nb_decoded_samples / SAMPLE_RATE % 60),
                                                          (int)(nb_decoded_samples * 25 / SAMPLE_RATE % 25),
                                                          20 * log10(calc->peak.current_peak),
                                                          crlf ? "\r" : "");
        for (i=0; i<nb_audio_streams; i++)
            if (out[i].buffer_pos)
                for (j=0;j<out[i].last_channels;j++)
                    memmove(out[i].buffers[j], out[i].buffers[j] + min_nb_samples, out[i].buffer_pos * av_get_bytes_per_sample(AV_SAMPLE_FMT_DBLP));
    }

    return min_nb_samples;
}

static void print_calc_results(int nb_channel, int track, const char *filename, double lufs, double lra, double peak, int silent, int json, int last) {
    if (json) {
        if (lra >= 0) {
            fprintf(stdout, "{\"loudness\": \"%.1f\", \"peak\":\"%.1f\", \"lra\":\"%.1f\"}%s\n", lufs, peak, lra, (last?"":","));
        } else {
            fprintf(stdout, "{\"loudness\": \"%.1f\", \"peak\":\"%.1f\"}%s\n", lufs, peak, (last?"":","));
        }
    } else {
        if (lra >= 0) {
            if (silent)
                fprintf(stdout, "%.1f %.1f %.1f\n", lufs, peak, lra);
            else
                fprintf(stdout, "%d channel (track %d) LUFS, Peak and LRA for %s: %.1f %.1f %.1f\n", nb_channel, track, filename, lufs, peak, lra);
        } else {
            if (silent)
                fprintf(stdout, "%.1f %.1f\n", lufs, peak);
            else
                fprintf(stdout, "%d channel (track %d) LUFS and Peak for %s: %.1f %.1f\n", nb_channel, track, filename, lufs, peak);
        }
    }
}

static void print_results(const char *filename, LufscalcConfig *conf, CalcContext *calc) {
    int i;
    if (conf->json)
        printf("%s", "[\n");
    for (i=0; calc; calc = calc->next, i++) {
        print_calc_results(calc->nb_channels, i, filename,
                           calc->lufs, calc->lra,
                           20*log10(FFMAX(0.00001, calc->peak.peak)),
                           conf->silent, conf->json, !calc->next);
    }
    if (conf->json)
        printf("%s", "]\n");
}

static void fix_frame(FILE *f, int64_t offset, double gain, int chindex, int chcount, int format) {
    int size;
    uint8_t buf[65536];
    uint8_t *xbuf;
    static const uint8_t sdident[] = {0x06, 0x0e, 0x2b, 0x34, 0x01, 0x02, 0x01, 0x01, 0x0d, 0x01, 0x03, 0x01, 0x06, 0x01, 0x10, 0x00, 0x83, 0x00, 0xf0, 0x04};
    static const uint8_t hdident[] = {0x06, 0x0e, 0x2b, 0x34, 0x01, 0x02, 0x01, 0x01, 0x0d, 0x01, 0x03, 0x01, 0x16, 0x08, 0x03, 0x00, 0x83, 0x00, 0x16, 0x80};
    const uint8_t* header;
    int i;

    if (fabs(gain) < 0.1)
        return;

    gain = pow(10, gain / 20.0);
    if (format == AV_CODEC_ID_PCM_S16LE) {
        size = 20 + 4 * 1920 * chcount;
        header = sdident;
    } else {
        size = 20 + 3 * 1920 * chcount;
        header = hdident;
    }
    if (size > sizeof(buf))
        panic("cannot fix: too big packet");
    if (fseek(f, offset, SEEK_SET) == -1)
        panic("cannot fix: cannot seek");
    if (fread(buf, 1, size, f) != size)
        panic("cannot fix: read too little");
    for (i = 0; i < 20; i++)
        if (i != 15 && buf[i] != header[i])
             panic("cannot fix: mxf ident mismatch at index %d at offset %"PRId64, i, offset);
    xbuf = buf + 20;
    if (format == AV_CODEC_ID_PCM_S16LE) {
        if ((xbuf[0] != 0 && xbuf[0] != 0x80) || xbuf[1] != 0x80 || xbuf[2] != 0x07 || xbuf[3] != 0xff)
            panic("cannot fix: AES header mismatch");
        xbuf += 4;
        {
            uint32_t *p = ((uint32_t*)xbuf) + chindex;
            uint32_t *pend = p + 1920 * chcount;
            for (; p < pend; p += chcount) {
                double val = (int16_t)((*p >> 12) & 0xffff);
//                av_log(NULL, AV_LOG_ERROR, "Old: %08x\n", *p);
                *p = (*p & 0xf0000fff) | (((uint32_t)((uint16_t)av_clip_int16(lrint(val * gain)))) << 12);
//                av_log(NULL, AV_LOG_ERROR, "New: %08x\n", *p);
            }
        }
    } else {
        uint32_t *p = (uint32_t*)(xbuf - 1);
        for (i = 0; i < 1920; i++, p = (uint32_t *)((uint8_t*)p + 3)) {
            double val = (int32_t)((*p & 0xffffff00));
//            av_log(NULL, AV_LOG_ERROR, "Old: %08x\n", *p);
            *p = (*p & 0x000000ff) | (((uint32_t)av_clipl_int32(llrint(val * gain))) & 0xffffff00);
//            av_log(NULL, AV_LOG_ERROR, "New: %08x\n", *p);
        }
    }
    if (fseek(f, offset, SEEK_SET) == -1)
        panic("cannot fix: cannot seek");
    if (fwrite(buf, 1, size, f) != size)
        panic("cannot fix: wrote too little");
}

static void fix_file(const char *filename, LufscalcConfig *conf, CalcContext *rootcalc, int nb_offsets, int64_t **offsets, int *chindex, int *chcount, int format) {
    CalcContext *calc;
    int i, j;
    int64_t **offsets0 = offsets;
    int *chindex0 = chindex;
    int *chcount0 = chcount;
    int needs_fixing = 0;
    FILE *f;

    for (calc = rootcalc; calc; calc = calc->next) {
        double peak = 20 * log10(calc->peak.peak);
        calc->gain = (conf->fix_target - calc->lufs);
        if (fabs(calc->gain) >= 0.1) {
            if (calc->gain + peak > conf->max_clip)
                panic("cannot fix: will clip, peak is %7.4f, gain is %7.4f, max clip is: %7.4f", peak, calc->gain, conf->max_clip);
            if (calc->gain + peak > 0)
                av_log(conf, AV_LOG_WARNING, "Fixing the file involves clipping of %7.4f dB.\n", calc->gain + peak);
            if (fabs(calc->gain) > 20.0)
                panic("cannot fix: too big gain: %7.3f", calc->gain);
            needs_fixing = 1;
        }
    }

    if (!needs_fixing) {
        av_log(conf, AV_LOG_INFO, "Not fixing file %s because gain is negligable.\n", filename);
        return;
    }

    av_log(conf, AV_LOG_INFO, "Fixing file %s.\n", filename);

    f = fopen(filename, "r+");
    if (!f)
        panic("cannot fix: cannot open file");

    for (i=0; i<nb_offsets; i++) {
        offsets = offsets0;
        chindex = chindex0;
        chcount = chcount0;
        for (calc = rootcalc; calc; calc = calc->next) {
            for (j = 0; j < calc->nb_channels; j++) {
                fix_frame(f, **offsets, calc->gain, *chindex, *chcount, format);
                offsets[0]++;
                offsets++;
                chindex++;
                chcount++;
            }
        }
    }

    fclose(f);
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
    CalcContext *calc = NULL, *rootcalc = NULL;
    int sum_channels = 0;
    int channel_limit = 256;
    char *track_spec_temp;
    char *track_spec = conf->track_spec;
    int64_t nb_decoded_samples = 0;
    double peak_log_limit = pow(10, conf->peak_log_limit / 20.0);
    int64_t starttime, starttime_diff;
    int64_t starttime_nb_decoded_samples = 0;
    int codec_index = 0;
    int remaining_codec_channels = 0;
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
    if (!(decoded_frame = av_frame_alloc()))
        panic("out of memory allocating the frame");

    if (fabs(conf->tplimit) != 0)
        av_log(conf, AV_LOG_INFO, "Calculating true peak above %.1f dBFS (%.2f) sample peak.\n", -fabs(conf->tplimit), pow(10, -fabs(conf->tplimit) / 20.0));
    else
        av_log(conf, AV_LOG_INFO, "Calculating sample peak.\n");
    av_log(conf, AV_LOG_INFO, "Starting audio decoding of %s ...\n", filename);
    
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
        if (ic->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO && sum_channels < channel_limit && nb_audio_streams < conf->track_limit) {
            if (nb_audio_streams >= MAX_STREAMS)
                panic("cannot handle that many audio streams");
            if (ic->streams[i]->codec->channels <= 0)
                panic("channel count is 0");
            if (sum_channels + ic->streams[i]->codec->channels >= CH_MAX)
                panic("cannot handle that many audio channels");
            if ((audio_streams[nb_audio_streams] = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, i, -1, codec + nb_audio_streams, 0)) < 0)
                panic("cannot find valid audio stream");
            ic->streams[i]->discard = AVDISCARD_DEFAULT;
            nb_audio_streams++;
            sum_channels += (conf->downmix ? conf->downmix : ic->streams[i]->codec->channels);
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
        av_opt_set_int(c[i], "refcounted_frames", 1, 0);
        if (avcodec_open2(c[i], codec[i], NULL) < 0)
            panic("could not open codec");
    }

    sum_channels = FFMIN(sum_channels, channel_limit);
    remaining_codec_channels = 0;
    while (sum_channels) {
        int channels = 0;
        CalcContext *newcalc;
        if (track_spec) {
            if (!*track_spec)
                panic("track spec is not enough for sum channels");
            channels = *track_spec - '0';
            track_spec++;
        } else {
            if (c[codec_index]->channels == 0)
                panic("track has 0 channels");
            if (conf->downmix) {
                channels = conf->downmix;
            } else {
                if (remaining_codec_channels == 0)
                    remaining_codec_channels = c[codec_index]->channels;
                channels = FFMIN(6, remaining_codec_channels);
                remaining_codec_channels -= channels;
                if (!remaining_codec_channels)
                    codec_index++;
            }
        }
        if (sum_channels < channels)
            panic("channel count is not enough for track specification");
        sum_channels -= channels;
        newcalc = av_mallocz(sizeof(CalcContext));
        if (!newcalc)
            panic("cannot alloc calc context");
        newcalc->nb_channels = channels;
        if (!rootcalc)
            calc = rootcalc = newcalc;
        else
            calc->next = newcalc, calc = newcalc;
    }

    if (track_spec && *track_spec)
        panic("channel count is not enough for track specification");

    for (calc = rootcalc; calc; calc = calc->next) {
        calc->bs1770_ctx = bs1770_ctx_open(1, bs1770_lufs_ps_default(), conf->lra ? bs1770_lra_ps_default() : NULL);
        calc->peak.tplimit = pow(10, -fabs(conf->tplimit) / 20.0);
        calc->peak.peak = 0.0;
        if (!calc->bs1770_ctx)
            panic("failed to initialize bs1770 context");
    }

    starttime = av_gettime();
    while (ret == 0) {
        ret = av_read_frame(ic, &pkt);
        
        if (ret < 0) {
            if (ret == AVERROR_EOF || avio_feof(ic->pb))
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
        
                    len = avcodec_decode_audio4(c[i], decoded_frame, &got_frame, &avpkt);
                    if (len < 0) {
                        av_log(conf, AV_LOG_ERROR, "Error while decoding.\n");
                        if (!conf->resilient)
                            ret = len;
                        break;
                    }
                    if (got_frame)
                        output_samples(decoded_frame, &out[i], conf->downmix, conf->fix);
                    avpkt.size -= len;
                    avpkt.data += len;
                    avpkt.dts =
                    avpkt.pts = AV_NOPTS_VALUE;
                }
            }
        }

        av_free_packet(&pkt);

        nb_decoded_samples += calc_available_audio_samples(rootcalc, out, nb_audio_streams, nb_decoded_samples, peak_log_limit, logfile, conf->crlf);

        if (conf->speedlimit || conf->status) {
            starttime_diff = av_gettime() - starttime;
            if (starttime_diff < 0 || starttime_diff > 1000000) {
                starttime = av_gettime();
                starttime_nb_decoded_samples = nb_decoded_samples;
                starttime_diff = 1;
                if (conf->status) {
                    fprintf(stderr, "%3d %%\r", (ic->duration > 0) ? (int)(nb_decoded_samples * 100 * AV_TIME_BASE / SAMPLE_RATE / ic->duration) : 0);
                    fflush(stderr);
                }
            }
            if (conf->speedlimit)
                if (!starttime_diff || (nb_decoded_samples - starttime_nb_decoded_samples) * 1000000 / starttime_diff > conf->speedlimit * SAMPLE_RATE)
                    usleep(40000);
        }
    }

    if (eof) {
        for (i=0; i<nb_audio_streams; i++)
            if (out[i].buffer_pos)
                av_log(conf, AV_LOG_WARNING, "Buffer #%d is not empty after eof.\n", i);
        av_log(conf, AV_LOG_INFO, "Decoding finished.\n");

        for (calc = rootcalc; calc; calc = calc->next) {
            calc->lufs = bs1770_ctx_track_lufs_r128(calc->bs1770_ctx,0);
            calc->lra = conf->lra ? bs1770_ctx_track_lra_default(calc->bs1770_ctx,0) : -1;
        }

        if (conf->fix) {
            int sd = (ic->nb_streams == 2);
            int hd = (ic->nb_streams == 9);
            int k = 0;
            AVStream *video;
            int64_t *offsets[CH_MAX];
            int chindex[CH_MAX];
            int chcount[CH_MAX];

            if (!ic->iformat || strcmp(ic->iformat->name, "mxf"))
                panic("cannot fix: not mxf");
            if (!(sd ^ hd))
                panic("cannot fix: invalid number of streams");
            video = ic->streams[0];
            if (video->codec->codec_id != AV_CODEC_ID_MPEG2VIDEO)
                panic("cannot fix: first stream is not mpeg2");
            if (!((sd && video->codec->width ==  720 && video->codec->height ==  608) ||
                  (hd && video->codec->width == 1920 && video->codec->height == 1080)))
                panic("cannot fix: invalid video width for stream");
            if (video->codec->pix_fmt != AV_PIX_FMT_YUV422P)
                panic("cannot fix: invalid video pix fmt");
            if (av_cmp_q(video->r_frame_rate, (AVRational){25, 1}))
                panic("cannot fix: invalid frame rate");
            for (i=0; i<nb_audio_streams; i++) {
                if (c[i]->sample_rate != 48000)
                    panic("cannot fix: invalid sample_rate %d", c[i]->sample_rate);
                if (!((sd && c[i]->codec_id == AV_CODEC_ID_PCM_S16LE && c[i]->channels == 8) ||
                      (hd && c[i]->codec_id == AV_CODEC_ID_PCM_S24LE && c[i]->channels == 1)))
                    panic("cannot fix: invalid codec and channel combination (%s, %d)", c[i]->codec->name, c[i]->channels);
            }
            for (i=0; i<nb_audio_streams; i++) {
                if (i > 1 && out[i].nb_offsets != out[i-1].nb_offsets)
                    panic("cannot fix: inconsistent number of offsets");
            }
            for (i=0; i<nb_audio_streams; i++) {
                for (j=0;j<out[i].last_channels;j++) {
                    chindex[k] = j;
                    chcount[k] = out[i].last_channels;
                    offsets[k++] = out[i].offsets;
                }
            }

            fix_file(filename, conf, rootcalc, out[0].nb_offsets, offsets, chindex, chcount, c[0]->codec_id);
        }

        print_results(filename, conf, rootcalc);
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

    for (calc = rootcalc; calc; calc = calc->next) {
        bs1770_ctx_close(calc->bs1770_ctx);
        av_free(calc->peak.buffers[0]);
        for (j=0; j<calc->nb_channels; j++)
            swr_free(&calc->peak.swr_ctx[j]);
    }
    for (i = 0; i < nb_audio_streams; i++) {
        swr_free(&out[i].swr_ctx);
        av_free(out[i].offsets);
        for (j=0; j<CH_MAX; j++)
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
                const char *value;
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
        if (conf.downmix && conf.track_spec)
            panic("downmix and track_spec are mutually exclusive");
        filecount++;
        ret = lufscalc_file(argv[0], &conf);
    }
    
    avformat_network_deinit();
    av_opt_free(&conf);

    return ret;
}

