/* GStreamer dav1d AV1 decoder plugin
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstdav1ddec.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  guint major, minor, micro, nano;
  guint rank = GST_RANK_PRIMARY;

  gst_version (&major, &minor, &micro, &nano);

  /* The libaom based av1dec had its rank demoted during the 1.22 development
   * cycle. On anything older than that we have to outrank it explicitly.
   * See gstreamer!3287. */
  if (major == 1 && (minor < 21 || (minor == 21 && (micro < 2
                  || (micro == 2 && nano < 1)))))
    rank = GST_RANK_PRIMARY + 1;

  return gst_element_register (plugin, "dav1ddec", rank, GST_TYPE_DAV1D_DEC);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    dav1d,
    "dav1d AV1 decoder",
    plugin_init, PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME,
    GST_PACKAGE_ORIGIN)
