/*
 * bs1770_add_samples.c
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
#include <math.h>
#include "bs1770.h"
#include "bs1770_types.h"

#define IS_DEN(x) \
    (fabs(den_tmp=(x))<1.0e-15)
#define DEN(x) \
    (IS_DEN(x)?0.0:den_tmp)

#define GET(buf,offs,i) \
    ((buf)[(get_tmp=(offs)+(i))<0?BS1770_BUF_SIZE+get_tmp:get_tmp])

#define GETX(buf,offs,i)  GET(buf,offs,i)
#define GETY(buf,offs,i)  GET(buf,(offs)-6,i)
#define GETZ(buf,offs,i)  GET(buf,(offs)-3,i)

#define MIN(x,y) \
  ((x)<(y)?x:y)

#if defined (PLANAR)
void FN(bs1770_add_samples)(bs1770_t *bs1770, double fs, int channels,
    TP samples, size_t nsamples)
#elif defined (INTERLEAVED)
void FN(bs1770_add_samples)(bs1770_t *bs1770, double fs, int channels,
    TP *samples, size_t nsamples)
#else
void FN(bs1770_add_sample)(bs1770_t *bs1770, double fs, int channels,
    TP sample)
#endif
{
  biquad_t *pre=&bs1770->pre;
  biquad_t *rlb=&bs1770->rlb;
  double wssqs=0.0;
  double *g;
  int offs, size, i;
  int get_tmp;
  double den_tmp;
#if defined (PLANAR)
  int j;
#elif defined (INTERLEAVED)
  TP *max_samples=samples+nsamples*channels;
#endif

#if defined (PLANAR)
  for (j=0;j<nsamples;++j) {
#elif defined (INTERLEAVED)
  while (samples<max_samples) {
#endif
    g=BS1770_G;
    wssqs=0.0;

    if (bs1770->fs!=fs)
      bs1770_set_fs(bs1770, fs, channels);

    offs=bs1770->ring.offs;
    size=bs1770->ring.size;

    for (i=0;i<MIN(channels,BS1770_MAX_CHANNELS);++i) {
      double *buf=bs1770->ring.buf[i];
#if defined (PLANAR)
  #if defined (FLOAT)
      double y=samples[i][j];
  #else
      double y=(double)samples[i][j]/MAX;
  #endif
#elif defined (INTERLEAVED)
  #if defined (FLOAT)
      double y=*samples++;
  #else
      double y=(double)*samples++/MAX;
  #endif
#else
  #if defined (FLOAT)
      double y=sample[i];
  #else
      double y=(double)sample[i]/MAX;
  #endif
#endif
      double x=GETX(buf,offs,0)=DEN(y);

      if (1<size) {
        double y=GETY(buf,offs,0)=DEN(pre->b0*x
          +pre->b1*GETX(buf,offs,-1)+pre->b2*GETX(buf,offs,-2)
          -pre->a1*GETY(buf,offs,-1)-pre->a2*GETY(buf,offs,-2))
          ;
        double z=GETZ(buf,offs,0)=DEN(rlb->b0*y
          +rlb->b1*GETY(buf,offs,-1)+rlb->b2*GETY(buf,offs,-2)
          -rlb->a1*GETZ(buf,offs,-1)-rlb->a2*GETZ(buf,offs,-2))
          ;
        wssqs+=(*g++)*z*z;
        ++buf;
      }
    }

    if (NULL!=bs1770->lufs)
      bs1770_aggr_add_sqs(bs1770->lufs,fs,wssqs);

    if (NULL!=bs1770->lra)
      bs1770_aggr_add_sqs(bs1770->lra,fs,wssqs);

    if (size<2)
      ++bs1770->ring.size;

    if (++bs1770->ring.offs==BS1770_BUF_SIZE)
      bs1770->ring.offs=0;

#if defined (INTERLEAVED)
    if (i<channels)
      samples+=channels-i;
  }
#elif defined (PLANAR)
  }
#endif
}
