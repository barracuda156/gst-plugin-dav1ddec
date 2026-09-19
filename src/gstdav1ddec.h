/* GStreamer dav1d AV1 decoder
 * Copyright (C) 2026 Barracuda project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef __GST_DAV1D_DEC_H__
#define __GST_DAV1D_DEC_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideodecoder.h>

G_BEGIN_DECLS

#define GST_TYPE_DAV1D_DEC (gst_dav1d_dec_get_type ())
G_DECLARE_FINAL_TYPE (GstDav1dDec, gst_dav1d_dec, GST, DAV1D_DEC,
    GstVideoDecoder);

/**
 * GstDav1dInloopFilterType:
 * @GST_DAV1D_INLOOP_FILTER_DEBLOCK: deblocking filter
 * @GST_DAV1D_INLOOP_FILTER_CDEF: constrained directional enhancement filter
 * @GST_DAV1D_INLOOP_FILTER_RESTORATION: loop restoration filter
 *
 * In-loop post processing filters to apply while decoding. These are part of
 * normative AV1 reconstruction and feed back into the reference frames, so
 * disabling any of them degrades every frame that follows until the next
 * keyframe. They are exposed because dropping them is a large speed win on
 * hardware that cannot keep up otherwise.
 */
typedef enum
{
  GST_DAV1D_INLOOP_FILTER_DEBLOCK = (1 << 0),
  GST_DAV1D_INLOOP_FILTER_CDEF = (1 << 1),
  GST_DAV1D_INLOOP_FILTER_RESTORATION = (1 << 2)
} GstDav1dInloopFilterType;

#define GST_TYPE_DAV1D_INLOOP_FILTER_TYPE \
    (gst_dav1d_inloop_filter_type_get_type ())
GType gst_dav1d_inloop_filter_type_get_type (void);

/**
 * GstDav1dDecodeFrameType:
 * @GST_DAV1D_DECODE_FRAME_TYPE_ALL: decode and output all frames
 * @GST_DAV1D_DECODE_FRAME_TYPE_REFERENCE: only frames referenced by others
 * @GST_DAV1D_DECODE_FRAME_TYPE_INTRA: only intra frames, keyframes included
 * @GST_DAV1D_DECODE_FRAME_TYPE_KEY: only keyframes
 *
 * Which frames to spend time decoding. Anything but %GST_DAV1D_DECODE_FRAME_TYPE_ALL
 * produces an incomplete stream and is only useful to stay realtime on slow
 * hardware, or for thumbnailing.
 */
typedef enum
{
  GST_DAV1D_DECODE_FRAME_TYPE_ALL = 0,
  GST_DAV1D_DECODE_FRAME_TYPE_REFERENCE = 1,
  GST_DAV1D_DECODE_FRAME_TYPE_INTRA = 2,
  GST_DAV1D_DECODE_FRAME_TYPE_KEY = 3
} GstDav1dDecodeFrameType;

#define GST_TYPE_DAV1D_DECODE_FRAME_TYPE \
    (gst_dav1d_decode_frame_type_get_type ())
GType gst_dav1d_decode_frame_type_get_type (void);

G_END_DECLS

#endif /* __GST_DAV1D_DEC_H__ */
