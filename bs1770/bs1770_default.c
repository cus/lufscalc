/*
 * bs1770_default.c
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

///////////////////////////////////////////////////////////////////////////////
const bs1770_ps_t *bs1770_lufs_ps_default(void)
{
  static const bs1770_ps_t ps={
#if defined (_MSC_VER)
    400.0,
    4,
    -10.0
#else
    .ms=400.0,
    .partition=4,
    .gate=-10.0
#endif
  };

  return &ps;
}

const bs1770_ps_t *bs1770_lra_ps_default(void)
{
  static const bs1770_ps_t ps={
#if defined (_MSC_VER)
    3000.0,
    3,
    -20.0
#else
    .ms=3000.0,
    .partition=3,
    .gate=-20.0
#endif
  };

  return &ps;
}

///////////////////////////////////////////////////////////////////////////////
bs1770_ctx_t *bs1770_ctx_init_default(bs1770_ctx_t *ctx, size_t size)
{
  return bs1770_ctx_init(ctx,size,bs1770_lufs_ps_default(),
      bs1770_lra_ps_default());
}

bs1770_ctx_t *bs1770_ctx_open_default(size_t size)
{
  return bs1770_ctx_open(size,bs1770_lufs_ps_default(),
      bs1770_lra_ps_default());
}

double bs1770_ctx_track_lufs_default(bs1770_ctx_t *ctx, size_t i)
{
  return bs1770_ctx_track_lufs(ctx,i,R128_REFERENCE);
}

double bs1770_ctx_track_lra_default(bs1770_ctx_t *ctx, size_t i)
{
  return bs1770_ctx_track_lra(ctx,i,BS1770_LOWER,BS1770_UPPER);
}

double bs1770_ctx_album_lra_default(bs1770_ctx_t *ctx)
{
  return bs1770_ctx_album_lra(ctx,BS1770_LOWER,BS1770_UPPER);
}
