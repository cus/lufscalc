/*
 * bs1770_ctx.h
 * Copyright (C) 2011, 2012 Peter Belkner <pbelkner@snafu.de>
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301  USA
 */
#ifndef __BS1770_CTX_H__
#define __BS1770_CTX_H__
#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdint.h>

///////////////////////////////////////////////////////////////////////////////
#define BS1770_MAX_CHANNELS     5

#define BS1770_LOWER            (0.1)
#define BS1770_UPPER            (0.95)

#define R128_LOWER              BS1770_LOWER
#define R128_UPPER              BS1770_UPPER
#define R128_REFERENCE          (-23.0)

#define A85_LOWER               BS1770_LOWER
#define A85_UPPER               BS1770_UPPER
#define A85_REFERENCE           (-24.0)

typedef int16_t bs1770_i16_t;
typedef int32_t bs1770_i32_t;
typedef float bs1770_f32_t;
typedef double bs1770_f64_t;

typedef bs1770_i16_t bs1770_sample_i16_t[BS1770_MAX_CHANNELS];
typedef bs1770_i32_t bs1770_sample_i32_t[BS1770_MAX_CHANNELS];
typedef bs1770_f32_t bs1770_sample_f32_t[BS1770_MAX_CHANNELS];
typedef bs1770_f64_t bs1770_sample_f64_t[BS1770_MAX_CHANNELS];

typedef bs1770_i16_t *bs1770_samples_i16_t[BS1770_MAX_CHANNELS];
typedef bs1770_i32_t *bs1770_samples_i32_t[BS1770_MAX_CHANNELS];
typedef bs1770_f32_t *bs1770_samples_f32_t[BS1770_MAX_CHANNELS];
typedef bs1770_f64_t *bs1770_samples_f64_t[BS1770_MAX_CHANNELS];

typedef bs1770_sample_f64_t bs1770_sample_t;
typedef bs1770_samples_f64_t bs1770_samples_t;

///////////////////////////////////////////////////////////////////////////////
typedef struct bs1770_ctx bs1770_ctx_t;

typedef double (*bs1770_ctx_lufs_t)(bs1770_ctx_t *, double);
typedef double (*bs1770_ctx_lra_t)(bs1770_ctx_t *, double, double);

typedef struct bs1770_ps {
  double ms;
  int partition;
  double gate;
} bs1770_ps_t;

///////////////////////////////////////////////////////////////////////////////
bs1770_ctx_t *bs1770_ctx_open(size_t size, const bs1770_ps_t *lufs,
    const bs1770_ps_t *lra);
void bs1770_ctx_close(bs1770_ctx_t *ctx);

#define bs1770_ctx_add_sample(ctx,i,fs,channels,sample) \
  bs1770_ctx_add_sample_f64(ctx,i,fs,channels,sample)

// interleaved
void bs1770_ctx_add_samples_i_i16(bs1770_ctx_t *ctx, size_t i, double fs,
    int channels, bs1770_i16_t *samples, size_t nsamples);
void bs1770_ctx_add_samples_i_i32(bs1770_ctx_t *ctx, size_t i, double fs,
    int channels, bs1770_i32_t *samples, size_t nsamples);
void bs1770_ctx_add_samples_i_f32(bs1770_ctx_t *ctx, size_t i, double fs,
    int channels, bs1770_f32_t *samples, size_t nsamples);
void bs1770_ctx_add_samples_i_f64(bs1770_ctx_t *ctx, size_t i, double fs,
    int channels, bs1770_f64_t *samples, size_t nsamples);

// planar
void bs1770_ctx_add_samples_p_i16(bs1770_ctx_t *ctx, size_t i, double fs,
    int channels, bs1770_samples_i16_t samples, size_t nsamples);
void bs1770_ctx_add_samples_p_i32(bs1770_ctx_t *ctx, size_t i, double fs,
    int channels, bs1770_samples_i32_t samples, size_t nsamples);
void bs1770_ctx_add_samples_p_f32(bs1770_ctx_t *ctx, size_t i, double fs,
    int channels, bs1770_samples_f32_t samples, size_t nsamples);
void bs1770_ctx_add_samples_p_f64(bs1770_ctx_t *ctx, size_t i, double fs,
    int channels, bs1770_samples_f64_t samples, size_t nsamples);

// one by one
void bs1770_ctx_add_sample_i16(bs1770_ctx_t *ctx, size_t i, double fs,
    int channels, bs1770_sample_i16_t sample);
void bs1770_ctx_add_sample_i32(bs1770_ctx_t *ctx, size_t i, double fs,
    int channels, bs1770_sample_i32_t sample);
void bs1770_ctx_add_sample_f32(bs1770_ctx_t *ctx, size_t i, double fs,
    int channels, bs1770_sample_f32_t sample);
void bs1770_ctx_add_sample_f64(bs1770_ctx_t *ctx, size_t i, double fs,
    int channels, bs1770_sample_f64_t sample);

double bs1770_ctx_track_lufs(bs1770_ctx_t *ctx, size_t i, double reference);
double bs1770_ctx_track_lra(bs1770_ctx_t *ctx, size_t i, double lower,
    double upper);
double bs1770_ctx_album_lufs(bs1770_ctx_t *ctx, double reference);
double bs1770_ctx_album_lufs_default(bs1770_ctx_t *ctx);
double bs1770_ctx_album_lra(bs1770_ctx_t *ctx, double lower, double upper);

///////////////////////////////////////////////////////////////////////////////
const bs1770_ps_t *bs1770_lufs_ps_default(void);
const bs1770_ps_t *bs1770_lra_ps_default(void);

bs1770_ctx_t *bs1770_ctx_open_default(size_t size);
double bs1770_ctx_track_lra_default(bs1770_ctx_t *ctx, size_t i);
double bs1770_ctx_album_lra_default(bs1770_ctx_t *ctx);

///////////////////////////////////////////////////////////////////////////////
#define bs1770_lufs_ps_r128() \
  bs1770_lufs_ps_default()
#define bs1770_lra_ps_r128() \
  bs1770_lra_ps_default()

#define bs1770_ctx_open_r128(size) \
  bs1770_ctx_open_default(size)
#define bs1770_ctx_track_lra_r128(ctx,i) \
  bs1770_ctx_track_lra_default(ctx,i)
#define bs1770_ctx_album_lra_r128(ctx) \
  bs1770_ctx_album_lra_default(ctx)

double bs1770_ctx_track_lufs_r128(bs1770_ctx_t *ctx, size_t i);
double bs1770_ctx_album_lufs_r128(bs1770_ctx_t *ctx);

///////////////////////////////////////////////////////////////////////////////
#define bs1770_lufs_ps_a85() \
  bs1770_lufs_ps_default()
#define bs1770_lra_ps_a85() \
  bs1770_lra_ps_default()

#define bs1770_ctx_open_a85(size) \
  bs1770_ctx_open_a85(size);
#define bs1770_ctx_track_lra_a85(ctx,i) \
  bs1770_ctx_track_lra_a85(ctx,i)
#define bs1770_ctx_album_lra_a85(ctx) \
  bs1770_ctx_album_lra_a85(ctx)

double bs1770_ctx_track_lufs_a85(bs1770_ctx_t *ctx, size_t i);
double bs1770_ctx_album_lufs_a85(bs1770_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
#endif // __BS1770_CTX_H__
