/*
 * bs1770_r128.c
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
double bs1770_ctx_track_lufs_r128(bs1770_ctx_t *ctx, size_t i)
{
  return bs1770_ctx_track_lufs(ctx,i,R128_REFERENCE);
}

double bs1770_ctx_album_lufs_r128(bs1770_ctx_t *ctx)
{
  return bs1770_ctx_album_lufs(ctx,R128_REFERENCE);
}
