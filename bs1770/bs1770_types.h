/*
 * bs1770_types.h
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
#ifndef __BS1770_TYPES_H__
#define __BS1770_TYPES_H__

#if defined (PLANAR)
  #define MKTP(tp)            bs1770_samples_##tp##_t
  #define MKFN(id,tp)         id##_p_##tp
#elif defined (INTERLEAVED)
  #define MKTP(tp)            bs1770_##tp##_t
  #define MKFN(id,tp)         id##_i_##tp
#else
  #define MKTP(tp)            bs1770_sample_##tp##_t
  #define MKFN(id,tp)         id##_##tp
#endif

#if defined (i16)
  #undef i16
  #define MAX                 (INT16_MAX-1)
  #define TP                  MKTP(i16)
  #define FN(id)              MKFN(id,i16)
#elif defined (i32)
  #undef i32
  #define MAX                 (INT32_MAX-1)
  #define TP                  MKTP(i32)
  #define FN(id)              MKFN(id,i32)
#elif defined (f32)
  #undef f32
  #define                     FLOAT
  #define TP                  MKTP(f32)
  #define FN(id)              MKFN(id,f32)
#elif defined (f64)
  #undef f64
  #define                     FLOAT
  #define TP                  MKTP(f64)
  #define FN(id)              MKFN(id,f64)
#else
  #error "Undefined format."
#endif

#endif // __BS1770_TYPES_H__
