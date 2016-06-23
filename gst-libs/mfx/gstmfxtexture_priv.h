/*
 *  Copyright (C) 2012-2013 Intel Corporation
 *    Author: Gwenole Beauchesne <gwenole.beauchesne@intel.com>
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

#ifndef GST_MFX_TEXTURE_PRIV_H
#define GST_MFX_TEXTURE_PRIV_H

#include "gstmfxobject_priv.h"

G_BEGIN_DECLS

#define GST_MFX_TEXTURE_CLASS(klass) \
	((GstMfxTextureClass *)(klass))

#define GST_MFX_TEXTURE_GET_CLASS(obj) \
	GST_MFX_TEXTURE_CLASS (GST_MFX_OBJECT_GET_CLASS (obj))


/**
* GST_MFX_TEXTURE_ID:
* @texture: a #GstMfxTexture
*
* Macro that evaluates to the GL texture id associated with the @texture
*/
#undef  GST_MFX_TEXTURE_ID
#define GST_MFX_TEXTURE_ID(texture) \
	(GST_MFX_OBJECT_ID (texture))

/**
* GST_MFX_TEXTURE_TARGET:
* @texture: a #GstMfxTexture
*
* Macro that evaluates to the GL texture target associated with the @texture
*/
#undef  GST_MFX_TEXTURE_TARGET
#define GST_MFX_TEXTURE_TARGET(texture) \
	(GST_MFX_TEXTURE (texture)->gl_target)

/**
* GST_MFX_TEXTURE_FORMAT:
* @texture: a #GstMfxTexture
*
* Macro that evaluates to the GL texture format associated with the @texture
*/
#undef  GST_MFX_TEXTURE_FORMAT
#define GST_MFX_TEXTURE_FORMAT(texture) \
	(GST_MFX_TEXTURE (texture)->gl_format)

/**
* GST_MFX_TEXTURE_WIDTH:
* @texture: a #GstMfxTexture
*
* Macro that evaluates to the GL texture width associated with the @texture
*/
#undef  GST_MFX_TEXTURE_WIDTH
#define GST_MFX_TEXTURE_WIDTH(texture) \
	(GST_MFX_TEXTURE (texture)->width)

/**
* GST_MFX_TEXTURE_HEIGHT:
* @texture: a #GstMfxTexture
*
* Macro that evaluates to the GL texture height associated with the @texture
*/
#undef  GST_MFX_TEXTURE_HEIGHT
#define GST_MFX_TEXTURE_HEIGHT(texture) \
	(GST_MFX_TEXTURE (texture)->height)

#define GST_MFX_TEXTURE_FLAGS         GST_MFX_MINI_OBJECT_FLAGS
#define GST_MFX_TEXTURE_FLAG_IS_SET   GST_MFX_MINI_OBJECT_FLAG_IS_SET
#define GST_MFX_TEXTURE_FLAG_SET      GST_MFX_MINI_OBJECT_FLAG_SET
#define GST_MFX_TEXTURE_FLAG_UNSET    GST_MFX_MINI_OBJECT_FLAG_UNSET

/* GstMfxTextureClass hooks */
typedef gboolean(*GstMfxTextureAllocateFunc) (GstMfxTexture * texture);
typedef gboolean(*GstMfxTexturePutSurfaceFunc) (GstMfxTexture * texture,
	GstMfxSurfaceProxy * proxy);

typedef struct _GstMfxTextureClass GstMfxTextureClass;

/**
* GstMfxTexture:
*
* Base class for API-dependent textures.
*/
struct _GstMfxTexture {
	/*< private >*/
	GstMfxObject parent_instance;

	/*< protected >*/
	guint gl_target;
	guint gl_format;
	guint width;
	guint height;
	guint is_wrapped : 1;
};

/**
* GstMfxTextureClass:
* @put_surface: virtual function to render a #GstMfxSurfaceProxy into a texture
*
* Base class for API-dependent textures.
*/
struct _GstMfxTextureClass {
	/*< private >*/
	GstMfxObjectClass parent_class;

	/*< protected >*/
	GstMfxTextureAllocateFunc allocate;
	GstMfxTexturePutSurfaceFunc put_surface;
};

GstMfxTexture *
gst_mfx_texture_new_internal(const GstMfxTextureClass * klass,
	GstMfxDisplay * display, GstMfxID id, guint target, guint format,
	guint width, guint height);

#define gst_mfx_texture_ref_internal(texture) \
	((gpointer)gst_mfx_mini_object_ref (GST_MFX_MINI_OBJECT (texture)))

#define gst_mfx_texture_unref_internal(texture) \
	gst_mfx_mini_object_unref (GST_MFX_MINI_OBJECT (texture))

#define gst_mfx_texture_replace_internal(old_texture_ptr, new_texture) \
	gst_mfx_mini_object_replace ((GstMfxMiniObject **)(old_texture_ptr), \
	GST_MFX_MINI_OBJECT (new_texture))

#undef  gst_mfx_texture_ref
#define gst_mfx_texture_ref(texture) \
	gst_mfx_texture_ref_internal ((texture))

#undef  gst_mfx_texture_unref
#define gst_mfx_texture_unref(texture) \
	gst_mfx_texture_unref_internal ((texture))

#undef  gst_mfx_texture_replace
#define gst_mfx_texture_replace(old_texture_ptr, new_texture) \
	gst_mfx_texture_replace_internal ((old_texture_ptr), (new_texture))

#endif /* GST_MFX_TEXTURE_PRIV_H */
