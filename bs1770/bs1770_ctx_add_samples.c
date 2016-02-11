/*
 * bs1770_ctx_add_samples.c
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
#include "bs1770.h"
#include "bs1770_types.h"

#if defined (PLANAR)
void FN(bs1770_ctx_add_samples)(bs1770_ctx_t *ctx, size_t i, double fs,
    int channels, TP samples, size_t nsamples)
{
  FN(bs1770_add_samples)(&ctx->nodes[i].bs1770,fs,channels,samples,
      nsamples);
}
#elif defined (INTERLEAVED)
void FN(bs1770_ctx_add_samples)(bs1770_ctx_t *ctx, size_t i, double fs,
    int channels, TP *samples, size_t nsamples)
{
  FN(bs1770_add_samples)(&ctx->nodes[i].bs1770,fs,channels,samples,
      nsamples);
}
#else
void FN(bs1770_ctx_add_sample)(bs1770_ctx_t *ctx, size_t i, double fs,
    int channels, TP sample)
{
  FN(bs1770_add_sample)(&ctx->nodes[i].bs1770,fs,channels,sample);
}
#endif
