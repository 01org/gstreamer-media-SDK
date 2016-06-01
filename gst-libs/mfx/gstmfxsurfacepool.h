/*
 *  Copyright (C) 2016 Intel Corporation
 *    Author: Ishmael Visayana Sameen <ishmael.visayana.sameen@intel.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301 USA
 */

#ifndef GST_MFX_SURFACE_POOL_H
#define GST_MFX_SURFACE_POOL_H

#include "gstmfxsurfaceproxy.h"
#include "gstmfxtask.h"
#include <glib.h>

G_BEGIN_DECLS

#define GST_MFX_SURFACE_POOL(obj) \
  ((GstMfxSurfacePool *)(obj))

GstMfxSurfacePool *
gst_mfx_surface_pool_new (GstMfxDisplay * display, GstVideoInfo * info,
    gboolean mapped);

GstMfxSurfacePool *
gst_mfx_surface_pool_new_with_task (GstMfxTask * task);

GstMfxSurfacePool *
gst_mfx_surface_pool_ref (GstMfxSurfacePool * pool);

void
gst_mfx_surface_pool_unref (GstMfxSurfacePool * pool);

void
gst_mfx_surface_pool_replace (GstMfxSurfacePool ** old_pool_ptr,
    GstMfxSurfacePool * new_pool);

GstMfxSurfaceProxy *
gst_mfx_surface_pool_get_surface (GstMfxSurfacePool * pool);

void
gst_mfx_surface_pool_put_surface (GstMfxSurfacePool * pool,
	GstMfxSurfaceProxy * surface);

guint
gst_mfx_surface_pool_get_size (GstMfxSurfacePool * pool);

guint
gst_mfx_surface_pool_get_capacity (GstMfxSurfacePool * pool);

void
gst_mfx_surface_pool_set_capacity (GstMfxSurfacePool * pool, guint capacity);

GstMfxSurfaceProxy *
gst_mfx_surface_pool_find_proxy(GstMfxSurfacePool * pool,
    mfxFrameSurface1 * surface);

G_END_DECLS

#endif /* GST_MFX_SURFACE_POOL_H */
