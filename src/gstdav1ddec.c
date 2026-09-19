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

/**
 * SECTION:element-dav1ddec
 * @title: dav1ddec
 *
 * AV1 video decoder based on the dav1d library.
 *
 * ## Example launch line
 * |[
 * gst-launch-1.0 filesrc location=movie.webm ! matroskademux ! av1parse ! \
 *     dav1ddec ! videoconvert ! autovideosink
 * ]|
 *
 * Decoded pictures are written directly into buffers taken from the
 * downstream buffer pool whenever downstream can accept the layout dav1d
 * needs, so that no copy of the picture data is made at all.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstdav1ddec.h"

#include <dav1d/dav1d.h>

#include <errno.h>
#include <string.h>

GST_DEBUG_CATEGORY_STATIC (dav1d_dec_debug);
#define GST_CAT_DEFAULT dav1d_dec_debug

/* dav1d wants the allocation to cover a picture whose width and height are
 * rounded up to a multiple of 128 pixels, with every plane aligned to, and
 * padded by, DAV1D_PICTURE_ALIGNMENT bytes. See Dav1dPicAllocator. */
#define GST_DAV1D_PICTURE_PIXEL_ALIGN 128

/* dav1d stores 10 and 12 bit samples in host endian 16 bit words. */
#if G_BYTE_ORDER == G_BIG_ENDIAN
#define GST_DAV1D_FORMAT_GRAY16   GST_VIDEO_FORMAT_GRAY16_BE
#define GST_DAV1D_FORMAT_I420_10  GST_VIDEO_FORMAT_I420_10BE
#define GST_DAV1D_FORMAT_I420_12  GST_VIDEO_FORMAT_I420_12BE
#define GST_DAV1D_FORMAT_I422_10  GST_VIDEO_FORMAT_I422_10BE
#define GST_DAV1D_FORMAT_I422_12  GST_VIDEO_FORMAT_I422_12BE
#define GST_DAV1D_FORMAT_Y444_10  GST_VIDEO_FORMAT_Y444_10BE
#define GST_DAV1D_FORMAT_Y444_12  GST_VIDEO_FORMAT_Y444_12BE
#define GST_DAV1D_FORMAT_GBR_10   GST_VIDEO_FORMAT_GBR_10BE
#define GST_DAV1D_FORMAT_GBR_12   GST_VIDEO_FORMAT_GBR_12BE
#else
#define GST_DAV1D_FORMAT_GRAY16   GST_VIDEO_FORMAT_GRAY16_LE
#define GST_DAV1D_FORMAT_I420_10  GST_VIDEO_FORMAT_I420_10LE
#define GST_DAV1D_FORMAT_I420_12  GST_VIDEO_FORMAT_I420_12LE
#define GST_DAV1D_FORMAT_I422_10  GST_VIDEO_FORMAT_I422_10LE
#define GST_DAV1D_FORMAT_I422_12  GST_VIDEO_FORMAT_I422_12LE
#define GST_DAV1D_FORMAT_Y444_10  GST_VIDEO_FORMAT_Y444_10LE
#define GST_DAV1D_FORMAT_Y444_12  GST_VIDEO_FORMAT_Y444_12LE
#define GST_DAV1D_FORMAT_GBR_10   GST_VIDEO_FORMAT_GBR_10LE
#define GST_DAV1D_FORMAT_GBR_12   GST_VIDEO_FORMAT_GBR_12LE
#endif

#define DEFAULT_N_THREADS 0
#define DEFAULT_MAX_FRAME_DELAY -1
#define DEFAULT_APPLY_GRAIN FALSE
/* dav1d's own default. gst-plugins-rs defaults this to no filters at all,
 * which silently drops normative reconstruction steps; see README. */
#define DEFAULT_INLOOP_FILTERS (GST_DAV1D_INLOOP_FILTER_DEBLOCK | \
    GST_DAV1D_INLOOP_FILTER_CDEF | GST_DAV1D_INLOOP_FILTER_RESTORATION)
#define DEFAULT_DECODE_FRAME_TYPE GST_DAV1D_DECODE_FRAME_TYPE_ALL
#define DEFAULT_FRAME_SIZE_LIMIT 0
#define DEFAULT_OUTPUT_ALL_LAYERS TRUE
#define DEFAULT_OPERATING_POINT 0
#define DEFAULT_STRICT_STD_COMPLIANCE FALSE

enum
{
  PROP_0,
  PROP_N_THREADS,
  PROP_MAX_FRAME_DELAY,
  PROP_APPLY_GRAIN,
  PROP_INLOOP_FILTERS,
  PROP_DECODE_FRAME_TYPE,
  PROP_FRAME_SIZE_LIMIT,
  PROP_OUTPUT_ALL_LAYERS,
  PROP_OPERATING_POINT,
  PROP_STRICT_STD_COMPLIANCE
};

/* Picture parameters that select the output format and buffer layout. The
 * buffer pool is rebuilt whenever a decoded picture disagrees with these. */
typedef struct
{
  gboolean valid;
  gint width;
  gint height;
  gint layout;
  gint hbd;
  gint mtrx;
} GstDav1dPicParams;

struct _GstDav1dDec
{
  GstVideoDecoder parent;

  /* Accessed from the streaming thread only. The base class serialises
   * set_format(), handle_frame(), flush(), drain(), finish(), stop() and
   * decide_allocation() with its stream lock. */
  Dav1dContext *decoder;
  Dav1dData pending_data;
  GstVideoCodecState *input_state;
  GstVideoCodecState *output_state;
  /* Picture parameters output_state was negotiated for. */
  GstDav1dPicParams negotiated_params;
  guint frame_delay;
  gboolean direct_output;
  gboolean video_meta_supported;
  /* Set when dav1d's samples cannot be handed out as they are: high bit
   * depth grayscale is described as GRAY16 and has to be scaled up while
   * copying, so pictures are never decoded straight into output buffers. */
  gboolean shift_output;

  /* Colorimetry and chroma siting as signalled by upstream. Snapshotted in
   * set_format() so that the allocator, which runs on dav1d worker threads,
   * never has to touch a refcounted object. */
  gboolean have_input_colorimetry;
  gboolean have_input_chroma_site;
  GstVideoColorimetry input_colorimetry;
  GstVideoChromaSite input_chroma_site;

  /* Shared with dav1d worker threads, protected by alloc_lock. */
  GMutex alloc_lock;
  GstBufferPool *pool;
  GstVideoInfo pool_info;
  GstDav1dPicParams pool_params;

  /* Properties, protected by the GstObject lock. */
  guint n_threads;
  gint64 max_frame_delay;
  gboolean apply_grain;
  guint inloop_filters;
  gint decode_frame_type;
  guint frame_size_limit;
  gboolean output_all_layers;
  guint operating_point;
  gboolean strict_std_compliance;
};

/* Backing store of one decoded picture. Created by the picture allocator,
 * which dav1d may call from any of its worker threads, and destroyed by the
 * release callback, which likewise runs on an arbitrary thread. */
typedef struct
{
  /* The buffer the picture was decoded into. Exactly one reference exists;
   * the allocation owns it until it is frozen into a codec frame, which
   * then owns it (see gst_dav1d_dec_freeze_allocation()). The mappings
   * below hold no reference of their own. */
  GstBuffer *buffer;
  gboolean from_pool;
  /* Valid when from_pool: the buffer carries a GstVideoMeta and this is the
   * plane-wise mapping of it. */
  GstVideoFrame vframe;
  /* Valid when !from_pool, in which case the picture was decoded into a
   * plain buffer whose layout only dav1d knows. */
  GstMapInfo map;
  /* Set once the buffer has been handed to an output frame, which then owns
   * the buffer reference for as long as dav1d keeps using the picture. */
  GstVideoCodecFrame *codec_frame;
} GstDav1dAllocation;

#define gst_dav1d_dec_parent_class parent_class
G_DEFINE_TYPE (GstDav1dDec, gst_dav1d_dec, GST_TYPE_VIDEO_DECODER);

GType
gst_dav1d_inloop_filter_type_get_type (void)
{
  static gsize static_type = 0;

  if (g_once_init_enter (&static_type)) {
    static const GFlagsValue values[] = {
      {GST_DAV1D_INLOOP_FILTER_DEBLOCK, "Enable deblocking filter", "deblock"},
      {GST_DAV1D_INLOOP_FILTER_CDEF,
          "Enable Constrained Directional Enhancement Filter", "cdef"},
      {GST_DAV1D_INLOOP_FILTER_RESTORATION,
          "Enable loop restoration filter", "restoration"},
      {0, NULL, NULL}
    };
    GType type = g_flags_register_static ("GstDav1dInloopFilterType", values);
    g_once_init_leave (&static_type, type);
  }

  return (GType) static_type;
}

GType
gst_dav1d_decode_frame_type_get_type (void)
{
  static gsize static_type = 0;

  if (g_once_init_enter (&static_type)) {
    static const GEnumValue values[] = {
      {GST_DAV1D_DECODE_FRAME_TYPE_ALL, "Decode all frames", "all"},
      {GST_DAV1D_DECODE_FRAME_TYPE_REFERENCE,
          "Decode frames referenced by other frames only", "reference"},
      {GST_DAV1D_DECODE_FRAME_TYPE_INTRA,
          "Decode intra frames only, keyframes included", "intra"},
      {GST_DAV1D_DECODE_FRAME_TYPE_KEY, "Decode keyframes only", "key"},
      {0, NULL, NULL}
    };
    GType type = g_enum_register_static ("GstDav1dDecodeFrameType", values);
    g_once_init_leave (&static_type, type);
  }

  return (GType) static_type;
}

/* --------------------------------------------------------------------- */
/* Picture parameter and format mapping                                   */
/* --------------------------------------------------------------------- */

static void
gst_dav1d_dec_pic_params (const Dav1dPicture * pic, GstDav1dPicParams * params)
{
  params->valid = (pic->seq_hdr != NULL);
  params->width = pic->p.w;
  params->height = pic->p.h;
  params->layout = pic->p.layout;
  params->hbd = pic->seq_hdr ? (gint) pic->seq_hdr->hbd : -1;
  params->mtrx = pic->seq_hdr ? (gint) pic->seq_hdr->mtrx : -1;
}

static gboolean
gst_dav1d_dec_pic_params_equal (const GstDav1dPicParams * a,
    const GstDav1dPicParams * b)
{
  return a->valid && b->valid && a->width == b->width
      && a->height == b->height && a->layout == b->layout && a->hbd == b->hbd
      && a->mtrx == b->mtrx;
}

/* Number of chroma planes and their subsampling for a dav1d pixel layout. */
static gboolean
gst_dav1d_dec_layout_info (gint layout, gint * n_planes, gint * hsub,
    gint * vsub)
{
  switch (layout) {
    case DAV1D_PIXEL_LAYOUT_I400:
      *n_planes = 1;
      *hsub = *vsub = 0;
      return TRUE;
    case DAV1D_PIXEL_LAYOUT_I420:
      *n_planes = 3;
      *hsub = *vsub = 1;
      return TRUE;
    case DAV1D_PIXEL_LAYOUT_I422:
      *n_planes = 3;
      *hsub = 1;
      *vsub = 0;
      return TRUE;
    case DAV1D_PIXEL_LAYOUT_I444:
      *n_planes = 3;
      *hsub = *vsub = 0;
      return TRUE;
    default:
      return FALSE;
  }
}

static GstVideoFormat
gst_dav1d_dec_video_format_from_picture (GstDav1dDec * self,
    const Dav1dPicture * pic)
{
  GstVideoFormat format = GST_VIDEO_FORMAT_UNKNOWN;
  gboolean is_rgb;
  gint bits;

  if (pic->seq_hdr == NULL) {
    GST_WARNING_OBJECT (self, "Picture without sequence header");
    return GST_VIDEO_FORMAT_UNKNOWN;
  }

  switch (pic->seq_hdr->hbd) {
    case 0:
      bits = 8;
      break;
    case 1:
      bits = 10;
      break;
    case 2:
      bits = 12;
      break;
    default:
      GST_WARNING_OBJECT (self, "Unsupported bit depth index %d",
          pic->seq_hdr->hbd);
      return GST_VIDEO_FORMAT_UNKNOWN;
  }

  switch (pic->p.layout) {
    case DAV1D_PIXEL_LAYOUT_I400:
      /* There is no planar 10 or 12 bit grayscale format in GStreamer, so the
       * samples are described as 16 bit words with the value range implied by
       * the caps. */
      format = (bits == 8) ? GST_VIDEO_FORMAT_GRAY8 : GST_DAV1D_FORMAT_GRAY16;
      break;
    case DAV1D_PIXEL_LAYOUT_I420:
      format = (bits == 8) ? GST_VIDEO_FORMAT_I420
          : (bits == 10) ? GST_DAV1D_FORMAT_I420_10 : GST_DAV1D_FORMAT_I420_12;
      break;
    case DAV1D_PIXEL_LAYOUT_I422:
      format = (bits == 8) ? GST_VIDEO_FORMAT_Y42B
          : (bits == 10) ? GST_DAV1D_FORMAT_I422_10 : GST_DAV1D_FORMAT_I422_12;
      break;
    case DAV1D_PIXEL_LAYOUT_I444:
      format = (bits == 8) ? GST_VIDEO_FORMAT_Y444
          : (bits == 10) ? GST_DAV1D_FORMAT_Y444_10 : GST_DAV1D_FORMAT_Y444_12;
      break;
    default:
      GST_WARNING_OBJECT (self, "Unsupported dav1d pixel layout %d",
          pic->p.layout);
      return GST_VIDEO_FORMAT_UNKNOWN;
  }

  /* AV1 codes RGB as 4:4:4 with the identity matrix. Trust upstream if it
   * said anything about the matrix, and the sequence header otherwise. */
  if (self->have_input_colorimetry
      && self->input_colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_UNKNOWN)
    is_rgb = (self->input_colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_RGB);
  else
    is_rgb = (pic->seq_hdr->mtrx == DAV1D_MC_IDENTITY);

  if (is_rgb) {
    GST_DEBUG_OBJECT (self, "Input is RGB, remapping format %s",
        gst_video_format_to_string (format));

    if (pic->p.layout != DAV1D_PIXEL_LAYOUT_I444) {
      GST_ERROR_OBJECT (self, "RGB requires a 4:4:4 pixel layout");
      return GST_VIDEO_FORMAT_UNKNOWN;
    }

    format = (bits == 8) ? GST_VIDEO_FORMAT_GBR
        : (bits == 10) ? GST_DAV1D_FORMAT_GBR_10 : GST_DAV1D_FORMAT_GBR_12;
  }

  return format;
}

static void
gst_dav1d_dec_colorimetry_from_picture (GstDav1dDec * self,
    const Dav1dPicture * pic, GstVideoColorimetry * cinfo)
{
  const Dav1dSequenceHeader *hdr = pic->seq_hdr;

  /* Only fill in what upstream did not already tell us. */
  if (self->have_input_colorimetry
      && self->input_colorimetry.range != GST_VIDEO_COLOR_RANGE_UNKNOWN) {
    cinfo->range = self->input_colorimetry.range;
  } else {
    cinfo->range = hdr->color_range ? GST_VIDEO_COLOR_RANGE_0_255
        : GST_VIDEO_COLOR_RANGE_16_235;
  }

  if (self->have_input_colorimetry
      && self->input_colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_UNKNOWN) {
    cinfo->matrix = self->input_colorimetry.matrix;
  } else {
    switch (hdr->mtrx) {
      case DAV1D_MC_IDENTITY:
        cinfo->matrix = GST_VIDEO_COLOR_MATRIX_RGB;
        break;
      case DAV1D_MC_BT709:
      case DAV1D_MC_UNKNOWN:
        cinfo->matrix = GST_VIDEO_COLOR_MATRIX_BT709;
        break;
      case DAV1D_MC_FCC:
        cinfo->matrix = GST_VIDEO_COLOR_MATRIX_FCC;
        break;
      case DAV1D_MC_BT470BG:
      case DAV1D_MC_BT601:
        cinfo->matrix = GST_VIDEO_COLOR_MATRIX_BT601;
        break;
      case DAV1D_MC_SMPTE240:
        cinfo->matrix = GST_VIDEO_COLOR_MATRIX_SMPTE240M;
        break;
      case DAV1D_MC_BT2020_NCL:
        cinfo->matrix = GST_VIDEO_COLOR_MATRIX_BT2020;
        break;
      default:
        GST_WARNING_OBJECT (self, "Unsupported matrix coefficients %d",
            hdr->mtrx);
        cinfo->matrix = GST_VIDEO_COLOR_MATRIX_UNKNOWN;
        break;
    }
  }

  if (self->have_input_colorimetry
      && self->input_colorimetry.transfer !=
      GST_VIDEO_TRANSFER_UNKNOWN) {
    cinfo->transfer = self->input_colorimetry.transfer;
  } else {
    switch (hdr->trc) {
      case DAV1D_TRC_BT709:
      case DAV1D_TRC_UNKNOWN:
      case DAV1D_TRC_BT470M:
      case DAV1D_TRC_BT1361:
        cinfo->transfer = GST_VIDEO_TRANSFER_BT709;
        break;
      case DAV1D_TRC_BT470BG:
        cinfo->transfer = GST_VIDEO_TRANSFER_GAMMA28;
        break;
      case DAV1D_TRC_BT601:
        cinfo->transfer = GST_VIDEO_TRANSFER_BT601;
        break;
      case DAV1D_TRC_SMPTE240:
        cinfo->transfer = GST_VIDEO_TRANSFER_SMPTE240M;
        break;
      case DAV1D_TRC_LINEAR:
        cinfo->transfer = GST_VIDEO_TRANSFER_GAMMA10;
        break;
      case DAV1D_TRC_LOG100:
        cinfo->transfer = GST_VIDEO_TRANSFER_LOG100;
        break;
      case DAV1D_TRC_LOG100_SQRT10:
        cinfo->transfer = GST_VIDEO_TRANSFER_LOG316;
        break;
      case DAV1D_TRC_IEC61966:
      case DAV1D_TRC_SRGB:
        cinfo->transfer = GST_VIDEO_TRANSFER_SRGB;
        break;
      case DAV1D_TRC_BT2020_10BIT:
        cinfo->transfer = GST_VIDEO_TRANSFER_BT2020_10;
        break;
      case DAV1D_TRC_BT2020_12BIT:
        cinfo->transfer = GST_VIDEO_TRANSFER_BT2020_12;
        break;
      case DAV1D_TRC_SMPTE2084:
        cinfo->transfer = GST_VIDEO_TRANSFER_SMPTE2084;
        break;
      case DAV1D_TRC_HLG:
        cinfo->transfer = GST_VIDEO_TRANSFER_ARIB_STD_B67;
        break;
      default:
        GST_WARNING_OBJECT (self, "Unsupported transfer characteristics %d",
            hdr->trc);
        cinfo->transfer = GST_VIDEO_TRANSFER_UNKNOWN;
        break;
    }
  }

  if (self->have_input_colorimetry
      && self->input_colorimetry.primaries !=
      GST_VIDEO_COLOR_PRIMARIES_UNKNOWN) {
    cinfo->primaries = self->input_colorimetry.primaries;
  } else {
    switch (hdr->pri) {
      case DAV1D_COLOR_PRI_BT709:
      case DAV1D_COLOR_PRI_UNKNOWN:
        cinfo->primaries = GST_VIDEO_COLOR_PRIMARIES_BT709;
        break;
      case DAV1D_COLOR_PRI_BT470M:
        cinfo->primaries = GST_VIDEO_COLOR_PRIMARIES_BT470M;
        break;
      case DAV1D_COLOR_PRI_BT470BG:
      case DAV1D_COLOR_PRI_BT601:
        cinfo->primaries = GST_VIDEO_COLOR_PRIMARIES_BT470BG;
        break;
      case DAV1D_COLOR_PRI_SMPTE240:
        cinfo->primaries = GST_VIDEO_COLOR_PRIMARIES_SMPTE240M;
        break;
      case DAV1D_COLOR_PRI_FILM:
        cinfo->primaries = GST_VIDEO_COLOR_PRIMARIES_FILM;
        break;
      case DAV1D_COLOR_PRI_BT2020:
        cinfo->primaries = GST_VIDEO_COLOR_PRIMARIES_BT2020;
        break;
      case DAV1D_COLOR_PRI_XYZ:
        cinfo->primaries = GST_VIDEO_COLOR_PRIMARIES_SMPTEST428;
        break;
      case DAV1D_COLOR_PRI_SMPTE431:
        cinfo->primaries = GST_VIDEO_COLOR_PRIMARIES_SMPTERP431;
        break;
      case DAV1D_COLOR_PRI_SMPTE432:
        cinfo->primaries = GST_VIDEO_COLOR_PRIMARIES_SMPTEEG432;
        break;
      case DAV1D_COLOR_PRI_EBU3213:
        cinfo->primaries = GST_VIDEO_COLOR_PRIMARIES_EBU3213;
        break;
      default:
        GST_WARNING_OBJECT (self, "Unsupported colour primaries %d", hdr->pri);
        cinfo->primaries = GST_VIDEO_COLOR_PRIMARIES_UNKNOWN;
        break;
    }
  }
}

static GstVideoChromaSite
gst_dav1d_dec_chroma_site_from_picture (GstDav1dDec * self,
    const Dav1dPicture * pic)
{
  if (self->have_input_chroma_site)
    return self->input_chroma_site;

  if (pic->p.layout == DAV1D_PIXEL_LAYOUT_I420
      || pic->p.layout == DAV1D_PIXEL_LAYOUT_I422) {
    /* AV1 CSP_VERTICAL is horizontally co-sited (MPEG-2 "left"),
     * CSP_COLOCATED is co-sited in both directions ("top-left", which
     * GStreamer calls the DV siting). Unknown is treated as centered, as
     * dav1d's own y4m output does. */
    switch (pic->seq_hdr->chr) {
      case DAV1D_CHR_UNKNOWN:
        return GST_VIDEO_CHROMA_SITE_JPEG;
      case DAV1D_CHR_VERTICAL:
        return GST_VIDEO_CHROMA_SITE_MPEG2;
      case DAV1D_CHR_COLOCATED:
        return GST_VIDEO_CHROMA_SITE_DV;
      default:
        break;
    }
  }

  return GST_VIDEO_CHROMA_SITE_UNKNOWN;
}

/* --------------------------------------------------------------------- */
/* Picture allocator, called from dav1d worker threads                    */
/* --------------------------------------------------------------------- */

/* Compute the buffer layout dav1d needs for a picture, without reference to
 * any GStreamer video format. Used for the fallback allocation made before
 * the output format has been negotiated. */
static gboolean
gst_dav1d_dec_plain_layout (const Dav1dPicture * pic, gsize * y_stride,
    gsize * uv_stride, gsize * y_size, gsize * uv_size)
{
  gint n_planes, hsub, vsub;
  gint aligned_w, aligned_h;
  gsize pixel_size;

  if (!gst_dav1d_dec_layout_info (pic->p.layout, &n_planes, &hsub, &vsub))
    return FALSE;

  aligned_w = GST_ROUND_UP_N (pic->p.w, GST_DAV1D_PICTURE_PIXEL_ALIGN);
  aligned_h = GST_ROUND_UP_N (pic->p.h, GST_DAV1D_PICTURE_PIXEL_ALIGN);
  pixel_size = (pic->p.bpc > 8) ? 2 : 1;

  *y_stride = GST_ROUND_UP_N ((gsize) aligned_w * pixel_size,
      DAV1D_PICTURE_ALIGNMENT);
  *y_size = *y_stride * aligned_h;

  if (n_planes > 1) {
    *uv_stride = GST_ROUND_UP_N ((gsize) (aligned_w >> hsub) * pixel_size,
        DAV1D_PICTURE_ALIGNMENT);
    *uv_size = *uv_stride * (aligned_h >> vsub);
  } else {
    *uv_stride = 0;
    *uv_size = 0;
  }

  return TRUE;
}

static void
gst_dav1d_dec_allocation_free (GstDav1dAllocation * alloc)
{
  if (alloc->buffer != NULL) {
    if (alloc->from_pool)
      gst_video_frame_unmap (&alloc->vframe);
    else
      gst_buffer_unmap (alloc->buffer, &alloc->map);
  }

  /* The single buffer reference belongs to the codec frame once the
   * allocation has been frozen, and to us otherwise. */
  if (alloc->codec_frame != NULL)
    gst_video_codec_frame_unref (alloc->codec_frame);
  else if (alloc->buffer != NULL)
    gst_buffer_unref (alloc->buffer);

  g_free (alloc);
}

/* Whether dav1d can decode into the planes of a mapped frame: every plane
 * pointer must be DAV1D_PICTURE_ALIGNMENT aligned and both chroma planes must
 * share one stride. The pool layout was checked when the pool was chosen, but
 * the mapped pointers are what actually matters, so check them too. */
static gboolean
gst_dav1d_dec_frame_is_usable (const GstVideoFrame * vframe, gint n_planes)
{
  gint i;

  for (i = 0; i < n_planes; i++) {
    if (GPOINTER_TO_SIZE (GST_VIDEO_FRAME_PLANE_DATA (vframe, i))
        % DAV1D_PICTURE_ALIGNMENT != 0)
      return FALSE;
    if (GST_VIDEO_FRAME_PLANE_STRIDE (vframe, i) % DAV1D_PICTURE_ALIGNMENT != 0)
      return FALSE;
  }

  if (n_planes > 1
      && GST_VIDEO_FRAME_PLANE_STRIDE (vframe, 1)
      != GST_VIDEO_FRAME_PLANE_STRIDE (vframe, 2))
    return FALSE;

  return TRUE;
}

/* Decode straight into a buffer from the negotiated pool. Returns FALSE when
 * the pool cannot be used for this picture, in which case the caller falls
 * back to a plain buffer. */
static gboolean
gst_dav1d_dec_alloc_from_pool (GstDav1dDec * self, Dav1dPicture * pic,
    GstBufferPool * pool, const GstVideoInfo * info, gint n_planes,
    GstDav1dAllocation * alloc)
{
  GstBuffer *buffer = NULL;
  GstFlowReturn ret;

  ret = gst_buffer_pool_acquire_buffer (pool, &buffer, NULL);
  if (ret != GST_FLOW_OK) {
    GST_DEBUG_OBJECT (self, "Failed to acquire buffer from pool: %s",
        gst_flow_get_name (ret));
    return FALSE;
  }

  /* We keep the buffer reference ourselves, so the mapping must not take
   * another one: it is handed over as is to the output frame later. */
  if (!gst_video_frame_map (&alloc->vframe, info, buffer,
          GST_MAP_READWRITE | GST_VIDEO_FRAME_MAP_FLAG_NO_REF)) {
    GST_WARNING_OBJECT (self, "Failed to map pool buffer");
    gst_buffer_unref (buffer);
    return FALSE;
  }

  if (!gst_dav1d_dec_frame_is_usable (&alloc->vframe, n_planes)) {
    GST_WARNING_OBJECT (self, "Pool buffer %p has planes dav1d cannot decode "
        "into, decoding into a plain buffer instead", buffer);
    gst_video_frame_unmap (&alloc->vframe);
    gst_buffer_unref (buffer);
    return FALSE;
  }

  alloc->buffer = buffer;
  alloc->from_pool = TRUE;

  pic->data[0] = GST_VIDEO_FRAME_PLANE_DATA (&alloc->vframe, 0);
  pic->stride[0] = GST_VIDEO_FRAME_PLANE_STRIDE (&alloc->vframe, 0);
  if (n_planes > 1) {
    pic->data[1] = GST_VIDEO_FRAME_PLANE_DATA (&alloc->vframe, 1);
    pic->data[2] = GST_VIDEO_FRAME_PLANE_DATA (&alloc->vframe, 2);
    pic->stride[1] = GST_VIDEO_FRAME_PLANE_STRIDE (&alloc->vframe, 1);
  } else {
    pic->data[1] = pic->data[2] = NULL;
    pic->stride[1] = 0;
  }

  return TRUE;
}

/* Decode into a freshly allocated buffer with the layout dav1d would have
 * chosen itself. Used before the output format is known and whenever the
 * negotiated pool cannot be used; the picture is copied on output. */
static gboolean
gst_dav1d_dec_alloc_plain (GstDav1dDec * self, Dav1dPicture * pic,
    gint n_planes, GstDav1dAllocation * alloc)
{
  GstAllocationParams alloc_params;
  gsize y_stride, uv_stride, y_size, uv_size;
  GstBuffer *buffer;
  guint8 *base;

  if (!gst_dav1d_dec_plain_layout (pic, &y_stride, &uv_stride, &y_size,
          &uv_size))
    return FALSE;

  GST_DEBUG_OBJECT (self,
      "No usable pool for %dx%d picture, decoding into a plain buffer",
      pic->p.w, pic->p.h);

  gst_allocation_params_init (&alloc_params);
  alloc_params.align = DAV1D_PICTURE_ALIGNMENT - 1;
  alloc_params.padding = DAV1D_PICTURE_ALIGNMENT;

  buffer = gst_buffer_new_allocate (NULL, y_size + 2 * uv_size, &alloc_params);
  if (buffer == NULL) {
    GST_ERROR_OBJECT (self, "Failed to allocate buffer");
    return FALSE;
  }

  if (!gst_buffer_map (buffer, &alloc->map, GST_MAP_READWRITE)) {
    GST_ERROR_OBJECT (self, "Failed to map buffer");
    gst_buffer_unref (buffer);
    return FALSE;
  }

  alloc->buffer = buffer;
  alloc->from_pool = FALSE;

  base = alloc->map.data;
  pic->data[0] = base;
  pic->stride[0] = y_stride;
  if (n_planes > 1) {
    pic->data[1] = base + y_size;
    pic->data[2] = base + y_size + uv_size;
    pic->stride[1] = uv_stride;
  } else {
    pic->data[1] = pic->data[2] = NULL;
    pic->stride[1] = 0;
  }

  return TRUE;
}

static int
gst_dav1d_dec_alloc_picture (Dav1dPicture * pic, void *cookie)
{
  GstDav1dDec *self = GST_DAV1D_DEC (cookie);
  GstDav1dAllocation *alloc;
  GstDav1dPicParams params;
  GstBufferPool *pool = NULL;
  GstVideoInfo info;
  gboolean allocated = FALSE;
  gint n_planes, hsub, vsub;

  gst_video_info_init (&info);

  if (!gst_dav1d_dec_layout_info (pic->p.layout, &n_planes, &hsub, &vsub)) {
    GST_ERROR_OBJECT (self, "Unsupported pixel layout %d", pic->p.layout);
    return DAV1D_ERR (EINVAL);
  }

  gst_dav1d_dec_pic_params (pic, &params);

  /* This runs on a dav1d worker thread whenever frame threading is active,
   * concurrently with the streaming thread sitting inside dav1d. Nothing
   * here may take the video decoder stream lock, so a picture whose format
   * has not been negotiated yet is decoded into a plain buffer and copied
   * later, on the streaming thread, once the format is known. */
  g_mutex_lock (&self->alloc_lock);
  if (self->pool != NULL
      && gst_dav1d_dec_pic_params_equal (&params, &self->pool_params)) {
    pool = gst_object_ref (self->pool);
    info = self->pool_info;
  }
  g_mutex_unlock (&self->alloc_lock);

  alloc = g_new0 (GstDav1dAllocation, 1);

  if (pool != NULL) {
    allocated = gst_dav1d_dec_alloc_from_pool (self, pic, pool, &info,
        n_planes, alloc);
    gst_object_unref (pool);
  }

  if (!allocated && !gst_dav1d_dec_alloc_plain (self, pic, n_planes, alloc)) {
    g_free (alloc);
    return DAV1D_ERR (ENOMEM);
  }

  pic->allocator_data = alloc;

  GST_TRACE_OBJECT (self, "Allocated %dx%d picture into buffer %p (pooled: %d)",
      pic->p.w, pic->p.h, alloc->buffer, alloc->from_pool);

  return 0;
}

static void
gst_dav1d_dec_release_picture (Dav1dPicture * pic, void *cookie)
{
  GstDav1dDec *self = GST_DAV1D_DEC (cookie);
  GstDav1dAllocation *alloc = pic->allocator_data;

  if (alloc == NULL)
    return;

  GST_TRACE_OBJECT (self, "Releasing buffer %p", alloc->buffer);

  gst_dav1d_dec_allocation_free (alloc);
  pic->allocator_data = NULL;
}

/* --------------------------------------------------------------------- */
/* Buffer pool configuration                                              */
/* --------------------------------------------------------------------- */

static void gst_dav1d_dec_install_pool (GstDav1dDec * self,
    GstBufferPool * pool, const GstVideoInfo * info);
static GstBufferPool *gst_dav1d_dec_create_internal_pool (GstDav1dDec * self,
    GstCaps * caps, const GstVideoInfo * info, GstVideoInfo * pool_info);

/* Extend @info so that the allocation covers a 128 pixel aligned picture with
 * DAV1D_PICTURE_ALIGNMENT aligned strides.
 *
 * The slack goes into padding_right and padding_bottom on purpose: padding on
 * the left or top shifts the plane pointers and would break the alignment
 * dav1d requires of them. Padding requested by downstream is honoured, and
 * gst_dav1d_dec_info_is_usable() afterwards rejects a layout that alignment
 * cannot survive. */
static gboolean
gst_dav1d_dec_align_info (GstVideoInfo * info, GstVideoAlignment * align,
    const GstVideoAlignment * downstream, guint stride_align)
{
  gint aligned_width, aligned_height;
  guint i;

  gst_video_alignment_reset (align);

  aligned_width = GST_ROUND_UP_N ((guint) GST_VIDEO_INFO_WIDTH (info),
      GST_DAV1D_PICTURE_PIXEL_ALIGN);
  aligned_height = GST_ROUND_UP_N ((guint) GST_VIDEO_INFO_HEIGHT (info),
      GST_DAV1D_PICTURE_PIXEL_ALIGN);

  align->padding_right = aligned_width - GST_VIDEO_INFO_WIDTH (info);
  align->padding_bottom = aligned_height - GST_VIDEO_INFO_HEIGHT (info);

  if (downstream != NULL) {
    align->padding_top = downstream->padding_top;
    align->padding_left = downstream->padding_left;
    align->padding_right = MAX (align->padding_right, downstream->padding_right);
    align->padding_bottom =
        MAX (align->padding_bottom, downstream->padding_bottom);
    for (i = 0; i < GST_VIDEO_MAX_PLANES; i++)
      stride_align |= downstream->stride_align[i];
  }

  /* GstVideoBufferPool merges the per-plane stride alignments into one and
   * applies it to every plane. Do the same so that the layout computed here
   * is the one the pool will actually produce. */
  for (i = 0; i < GST_VIDEO_MAX_PLANES; i++)
    align->stride_align[i] = stride_align;

  return gst_video_info_align (info, align);
}

/* Whether dav1d can decode straight into buffers with this layout, that is
 * whether every plane starts and every row ends on a DAV1D_PICTURE_ALIGNMENT
 * boundary. */
static gboolean
gst_dav1d_dec_info_is_usable (const GstVideoInfo * info)
{
  guint i, n_planes = GST_VIDEO_INFO_N_PLANES (info);

  for (i = 0; i < n_planes; i++) {
    if (GST_VIDEO_INFO_PLANE_OFFSET (info, i) % DAV1D_PICTURE_ALIGNMENT != 0)
      return FALSE;
    if (GST_VIDEO_INFO_PLANE_STRIDE (info, i) % DAV1D_PICTURE_ALIGNMENT != 0)
      return FALSE;
  }

  /* dav1d uses a single stride for both chroma planes. */
  if (n_planes == 3
      && GST_VIDEO_INFO_PLANE_STRIDE (info, 1)
      != GST_VIDEO_INFO_PLANE_STRIDE (info, 2))
    return FALSE;

  return TRUE;
}

static gboolean
gst_dav1d_dec_try_pool (GstDav1dDec * self, GstCaps * caps,
    const GstVideoInfo * info, GstBufferPool * pool, guint * size, guint min,
    guint max, GstAllocator * allocator, const GstAllocationParams * params,
    const GstVideoAlignment * downstream_align, gboolean video_meta_supported,
    GstVideoInfo * pool_info)
{
  GstStructure *config;
  GstVideoInfo aligned_info = *info;
  GstAllocationParams aligned_params = *params;
  GstVideoAlignment align;
  guint aligned_size;
  gboolean alignment_supported;
  guint i, max_align;

  GST_DEBUG_OBJECT (self, "Trying pool %s with allocator %s",
      GST_OBJECT_NAME (pool), allocator ? GST_OBJECT_NAME (allocator) : "none");

  config = gst_buffer_pool_get_config (pool);

  video_meta_supported = video_meta_supported
      && gst_buffer_pool_has_option (pool, GST_BUFFER_POOL_OPTION_VIDEO_META);
  alignment_supported = video_meta_supported
      && gst_buffer_pool_has_option (pool,
      GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);

  if (video_meta_supported)
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);

  aligned_size = MAX (*size, (guint) GST_VIDEO_INFO_SIZE (info));

  if (alignment_supported) {
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);

    if (!gst_dav1d_dec_align_info (&aligned_info, &align, downstream_align,
            (guint) params->align)) {
      GST_DEBUG_OBJECT (self, "Failed to align video info");
      gst_structure_free (config);
      return FALSE;
    }

    gst_buffer_pool_config_set_video_alignment (config, &align);
    aligned_size = MAX (aligned_size, (guint) GST_VIDEO_INFO_SIZE (&aligned_info));

    max_align = (guint) params->align;
    for (i = 0; i < GST_VIDEO_MAX_PLANES; i++)
      max_align |= align.stride_align[i];
    aligned_params.align = max_align;
  }

  gst_buffer_pool_config_set_allocator (config, allocator, &aligned_params);
  gst_buffer_pool_config_set_params (config, caps, aligned_size, min, max);

  if (!gst_buffer_pool_set_config (pool, config)) {
    config = gst_buffer_pool_get_config (pool);
    if (!gst_buffer_pool_config_validate_params (config, caps, aligned_size,
            min, max)) {
      gst_structure_free (config);
      return FALSE;
    }
    if (!gst_buffer_pool_set_config (pool, config))
      return FALSE;
  }

  *size = aligned_size;
  if (pool_info != NULL)
    *pool_info = aligned_info;

  return TRUE;
}

static gboolean
gst_dav1d_dec_decide_allocation (GstVideoDecoder * decoder, GstQuery * query)
{
  GstDav1dDec *self = GST_DAV1D_DEC (decoder);
  GstVideoCodecState *output_state;
  GstVideoInfo info, pool_info, selected_info;
  GstVideoAlignment downstream_align;
  GstAllocator *allocator = NULL;
  GstAllocationParams params;
  GstBufferPool *selected_pool = NULL;
  GstCaps *caps;
  guint index = 0;
  guint i, n_pools;
  guint selected_size, selected_min = 0, selected_max = 0;
  gboolean have_first_pool = FALSE;
  guint first_pool_min = 0, first_pool_max = 0;

  GST_DEBUG_OBJECT (self, "Renegotiating allocation");

  /* Pictures dav1d is still decoding into buffers of the old pool stay
   * valid: an outstanding buffer keeps its pool alive, and the pool is
   * deactivated when the last one is released. */
  gst_dav1d_dec_install_pool (self, NULL, NULL);

  self->video_meta_supported =
      gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, &index);

  output_state = gst_video_decoder_get_output_state (decoder);
  if (output_state == NULL) {
    GST_WARNING_OBJECT (self, "No output state set");
    return TRUE;
  }

  caps = output_state->caps ? gst_caps_ref (output_state->caps) : NULL;
  info = output_state->info;
  gst_video_codec_state_unref (output_state);

  if (caps == NULL) {
    GST_WARNING_OBJECT (self, "No output caps set");
    return TRUE;
  }

  /* Downstream may require padding of its own, for instance for a hardware
   * sink. Honour it on top of what dav1d needs. */
  gst_video_alignment_reset (&downstream_align);
  if (self->video_meta_supported) {
    const GstStructure *meta_params = NULL;

    gst_query_parse_nth_allocation_meta (query, index, &meta_params);
    if (meta_params != NULL
        && gst_structure_has_name (meta_params, "video-meta"))
      gst_buffer_pool_config_get_video_alignment ((GstStructure *) meta_params,
          &downstream_align);
  }

  gst_allocation_params_init (&params);
  if (gst_query_get_n_allocation_params (query) > 0)
    gst_query_parse_nth_allocation_param (query, 0, &allocator, &params);
  params.align = MAX (params.align, DAV1D_PICTURE_ALIGNMENT - 1);
  params.padding = MAX (params.padding, DAV1D_PICTURE_ALIGNMENT);

  selected_size = GST_VIDEO_INFO_SIZE (&info);
  selected_info = info;

  /* Use the first pool that can hand out at least 32 buffers. dav1d keeps a
   * good number of pictures alive as reference frames, so a pool with a low
   * maximum would starve the decoder. */
  n_pools = gst_query_get_n_allocation_pools (query);
  for (i = 0; i < n_pools; i++) {
    GstBufferPool *pool = NULL;
    guint size, min, max;

    gst_query_parse_nth_allocation_pool (query, i, &pool, &size, &min, &max);

    if (!have_first_pool) {
      first_pool_min = min;
      have_first_pool = TRUE;
    }

    if (max != 0 && max < 32) {
      /* The allocator may well be the reason for the limit, so drop it too
       * and fall back to the default one. */
      if (allocator != NULL) {
        gst_object_unref (allocator);
        allocator = NULL;
      }
      gst_clear_object (&pool);
      continue;
    }

    first_pool_max = max;

    if (pool == NULL)
      continue;

    selected_size = size;
    selected_min = min;
    selected_max = max;
    pool_info = info;

    if (gst_dav1d_dec_try_pool (self, caps, &info, pool, &selected_size,
            selected_min, selected_max, allocator, &params, &downstream_align,
            self->video_meta_supported, &pool_info)) {
      selected_pool = pool;
      selected_info = pool_info;
      break;
    }

    gst_object_unref (pool);
  }

  if (selected_pool == NULL) {
    GstBufferPool *pool = gst_video_buffer_pool_new ();

    selected_min = have_first_pool ? first_pool_min : 0;
    selected_max = have_first_pool ? first_pool_max : 0;
    selected_size = GST_VIDEO_INFO_SIZE (&info);
    pool_info = info;

    if (gst_dav1d_dec_try_pool (self, caps, &info, pool, &selected_size,
            selected_min, selected_max, allocator, &params, &downstream_align,
            self->video_meta_supported, &pool_info)) {
      selected_pool = pool;
      selected_info = pool_info;
    } else {
      gst_object_unref (pool);
    }
  }

  if (selected_pool != NULL) {
    gboolean usable = self->video_meta_supported
        && gst_buffer_pool_has_option (selected_pool,
        GST_BUFFER_POOL_OPTION_VIDEO_META)
        && gst_buffer_pool_has_option (selected_pool,
        GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT)
        && gst_dav1d_dec_info_is_usable (&selected_info);

    GST_DEBUG_OBJECT (self,
        "Selected pool %s, allocator %s (video meta: %d, usable for dav1d: %d)",
        GST_OBJECT_NAME (selected_pool),
        allocator ? GST_OBJECT_NAME (allocator) : "none",
        self->video_meta_supported, usable);

    if (usable)
      gst_dav1d_dec_install_pool (self, selected_pool, &selected_info);
  } else {
    GST_DEBUG_OBJECT (self, "Could not configure any pool");
  }

  /* Downstream offered nothing dav1d can decode into: use a pool of our own.
   * Its buffers still reach downstream without a copy when downstream reads
   * the video meta, and are copied in handle_picture() otherwise. */
  if (self->pool == NULL && !self->shift_output) {
    GstVideoInfo internal_info;
    GstBufferPool *internal_pool;

    internal_pool = gst_dav1d_dec_create_internal_pool (self, caps, &info,
        &internal_info);
    if (internal_pool != NULL) {
      gst_dav1d_dec_install_pool (self, internal_pool, &internal_info);
      gst_object_unref (internal_pool);
    }
  }

  if (n_pools > 0) {
    gst_query_set_nth_allocation_pool (query, 0, selected_pool, selected_size,
        selected_min, selected_max);
  } else {
    gst_query_add_allocation_pool (query, selected_pool, selected_size,
        selected_min, selected_max);
  }

  if (gst_query_get_n_allocation_params (query) > 0)
    gst_query_set_nth_allocation_param (query, 0, allocator, &params);
  else
    gst_query_add_allocation_param (query, allocator, &params);

  gst_clear_object (&selected_pool);
  gst_clear_object (&allocator);
  gst_caps_unref (caps);

  return TRUE;
}

/* Pool used when downstream offered nothing dav1d can decode into. Returns
 * a new, configured but not yet active pool, or NULL. */
static GstBufferPool *
gst_dav1d_dec_create_internal_pool (GstDav1dDec * self, GstCaps * caps,
    const GstVideoInfo * info, GstVideoInfo * pool_info)
{
  GstBufferPool *pool;
  GstStructure *config;
  GstAllocationParams params;
  GstVideoAlignment align;

  *pool_info = *info;

  if (!gst_dav1d_dec_align_info (pool_info, &align, NULL,
          DAV1D_PICTURE_ALIGNMENT - 1)) {
    GST_ERROR_OBJECT (self, "Failed to align video info");
    return NULL;
  }

  if (!gst_dav1d_dec_info_is_usable (pool_info)) {
    GST_ERROR_OBJECT (self, "Aligned layout is not usable by dav1d");
    return NULL;
  }

  GST_DEBUG_OBJECT (self, "Creating internal buffer pool");

  gst_allocation_params_init (&params);
  params.align = DAV1D_PICTURE_ALIGNMENT - 1;
  params.padding = DAV1D_PICTURE_ALIGNMENT;

  pool = gst_video_buffer_pool_new ();
  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_allocator (config, NULL, &params);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  gst_buffer_pool_config_add_option (config,
      GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
  gst_buffer_pool_config_set_video_alignment (config, &align);
  gst_buffer_pool_config_set_params (config, caps,
      GST_VIDEO_INFO_SIZE (pool_info), 0, 0);

  if (!gst_buffer_pool_set_config (pool, config)) {
    GST_ERROR_OBJECT (self, "Failed to configure internal buffer pool");
    gst_object_unref (pool);
    return NULL;
  }

  return pool;
}

/* Whether buffers from the current pool can go downstream untouched: always
 * when downstream reads the video meta, otherwise only when the padded
 * layout happens to coincide with the plain one. Informational only, the
 * decision is made per buffer in gst_dav1d_dec_can_forward(). */
static void
gst_dav1d_dec_update_direct_output (GstDav1dDec * self)
{
  gboolean direct = FALSE;

  if (self->output_state != NULL && self->pool != NULL && !self->shift_output) {
    direct = self->video_meta_supported
        || (GST_VIDEO_INFO_SIZE (&self->pool_info)
        == GST_VIDEO_INFO_SIZE (&self->output_state->info)
        && memcmp (self->pool_info.stride, self->output_state->info.stride,
            sizeof (self->pool_info.stride)) == 0
        && memcmp (self->pool_info.offset, self->output_state->info.offset,
            sizeof (self->pool_info.offset)) == 0);
  }

  GST_DEBUG_OBJECT (self, "Output buffers can be forwarded directly: %d",
      direct);
  self->direct_output = direct;
}

/* Make @pool, or no pool at all, the one the picture allocator decodes into.
 *
 * The pool is activated here, before it becomes visible to the allocator,
 * because dav1d worker threads may start allocating from it right away.
 * When the output state has already been negotiated for the current picture
 * parameters the pool is used for them immediately; otherwise it waits for
 * gst_dav1d_dec_ensure_output_state() to finish. Called on the streaming
 * thread only. */
static void
gst_dav1d_dec_install_pool (GstDav1dDec * self, GstBufferPool * pool,
    const GstVideoInfo * info)
{
  if (pool != NULL && self->shift_output) {
    GST_DEBUG_OBJECT (self, "Output needs scaling, not decoding into a pool");
    pool = NULL;
  }

  if (pool != NULL && !gst_buffer_pool_set_active (pool, TRUE)) {
    GST_WARNING_OBJECT (self, "Failed to activate buffer pool");
    pool = NULL;
  }

  g_mutex_lock (&self->alloc_lock);
  gst_clear_object (&self->pool);
  if (pool != NULL) {
    self->pool = gst_object_ref (pool);
    self->pool_info = *info;
    self->pool_params = self->negotiated_params;
  } else {
    self->pool_params.valid = FALSE;
  }
  g_mutex_unlock (&self->alloc_lock);

  gst_dav1d_dec_update_direct_output (self);
}

/* --------------------------------------------------------------------- */
/* Output negotiation, on the streaming thread                            */
/* --------------------------------------------------------------------- */

static void
gst_dav1d_dec_update_latency (GstDav1dDec * self)
{
  GstClockTime latency;
  gint fps_n, fps_d;

  if (self->output_state == NULL)
    return;

  fps_n = GST_VIDEO_INFO_FPS_N (&self->output_state->info);
  fps_d = GST_VIDEO_INFO_FPS_D (&self->output_state->info);
  if (fps_n <= 0 || fps_d <= 0) {
    /* Assume 30fps if the framerate is unknown, as upstream does. */
    fps_n = 30;
    fps_d = 1;
  }

  latency = gst_util_uint64_scale ((guint64) self->frame_delay * GST_SECOND,
      fps_d, fps_n);

  GST_DEBUG_OBJECT (self, "Reporting latency of %" GST_TIME_FORMAT
      " (%u frames)", GST_TIME_ARGS (latency), self->frame_delay);

  gst_video_decoder_set_latency (GST_VIDEO_DECODER (self), latency,
      GST_CLOCK_TIME_NONE);
}

/* Make sure the output state and buffer pool match @pic.
 *
 * Called from the output side, which is what makes it safe: dav1d hands out
 * pictures in presentation order, so by the time a picture with new
 * parameters shows up every picture in the old format has already been
 * pushed. No draining is needed and, unlike negotiating from the allocator,
 * this never runs on a dav1d worker thread. */
static gboolean
gst_dav1d_dec_ensure_output_state (GstDav1dDec * self, const Dav1dPicture * pic)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER (self);
  GstDav1dPicParams params;
  GstVideoCodecState *state;
  GstVideoFormat format;
  GstVideoColorimetry cinfo;
  GstVideoChromaSite site;

  gst_dav1d_dec_pic_params (pic, &params);

  if (self->output_state != NULL
      && gst_dav1d_dec_pic_params_equal (&params, &self->negotiated_params))
    return TRUE;

  format = gst_dav1d_dec_video_format_from_picture (self, pic);
  if (format == GST_VIDEO_FORMAT_UNKNOWN)
    return FALSE;

  gst_dav1d_dec_colorimetry_from_picture (self, pic, &cinfo);
  site = gst_dav1d_dec_chroma_site_from_picture (self, pic);

  GST_DEBUG_OBJECT (self, "Negotiating %s %dx%d",
      gst_video_format_to_string (format), pic->p.w, pic->p.h);

  /* Drop the old pool first so the allocator falls back to plain buffers for
   * pictures started while we are between formats, instead of decoding into
   * buffers with the previous layout. */
  self->negotiated_params.valid = FALSE;
  gst_dav1d_dec_install_pool (self, NULL, NULL);

  /* High bit depth grayscale is described as GRAY16 and has to be scaled
   * while copying, see gst_dav1d_dec_copy_picture(). */
  self->shift_output = (pic->p.layout == DAV1D_PIXEL_LAYOUT_I400
      && pic->p.bpc > 8 && pic->p.bpc < 16);

  state = gst_video_decoder_set_output_state (decoder, format, pic->p.w,
      pic->p.h, self->input_state);
  if (state == NULL) {
    GST_ERROR_OBJECT (self, "Failed to set output state");
    return FALSE;
  }

  state->info.colorimetry = cinfo;
  state->info.chroma_site = site;

  if (self->output_state != NULL)
    gst_video_codec_state_unref (self->output_state);
  self->output_state = state;

  /* Runs decide_allocation(), which installs the pool to decode into. */
  if (!gst_video_decoder_negotiate (decoder)) {
    GST_ERROR_OBJECT (self, "Failed to negotiate");
    return FALSE;
  }

  if (self->pool == NULL && !self->shift_output)
    GST_WARNING_OBJECT (self,
        "No pool to decode into, every picture will be copied");

  /* From here on the allocator may use the pool for these pictures. */
  self->negotiated_params = params;
  g_mutex_lock (&self->alloc_lock);
  if (self->pool != NULL)
    self->pool_params = params;
  g_mutex_unlock (&self->alloc_lock);

  gst_dav1d_dec_update_direct_output (self);
  gst_dav1d_dec_update_latency (self);

  return TRUE;
}

/* --------------------------------------------------------------------- */
/* Output                                                                 */
/* --------------------------------------------------------------------- */

static void
gst_dav1d_dec_copy_picture (GstDav1dDec * self, const Dav1dPicture * pic,
    GstVideoFrame * dest)
{
  guint p, n_planes = GST_VIDEO_FRAME_N_PLANES (dest);
  guint shift = 0;

  GST_TRACE_OBJECT (self, "Copying decoded picture to output buffer");

  /* GRAY16 carries full range 16 bit samples, dav1d's 10 and 12 bit
   * grayscale sits in the low bits of 16 bit words. Scale it up. */
  if (self->shift_output)
    shift = 16 - pic->p.bpc;

  for (p = 0; p < n_planes; p++) {
    const guint8 *src = pic->data[p];
    guint8 *dst = GST_VIDEO_FRAME_PLANE_DATA (dest, p);
    gint src_stride = (gint) pic->stride[p == 0 ? 0 : 1];
    gint dst_stride = GST_VIDEO_FRAME_PLANE_STRIDE (dest, p);
    gint width = GST_VIDEO_FRAME_COMP_WIDTH (dest, p)
        * GST_VIDEO_FRAME_COMP_PSTRIDE (dest, p);
    gint height = GST_VIDEO_FRAME_COMP_HEIGHT (dest, p);
    gint l, x;

    if (shift != 0) {
      gint n = width / 2;

      for (l = 0; l < height; l++) {
        const guint16 *s = (const guint16 *) src;
        guint16 *d = (guint16 *) dst;

        for (x = 0; x < n; x++)
          d[x] = s[x] << shift;
        dst += dst_stride;
        src += src_stride;
      }
    } else if (src_stride == dst_stride) {
      /* dav1d pads every plane, so copying the trailing bytes of the last row
       * from the source is safe, and one big copy beats a loop. */
      memcpy (dst, src, (gsize) dst_stride * (height - 1) + width);
    } else {
      for (l = 0; l < height; l++) {
        memcpy (dst, src, width);
        dst += dst_stride;
        src += src_stride;
      }
    }
  }
}

/* Whether the buffer an allocation holds can be handed downstream untouched.
 *
 * Besides needing a pooled buffer and downstream that accepts the padded
 * layout, the allocation must not already back an output frame, and its layout
 * must still agree with the negotiated output. The latter can differ after a
 * mid-stream format change, because show_existing_frame can show a picture
 * that was allocated several formats ago. Anything else is copied. */
static gboolean
gst_dav1d_dec_can_forward (GstDav1dDec * self, const GstDav1dAllocation * alloc)
{
  const GstVideoInfo *out, *in;

  if (alloc == NULL || !alloc->from_pool || self->shift_output)
    return FALSE;

  if (alloc->codec_frame != NULL) {
    /* One allocation backing two output frames, as show_existing_frame
     * produces. Copying the second one keeps the reference count of the
     * first buffer at one, which is what keeps it from being copied. */
    GST_DEBUG_OBJECT (self, "Buffer %p already output once, copying instead",
        alloc->buffer);
    return FALSE;
  }

  out = &self->output_state->info;
  in = &alloc->vframe.info;

  if (GST_VIDEO_INFO_FORMAT (in) != GST_VIDEO_INFO_FORMAT (out)
      || GST_VIDEO_INFO_WIDTH (in) != GST_VIDEO_INFO_WIDTH (out)
      || GST_VIDEO_INFO_HEIGHT (in) != GST_VIDEO_INFO_HEIGHT (out)) {
    GST_DEBUG_OBJECT (self,
        "Allocation layout no longer matches the output state, copying");
    return FALSE;
  }

  /* Without a video meta downstream assumes the plain layout, so the padded
   * one this particular buffer has must coincide with it. Checked per buffer
   * rather than per pool because a buffer may outlive the pool it came from
   * across a reconfiguration. */
  if (!self->video_meta_supported
      && (gst_buffer_get_size (alloc->buffer) < GST_VIDEO_INFO_SIZE (out)
          || memcmp (in->stride, out->stride, sizeof (out->stride)) != 0
          || memcmp (in->offset, out->offset, sizeof (out->offset)) != 0)) {
    GST_DEBUG_OBJECT (self,
        "Downstream needs the plain layout and this buffer differs, copying");
    return FALSE;
  }

  return TRUE;
}

/* Hand the buffer @pic was decoded into to @frame.
 *
 * No reference is added: the codec frame takes over the single reference
 * the allocation holds (the mapping holds none, see
 * gst_dav1d_dec_alloc_from_pool()). Keeping the codec frame alive until dav1d
 * releases the picture keeps the buffer alive for as long as dav1d may still
 * read it as a reference frame.
 *
 * The buffer therefore reaches gst_video_decoder_finish_frame() with a
 * reference count of one, which is what keeps the base class from copying it:
 * it calls gst_buffer_make_writable() on the output buffer, and a shared,
 * write-mapped memory cannot be shared, so it would be copied in full. The
 * base class then takes a second reference for the push, so downstream sees
 * a non-writable buffer and copies before modifying it in place, which
 * protects the reference frame. */
static void
gst_dav1d_dec_freeze_allocation (GstDav1dDec * self,
    GstDav1dAllocation * alloc, GstVideoCodecFrame * frame)
{
  g_assert (alloc->codec_frame == NULL);

  alloc->codec_frame = gst_video_codec_frame_ref (frame);

  if (frame->output_buffer != NULL)
    gst_buffer_unref (frame->output_buffer);
  frame->output_buffer = alloc->buffer;
}

/* Release the codec frames of input that produced no picture.
 *
 * dav1d hands out pictures in input order, so once the picture for frame
 * @current shows up no earlier frame can produce one any more. That happens
 * with frame-aligned input, where a hidden frame is a buffer of its own and
 * the picture it holds is attributed to the later show_existing_frame buffer,
 * with decode-frame-type skipping frames, and with damaged input. The base
 * class would otherwise keep them, along with their input buffers, until the
 * next flush, and let them take part in its timestamp reconstruction. */
static void
gst_dav1d_dec_release_stale_frames (GstDav1dDec * self, guint32 current)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER (self);
  GList *frames, *l;

  frames = gst_video_decoder_get_frames (decoder);
  for (l = frames; l != NULL; l = l->next) {
    GstVideoCodecFrame *frame = l->data;

    if (frame->system_frame_number < current) {
      GST_DEBUG_OBJECT (self, "Frame %u produced no picture, releasing it",
          frame->system_frame_number);
      /* Takes over the list's reference. */
      gst_video_decoder_release_frame (decoder, frame);
    } else {
      gst_video_codec_frame_unref (frame);
    }
  }
  g_list_free (frames);
}

static GstFlowReturn
gst_dav1d_dec_handle_picture (GstDav1dDec * self, Dav1dPicture * pic)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER (self);
  GstDav1dAllocation *alloc = pic->allocator_data;
  GstVideoCodecFrame *frame;
  GstFlowReturn ret;

  if (!gst_dav1d_dec_ensure_output_state (self, pic))
    return GST_FLOW_NOT_NEGOTIATED;

  frame = gst_video_decoder_get_frame (decoder, (int) pic->m.offset);
  if (frame == NULL) {
    GST_WARNING_OBJECT (self, "No frame for offset %" G_GINT64_FORMAT,
        pic->m.offset);
    return GST_FLOW_OK;
  }

  GST_TRACE_OBJECT (self, "Handling picture for frame %u",
      frame->system_frame_number);

  gst_dav1d_dec_release_stale_frames (self, frame->system_frame_number);

  if (pic->m.duration > 0)
    frame->duration = (GstClockTime) pic->m.duration;

  if (gst_dav1d_dec_can_forward (self, alloc)) {
    gst_dav1d_dec_freeze_allocation (self, alloc, frame);
  } else {
    GstVideoFrame vframe;

    ret = gst_video_decoder_allocate_output_frame (decoder, frame);
    if (ret != GST_FLOW_OK) {
      GST_ERROR_OBJECT (self, "Failed to allocate output frame");
      gst_video_codec_frame_unref (frame);
      return ret;
    }

    if (!gst_video_frame_map (&vframe, &self->output_state->info,
            frame->output_buffer, GST_MAP_WRITE)) {
      GST_ERROR_OBJECT (self, "Failed to map output buffer");
      gst_video_codec_frame_unref (frame);
      return GST_FLOW_ERROR;
    }

    gst_dav1d_dec_copy_picture (self, pic, &vframe);
    gst_video_frame_unmap (&vframe);
  }

  return gst_video_decoder_finish_frame (decoder, frame);
}

static GstFlowReturn
gst_dav1d_dec_forward_pictures (GstDav1dDec * self, gboolean drain)
{
  /* dav1d wants get_picture() called a second time after it returned EAGAIN
   * in order to actually hand out everything it has buffered. */
  gboolean call_twice = drain;
  GstFlowReturn ret = GST_FLOW_OK;

  if (self->decoder == NULL)
    return GST_FLOW_FLUSHING;

  for (;;) {
    for (;;) {
      Dav1dPicture pic;
      int res;

      memset (&pic, 0, sizeof (pic));
      res = dav1d_get_picture (self->decoder, &pic);

      if (res == DAV1D_ERR (EAGAIN)) {
        GST_TRACE_OBJECT (self, "Decoder needs more data");
        break;
      }

      if (res < 0) {
        GST_ERROR_OBJECT (self, "Failed to get picture (error %d)", res);
        GST_VIDEO_DECODER_ERROR (self, 1, STREAM, DECODE, (NULL),
            ("Failed to retrieve decoded picture (error %d)", res), ret);
        return ret;
      }

      GST_TRACE_OBJECT (self, "Retrieved picture with offset %"
          G_GINT64_FORMAT, pic.m.offset);

      ret = gst_dav1d_dec_handle_picture (self, &pic);
      dav1d_picture_unref (&pic);
      call_twice = FALSE;

      if (ret != GST_FLOW_OK)
        return ret;

      if (!drain)
        break;
    }

    if (!call_twice)
      break;
    call_twice = FALSE;
  }

  return ret;
}

/* --------------------------------------------------------------------- */
/* Input                                                                  */
/* --------------------------------------------------------------------- */

typedef struct
{
  GstBuffer *buffer;
  GstMapInfo map;
} GstDav1dInput;

static void
gst_dav1d_dec_input_free (const guint8 * buf, void *cookie)
{
  GstDav1dInput *input = cookie;

  gst_buffer_unmap (input->buffer, &input->map);
  gst_buffer_unref (input->buffer);
  g_free (input);
}

static GstFlowReturn
gst_dav1d_dec_send_pending_data (GstDav1dDec * self, gboolean * again,
    gboolean * release_frame)
{
  GstFlowReturn ret = GST_FLOW_OK;
  int res;

  *again = FALSE;

  if (self->pending_data.sz == 0)
    return GST_FLOW_OK;

  res = dav1d_send_data (self->decoder, &self->pending_data);

  if (res == 0) {
    GST_TRACE_OBJECT (self, "Decoder accepted the data");
    return GST_FLOW_OK;
  }

  if (res == DAV1D_ERR (EAGAIN)) {
    GST_TRACE_OBJECT (self, "Decoder needs output drained first");
    *again = TRUE;
    return GST_FLOW_OK;
  }

  /* Anything else means the data was rejected. dav1d has already dropped its
   * own reference to it by then, so drop ours as well rather than handing the
   * same buffer back: resubmitting it would re-parse OBUs dav1d has already
   * rejected and spin until the base class error count runs out. */
  dav1d_data_unref (&self->pending_data);

  if (res == DAV1D_ERR (EINVAL)) {
    /* A broken bitstream is not fatal on its own: report it and carry on so
     * that a damaged stream keeps playing. */
    GST_WARNING_OBJECT (self, "Bitstream error");
    GST_VIDEO_DECODER_ERROR (self, 1, STREAM, DECODE, (NULL),
        ("Bitstream error"), ret);
    return ret;
  }

  GST_ERROR_OBJECT (self, "Failed to send data (error %d)", res);
  if (release_frame != NULL)
    *release_frame = TRUE;
  GST_VIDEO_DECODER_ERROR (self, 1, STREAM, DECODE, (NULL),
      ("Failed to send data to the decoder (error %d)", res), ret);

  return ret;
}

static GstFlowReturn
gst_dav1d_dec_send_data (GstDav1dDec * self, GstVideoCodecFrame * frame,
    gboolean * again, gboolean * release_frame)
{
  GstDav1dInput *input;

  *again = FALSE;
  *release_frame = FALSE;

  if (frame->input_buffer == NULL)
    return GST_FLOW_ERROR;

  GST_TRACE_OBJECT (self, "Sending data for frame %u",
      frame->system_frame_number);

  input = g_new0 (GstDav1dInput, 1);
  input->buffer = gst_buffer_ref (frame->input_buffer);

  if (!gst_buffer_map (input->buffer, &input->map, GST_MAP_READ)) {
    GST_ERROR_OBJECT (self, "Failed to map input buffer");
    gst_buffer_unref (input->buffer);
    g_free (input);
    return GST_FLOW_ERROR;
  }

  /* The bitstream is handed to dav1d in place, no copy of the input is made.
   * dav1d holds on to the reference until it is done with the data and then
   * calls gst_dav1d_dec_input_free(). */
  if (dav1d_data_wrap (&self->pending_data, input->map.data, input->map.size,
          gst_dav1d_dec_input_free, input) < 0) {
    GST_ERROR_OBJECT (self, "Failed to wrap input data");
    gst_dav1d_dec_input_free (NULL, input);
    return GST_FLOW_ERROR;
  }

  /* The offset carries the frame number back to us on the decoded picture. */
  self->pending_data.m.offset = frame->system_frame_number;
  self->pending_data.m.timestamp = GST_CLOCK_TIME_IS_VALID (frame->dts)
      ? (gint64) frame->dts : INT64_MIN;
  self->pending_data.m.duration = GST_CLOCK_TIME_IS_VALID (frame->duration)
      ? (gint64) frame->duration : 0;

  return gst_dav1d_dec_send_pending_data (self, again, release_frame);
}

/* --------------------------------------------------------------------- */
/* GstVideoDecoder implementation                                         */
/* --------------------------------------------------------------------- */

static void
gst_dav1d_dec_close_decoder (GstDav1dDec * self)
{
  if (self->pending_data.sz > 0)
    dav1d_data_unref (&self->pending_data);
  memset (&self->pending_data, 0, sizeof (self->pending_data));

  if (self->decoder != NULL) {
    dav1d_close (&self->decoder);
    self->decoder = NULL;
  }

  g_mutex_lock (&self->alloc_lock);
  if (self->pool != NULL) {
    gst_buffer_pool_set_active (self->pool, FALSE);
    gst_clear_object (&self->pool);
  }
  self->pool_params.valid = FALSE;
  g_mutex_unlock (&self->alloc_lock);

  self->negotiated_params.valid = FALSE;
  self->shift_output = FALSE;
  self->direct_output = FALSE;

  if (self->output_state != NULL) {
    gst_video_codec_state_unref (self->output_state);
    self->output_state = NULL;
  }
}

static gboolean
gst_dav1d_dec_start (GstVideoDecoder * decoder)
{
  GstDav1dDec *self = GST_DAV1D_DEC (decoder);

  self->direct_output = FALSE;
  self->video_meta_supported = FALSE;
  self->frame_delay = 1;

  return TRUE;
}

static gboolean
gst_dav1d_dec_stop (GstVideoDecoder * decoder)
{
  GstDav1dDec *self = GST_DAV1D_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Stopping");

  gst_dav1d_dec_close_decoder (self);

  if (self->input_state != NULL) {
    gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;
  }

  return TRUE;
}

static gboolean
gst_dav1d_dec_set_format (GstVideoDecoder * decoder, GstVideoCodecState * state)
{
  GstDav1dDec *self = GST_DAV1D_DEC (decoder);
  GstStructure *structure;
  Dav1dSettings settings;
  gint64 max_frame_delay;
  int res;

  GST_DEBUG_OBJECT (self, "Setting format %" GST_PTR_FORMAT, state->caps);

  /* Let the old decoder finish what it has before reconfiguring. */
  if (self->decoder != NULL)
    gst_dav1d_dec_forward_pictures (self, TRUE);
  gst_dav1d_dec_close_decoder (self);

  dav1d_default_settings (&settings);

  GST_OBJECT_LOCK (self);
  settings.n_threads = self->n_threads;
  settings.apply_grain = self->apply_grain ? 1 : 0;
  settings.inloop_filters = (enum Dav1dInloopFilterType) self->inloop_filters;
  settings.decode_frame_type =
      (enum Dav1dDecodeFrameType) self->decode_frame_type;
  settings.frame_size_limit = self->frame_size_limit;
  settings.all_layers = self->output_all_layers ? 1 : 0;
  settings.operating_point = self->operating_point;
  settings.strict_std_compliance = self->strict_std_compliance ? 1 : 0;
  max_frame_delay = self->max_frame_delay;
  GST_OBJECT_UNLOCK (self);

  if (max_frame_delay < 0) {
    /* Autodetect: minimal delay when live, dav1d's own choice otherwise. */
    GstQuery *query = gst_query_new_latency ();
    gboolean live = FALSE;

    if (gst_pad_peer_query (GST_VIDEO_DECODER_SINK_PAD (decoder), query))
      gst_query_parse_latency (query, &live, NULL, NULL);
    gst_query_unref (query);

    max_frame_delay = live ? 1 : 0;
    GST_INFO_OBJECT (self, "Pipeline is %s, using max-frame-delay %"
        G_GINT64_FORMAT, live ? "live" : "not live", max_frame_delay);
  }
  settings.max_frame_delay = (int) max_frame_delay;

  /* Decode straight into buffers we hand out. The callbacks may run on any
   * dav1d worker thread; see gst_dav1d_dec_alloc_picture(). */
  settings.allocator.cookie = self;
  settings.allocator.alloc_picture_callback = gst_dav1d_dec_alloc_picture;
  settings.allocator.release_picture_callback = gst_dav1d_dec_release_picture;

  GST_INFO_OBJECT (self,
      "Opening decoder with n-threads=%d max-frame-delay=%d",
      settings.n_threads, settings.max_frame_delay);

  res = dav1d_open (&self->decoder, &settings);
  if (res < 0) {
    GST_ELEMENT_ERROR (self, LIBRARY, INIT, (NULL),
        ("Failed to open dav1d decoder (error %d)", res));
    return FALSE;
  }

  /* dav1d knows its own frame delay exactly, no need to estimate it. */
  res = dav1d_get_frame_delay (&settings);
  self->frame_delay = (res > 0) ? (guint) res : 1;
  GST_INFO_OBJECT (self, "Decoder frame delay is %u", self->frame_delay);

  if (self->input_state != NULL)
    gst_video_codec_state_unref (self->input_state);
  self->input_state = gst_video_codec_state_ref (state);

  /* Snapshot what upstream told us about colour, so that the format mapping
   * never has to touch a refcounted object from a dav1d worker thread. */
  structure = gst_caps_get_structure (state->caps, 0);
  self->have_input_colorimetry =
      gst_structure_has_field (structure, "colorimetry");
  self->have_input_chroma_site =
      gst_structure_has_field (structure, "chroma-site");
  self->input_colorimetry = GST_VIDEO_INFO_COLORIMETRY (&state->info);
  self->input_chroma_site = GST_VIDEO_INFO_CHROMA_SITE (&state->info);

  return TRUE;
}

static GstFlowReturn
gst_dav1d_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstDav1dDec *self = GST_DAV1D_DEC (decoder);
  GstFlowReturn ret;
  gboolean again = FALSE;
  gboolean release_frame = FALSE;

  if (self->decoder == NULL) {
    gst_video_decoder_release_frame (decoder, frame);
    return GST_FLOW_NOT_NEGOTIATED;
  }

  ret = gst_dav1d_dec_send_data (self, frame, &again, &release_frame);
  if (release_frame) {
    /* Takes over our reference. */
    gst_video_decoder_release_frame (decoder, frame);
    return ret;
  }
  if (ret != GST_FLOW_OK)
    goto done;

  /* The decoder would not take the data: hand out what it has already
   * decoded until it does. */
  while (again) {
    ret = gst_dav1d_dec_forward_pictures (self, FALSE);
    if (ret != GST_FLOW_OK)
      goto done;

    ret = gst_dav1d_dec_send_pending_data (self, &again, NULL);
    if (ret != GST_FLOW_OK)
      goto done;
  }

  ret = gst_dav1d_dec_forward_pictures (self, FALSE);

done:
  gst_video_codec_frame_unref (frame);
  return ret;
}

static gboolean
gst_dav1d_dec_flush (GstVideoDecoder * decoder)
{
  GstDav1dDec *self = GST_DAV1D_DEC (decoder);

  GST_INFO_OBJECT (self, "Flushing");

  if (self->pending_data.sz > 0)
    dav1d_data_unref (&self->pending_data);

  if (self->decoder != NULL)
    dav1d_flush (self->decoder);

  return TRUE;
}

/* Note: GstVideoDecoder installs no default drain() or finish(), it only
 * calls them when a subclass provides one, so there is nothing to chain up
 * to here. */
static GstFlowReturn
gst_dav1d_dec_drain (GstVideoDecoder * decoder)
{
  GstDav1dDec *self = GST_DAV1D_DEC (decoder);
  GstFlowReturn ret = GST_FLOW_OK;

  GST_INFO_OBJECT (self, "Draining");

  if (self->decoder != NULL)
    ret = gst_dav1d_dec_forward_pictures (self, TRUE);

  if (ret == GST_FLOW_FLUSHING)
    ret = GST_FLOW_OK;

  return ret;
}

static GstFlowReturn
gst_dav1d_dec_finish (GstVideoDecoder * decoder)
{
  GstDav1dDec *self = GST_DAV1D_DEC (decoder);
  GstFlowReturn ret = GST_FLOW_OK;

  GST_INFO_OBJECT (self, "Finishing");

  if (self->decoder != NULL)
    ret = gst_dav1d_dec_forward_pictures (self, TRUE);

  if (ret == GST_FLOW_FLUSHING)
    ret = GST_FLOW_OK;

  return ret;
}

/* --------------------------------------------------------------------- */
/* GObject                                                                */
/* --------------------------------------------------------------------- */

static void
gst_dav1d_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstDav1dDec *self = GST_DAV1D_DEC (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_N_THREADS:
      self->n_threads = g_value_get_uint (value);
      break;
    case PROP_MAX_FRAME_DELAY:
      self->max_frame_delay = g_value_get_int64 (value);
      break;
    case PROP_APPLY_GRAIN:
      self->apply_grain = g_value_get_boolean (value);
      break;
    case PROP_INLOOP_FILTERS:
      self->inloop_filters = g_value_get_flags (value);
      break;
    case PROP_DECODE_FRAME_TYPE:
      self->decode_frame_type = g_value_get_enum (value);
      break;
    case PROP_FRAME_SIZE_LIMIT:
      self->frame_size_limit = g_value_get_uint (value);
      break;
    case PROP_OUTPUT_ALL_LAYERS:
      self->output_all_layers = g_value_get_boolean (value);
      break;
    case PROP_OPERATING_POINT:
      self->operating_point = g_value_get_uint (value);
      break;
    case PROP_STRICT_STD_COMPLIANCE:
      self->strict_std_compliance = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_dav1d_dec_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstDav1dDec *self = GST_DAV1D_DEC (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_N_THREADS:
      g_value_set_uint (value, self->n_threads);
      break;
    case PROP_MAX_FRAME_DELAY:
      g_value_set_int64 (value, self->max_frame_delay);
      break;
    case PROP_APPLY_GRAIN:
      g_value_set_boolean (value, self->apply_grain);
      break;
    case PROP_INLOOP_FILTERS:
      g_value_set_flags (value, self->inloop_filters);
      break;
    case PROP_DECODE_FRAME_TYPE:
      g_value_set_enum (value, self->decode_frame_type);
      break;
    case PROP_FRAME_SIZE_LIMIT:
      g_value_set_uint (value, self->frame_size_limit);
      break;
    case PROP_OUTPUT_ALL_LAYERS:
      g_value_set_boolean (value, self->output_all_layers);
      break;
    case PROP_OPERATING_POINT:
      g_value_set_uint (value, self->operating_point);
      break;
    case PROP_STRICT_STD_COMPLIANCE:
      g_value_set_boolean (value, self->strict_std_compliance);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_dav1d_dec_finalize (GObject * object)
{
  GstDav1dDec *self = GST_DAV1D_DEC (object);

  gst_dav1d_dec_close_decoder (self);

  if (self->input_state != NULL) {
    gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;
  }

  g_mutex_clear (&self->alloc_lock);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstCaps *
gst_dav1d_dec_sink_template_caps (void)
{
  guint major, minor, micro, nano;

  gst_version (&major, &minor, &micro, &nano);

  /* av1parse only gained the ability to produce these before 1.20. */
  if (major > 1 || (major == 1 && minor >= 19)) {
    return gst_caps_from_string ("video/x-av1, "
        "stream-format = (string) obu-stream, "
        "alignment = (string) { frame, tu }");
  }

  return gst_caps_from_string ("video/x-av1");
}

static GstCaps *
gst_dav1d_dec_src_template_caps (void)
{
  static const GstVideoFormat formats[] = {
    GST_VIDEO_FORMAT_GRAY8,
    GST_DAV1D_FORMAT_GRAY16,
    GST_VIDEO_FORMAT_I420,
    GST_VIDEO_FORMAT_Y42B,
    GST_VIDEO_FORMAT_Y444,
    GST_DAV1D_FORMAT_I420_10,
    GST_DAV1D_FORMAT_I422_10,
    GST_DAV1D_FORMAT_Y444_10,
    GST_DAV1D_FORMAT_I420_12,
    GST_DAV1D_FORMAT_I422_12,
    GST_DAV1D_FORMAT_Y444_12,
    /* AV1 codes RGB as 4:4:4 with the identity matrix. */
    GST_VIDEO_FORMAT_GBR,
    GST_DAV1D_FORMAT_GBR_10,
    GST_DAV1D_FORMAT_GBR_12
  };
  GValue list = G_VALUE_INIT;
  GValue item = G_VALUE_INIT;
  GstCaps *caps;
  guint i;

  g_value_init (&list, GST_TYPE_LIST);
  g_value_init (&item, G_TYPE_STRING);

  for (i = 0; i < G_N_ELEMENTS (formats); i++) {
    g_value_set_string (&item, gst_video_format_to_string (formats[i]));
    gst_value_list_append_value (&list, &item);
  }
  g_value_unset (&item);

  caps = gst_caps_new_simple ("video/x-raw",
      "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
      "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
      "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
  gst_caps_set_value (caps, "format", &list);
  g_value_unset (&list);

  return caps;
}

static void
gst_dav1d_dec_class_init (GstDav1dDecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoDecoderClass *decoder_class = GST_VIDEO_DECODER_CLASS (klass);
  GstCaps *caps;

  gobject_class->set_property = gst_dav1d_dec_set_property;
  gobject_class->get_property = gst_dav1d_dec_get_property;
  gobject_class->finalize = gst_dav1d_dec_finalize;

  g_object_class_install_property (gobject_class, PROP_N_THREADS,
      g_param_spec_uint ("n-threads", "Number of threads",
          "Number of threads to use while decoding "
          "(set to 0 to use the number of logical cores)",
          0, DAV1D_MAX_THREADS, DEFAULT_N_THREADS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_MAX_FRAME_DELAY,
      g_param_spec_int64 ("max-frame-delay", "Maximum frame delay",
          "Maximum delay in frames for the decoder (set to 1 for low latency, "
          "0 to be equal to the number of logical cores, -1 to choose between "
          "these two based on pipeline liveness)",
          -1, DAV1D_MAX_FRAME_DELAY, DEFAULT_MAX_FRAME_DELAY,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_APPLY_GRAIN,
      g_param_spec_boolean ("apply-grain", "Enable film grain synthesis",
          "Enable the out-of-loop normative film grain filter",
          DEFAULT_APPLY_GRAIN,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_INLOOP_FILTERS,
      g_param_spec_flags ("inloop-filters", "Inloop filters",
          "Flags to enable in-loop post processing filters. These are part of "
          "normative reconstruction: disabling any of them degrades every "
          "frame until the next keyframe, but is a large speed win",
          GST_TYPE_DAV1D_INLOOP_FILTER_TYPE, DEFAULT_INLOOP_FILTERS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_DECODE_FRAME_TYPE,
      g_param_spec_enum ("decode-frame-type", "Decode frame type",
          "Which frames to decode. Anything but 'all' produces an incomplete "
          "stream and is meant for staying realtime on slow hardware",
          GST_TYPE_DAV1D_DECODE_FRAME_TYPE, DEFAULT_DECODE_FRAME_TYPE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_FRAME_SIZE_LIMIT,
      g_param_spec_uint ("frame-size-limit", "Frame size limit",
          "Maximum frame size in pixels the decoder will accept (0 = unlimited)",
          0, G_MAXUINT, DEFAULT_FRAME_SIZE_LIMIT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_OUTPUT_ALL_LAYERS,
      g_param_spec_boolean ("output-all-layers", "Output all layers",
          "Output all spatial layers of a scalable AV1 bitstream",
          DEFAULT_OUTPUT_ALL_LAYERS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_OPERATING_POINT,
      g_param_spec_uint ("operating-point", "Operating point",
          "Operating point to select from a scalable AV1 bitstream",
          0, 31, DEFAULT_OPERATING_POINT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_STRICT_STD_COMPLIANCE,
      g_param_spec_boolean ("strict-std-compliance", "Strict std compliance",
          "Abort decoding on standard compliance violations that do not "
          "affect the actual bitstream decoding",
          DEFAULT_STRICT_STD_COMPLIANCE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  caps = gst_dav1d_dec_sink_template_caps ();
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
  gst_caps_unref (caps);

  caps = gst_dav1d_dec_src_template_caps ();
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
  gst_caps_unref (caps);

  gst_element_class_set_static_metadata (element_class, "dav1d AV1 Decoder",
      "Codec/Decoder/Video", "Decode AV1 video streams with dav1d",
      "Barracuda project");

  decoder_class->start = GST_DEBUG_FUNCPTR (gst_dav1d_dec_start);
  decoder_class->stop = GST_DEBUG_FUNCPTR (gst_dav1d_dec_stop);
  decoder_class->set_format = GST_DEBUG_FUNCPTR (gst_dav1d_dec_set_format);
  decoder_class->handle_frame = GST_DEBUG_FUNCPTR (gst_dav1d_dec_handle_frame);
  decoder_class->flush = GST_DEBUG_FUNCPTR (gst_dav1d_dec_flush);
  decoder_class->drain = GST_DEBUG_FUNCPTR (gst_dav1d_dec_drain);
  decoder_class->finish = GST_DEBUG_FUNCPTR (gst_dav1d_dec_finish);
  decoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_dav1d_dec_decide_allocation);

  gst_type_mark_as_plugin_api (GST_TYPE_DAV1D_INLOOP_FILTER_TYPE, 0);
  gst_type_mark_as_plugin_api (GST_TYPE_DAV1D_DECODE_FRAME_TYPE, 0);

  GST_DEBUG_CATEGORY_INIT (dav1d_dec_debug, "dav1ddec", 0, "dav1d AV1 decoder");
}

static void
gst_dav1d_dec_init (GstDav1dDec * self)
{
  g_mutex_init (&self->alloc_lock);

  self->n_threads = DEFAULT_N_THREADS;
  self->max_frame_delay = DEFAULT_MAX_FRAME_DELAY;
  self->apply_grain = DEFAULT_APPLY_GRAIN;
  self->inloop_filters = DEFAULT_INLOOP_FILTERS;
  self->decode_frame_type = DEFAULT_DECODE_FRAME_TYPE;
  self->frame_size_limit = DEFAULT_FRAME_SIZE_LIMIT;
  self->output_all_layers = DEFAULT_OUTPUT_ALL_LAYERS;
  self->operating_point = DEFAULT_OPERATING_POINT;
  self->strict_std_compliance = DEFAULT_STRICT_STD_COMPLIANCE;
  self->frame_delay = 1;

  gst_video_decoder_set_packetized (GST_VIDEO_DECODER (self), TRUE);
  gst_video_decoder_set_needs_format (GST_VIDEO_DECODER (self), TRUE);
  GST_PAD_SET_ACCEPT_TEMPLATE (GST_VIDEO_DECODER_SINK_PAD (self));
}
