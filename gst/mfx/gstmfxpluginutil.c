#include "sysdeps.h"
#include "gstmfxvideocontext.h"
#if USE_DRM
# include "gstmfxdisplay_drm.h"
#endif
#if USE_X11
# include "gstmfxdisplay_x11.h"
#endif
#if USE_GLX
# include "gstmfxdisplay_glx.h"
#endif
#if USE_EGL
# include "gstmfxdisplay_egl.h"
#endif
#if USE_WAYLAND
# include "gstmfxdisplay_wayland.h"
#endif
#include "gstmfxpluginutil.h"
#include "gstmfxpluginbase.h"

typedef GstMfxDisplay *(*GstMfxDisplayCreateFunc) (const gchar *);
typedef GstMfxDisplay *(*GstMfxDisplayCreateFromHandleFunc) (gpointer);

typedef struct
{
	const gchar *type_str;
	GstMfxDisplayType type;
	GstMfxDisplayCreateFunc create_display;
	GstMfxDisplayCreateFromHandleFunc create_display_from_handle;
} DisplayMap;

/* *INDENT-OFF* */
static const DisplayMap g_display_map[] = {
#if USE_WAYLAND
	{ "wayland",
	GST_MFX_DISPLAY_TYPE_WAYLAND,
	gst_mfx_display_wayland_new,
	(GstMfxDisplayCreateFromHandleFunc)
	gst_mfx_display_wayland_new_with_display },
#endif
#if USE_GLX
	{ "glx",
	GST_MFX_DISPLAY_TYPE_GLX,
	gst_mfx_display_glx_new,
	(GstMfxDisplayCreateFromHandleFunc)
	gst_mfx_display_glx_new_with_display },
#endif
#if USE_X11
	{ "x11",
	GST_MFX_DISPLAY_TYPE_X11,
	gst_mfx_display_x11_new,
	(GstMfxDisplayCreateFromHandleFunc)
	gst_mfx_display_x11_new_with_display },
#endif
#if USE_DRM
	{ "drm",
	GST_MFX_DISPLAY_TYPE_DRM,
	gst_mfx_display_drm_new },
#endif
	{ NULL, }
};
/* *INDENT-ON* */

static GstMfxDisplay *
gst_mfx_create_display(GstMfxDisplayType display_type,
	const gchar * display_name)
{
	GstMfxDisplay *display = NULL;
	const DisplayMap *m;

	for (m = g_display_map; m->type_str != NULL; m++) {
		if (display_type != GST_MFX_DISPLAY_TYPE_ANY && display_type != m->type)
			continue;

		display = m->create_display(display_name);
		if (display || display_type != GST_MFX_DISPLAY_TYPE_ANY)
			break;
	}
	return display;
}

static GstMfxDisplay *
gst_mfx_create_display_from_handle(GstMfxDisplayType display_type,
	gpointer handle)
{
	GstMfxDisplay *display;
	const DisplayMap *m;

	if (display_type == GST_MFX_DISPLAY_TYPE_ANY)
		return NULL;

	for (m = g_display_map; m->type_str != NULL; m++) {
		if (m->type == display_type) {
			display = m->create_display_from_handle ?
				m->create_display_from_handle(handle) : NULL;
			return display;
		}
	}
	return NULL;
}

static GstMfxDisplay *
gst_mfx_create_display_from_gl_context(GstObject * gl_context_object)
{
#if USE_GST_GL_HELPERS
	GstGLContext *const gl_context = GST_GL_CONTEXT(gl_context_object);
	GstGLDisplay *const gl_display = gst_gl_context_get_display(gl_context);
	gpointer native_display =
		GSIZE_TO_POINTER(gst_gl_display_get_handle(gl_display));
	GstMfxDisplay *display, *out_display;
	GstMfxDisplayType display_type;

	switch (gst_gl_display_get_handle_type(gl_display)) {
#if USE_X11
	case GST_GL_DISPLAY_TYPE_X11:
		display_type = GST_MFX_DISPLAY_TYPE_X11;
		break;
#endif
#if USE_WAYLAND
	case GST_GL_DISPLAY_TYPE_WAYLAND:
		display_type = GST_MFX_DISPLAY_TYPE_WAYLAND;
		break;
#endif
	case GST_GL_DISPLAY_TYPE_ANY:{
		/* Derive from the active window */
		GstGLWindow *const gl_window = gst_gl_context_get_window(gl_context);
		const gchar *const gl_window_type = g_getenv("GST_GL_WINDOW");

		display_type = GST_MFX_DISPLAY_TYPE_ANY;
		if (!gl_window)
			break;
		native_display = GSIZE_TO_POINTER(gst_gl_window_get_display(gl_window));

		if (gl_window_type) {
#if USE_X11
			if (!display_type && g_strcmp0(gl_window_type, "x11") == 0)
				display_type = GST_MFX_DISPLAY_TYPE_X11;
#endif
#if USE_WAYLAND
			if (!display_type && g_strcmp0(gl_window_type, "wayland") == 0)
				display_type = GST_MFX_DISPLAY_TYPE_WAYLAND;
#endif
		}
		else {
#if USE_X11
			if (!display_type && GST_GL_HAVE_WINDOW_X11)
				display_type = GST_MFX_DISPLAY_TYPE_X11;
#endif
#if USE_WAYLAND
			if (!display_type && GST_GL_HAVE_WINDOW_WAYLAND)
				display_type = GST_MFX_DISPLAY_TYPE_WAYLAND;
#endif
		}
		break;
	}
	default:
		display_type = GST_MFX_DISPLAY_TYPE_ANY;
		break;
	}
	if (!display_type)
		return NULL;

	display = gst_mfx_create_display_from_handle(display_type, native_display);
	if (!display)
		return NULL;

	switch (gst_gl_context_get_gl_platform(gl_context)) {
#if USE_EGL
	case GST_GL_PLATFORM_EGL:{
		guint gles_version;

		switch (gst_gl_context_get_gl_api(gl_context)) {
		case GST_GL_API_GLES1:
			gles_version = 1;
			goto create_egl_display;
		case GST_GL_API_GLES2:
			gles_version = 2;
			goto create_egl_display;
		case GST_GL_API_OPENGL:
		case GST_GL_API_OPENGL3:
			gles_version = 0;
		create_egl_display:
			out_display = gst_mfx_display_egl_new(display, gles_version);
			break;
		default:
			out_display = NULL;
			break;
		}
		if (!out_display)
			return NULL;
		gst_mfx_display_egl_set_gl_context(GST_MFX_DISPLAY_EGL(out_display),
			GSIZE_TO_POINTER(gst_gl_context_get_gl_context(gl_context)));
		break;
	}
#endif
	default:
		out_display = gst_mfx_display_ref(display);
		break;
	}
	gst_mfx_display_unref(display);
	return out_display;
#endif
	GST_ERROR("unsupported GStreamer version %s", GST_API_VERSION_S);
	return NULL;
}

gboolean
gst_mfx_ensure_display(GstElement * element, GstMfxDisplayType type)
{
	GstMfxPluginBase *const plugin = GST_MFX_PLUGIN_BASE(element);
	GstMfxDisplay *display;

	g_return_val_if_fail(GST_IS_ELEMENT(element), FALSE);

	if (gst_mfx_video_context_prepare(element, &plugin->display)) {
		/* Neighbour found and it updated the display */
		if (gst_mfx_plugin_base_has_display_type(plugin, type))
			return TRUE;
	}

	/* If no neighboor, or application not interested, use system default */
	if (plugin->gl_context)
		display = gst_mfx_create_display_from_gl_context(plugin->gl_context);
	else
		display = gst_mfx_create_display(type, plugin->display_name);
	if (!display)
		return FALSE;

	gst_mfx_video_context_propagate(element, display);
	gst_mfx_display_unref(display);
	return TRUE;
}

gboolean
gst_mfx_handle_context_query (GstQuery * query, GstMfxDisplay * display)
{
    const gchar *type = NULL;
    GstContext *context, *old_context;

    g_return_val_if_fail (query != NULL, FALSE);

    if (!display)
        return FALSE;

    if (!gst_query_parse_context_type (query, &type))
        return FALSE;

    if (g_strcmp0 (type, GST_MFX_DISPLAY_CONTEXT_TYPE_NAME))
        return FALSE;

    gst_query_parse_context (query, &old_context);
    if (old_context) {
        context = gst_context_copy (old_context);
        gst_mfx_video_context_set_display (context, display);
    } else {
        context = gst_mfx_video_context_new_with_display (display, FALSE);
    }

    gst_query_set_context (query, context);
    gst_context_unref (context);

    return TRUE;
}


gboolean
gst_mfx_append_surface_caps(GstCaps * out_caps, GstCaps * in_caps)
{
	GstStructure *structure;
	const GValue *v_width, *v_height, *v_framerate, *v_par;
	guint i, n_structures;

	structure = gst_caps_get_structure(in_caps, 0);
	v_width = gst_structure_get_value(structure, "width");
	v_height = gst_structure_get_value(structure, "height");
	v_framerate = gst_structure_get_value(structure, "framerate");
	v_par = gst_structure_get_value(structure, "pixel-aspect-ratio");
	if (!v_width || !v_height)
		return FALSE;

	n_structures = gst_caps_get_size(out_caps);
	for (i = 0; i < n_structures; i++) {
		structure = gst_caps_get_structure(out_caps, i);
		gst_structure_set_value(structure, "width", v_width);
		gst_structure_set_value(structure, "height", v_height);
		if (v_framerate)
			gst_structure_set_value(structure, "framerate", v_framerate);
		if (v_par)
			gst_structure_set_value(structure, "pixel-aspect-ratio", v_par);
	}
	return TRUE;
}

gboolean
gst_mfx_value_set_format(GValue * value, GstVideoFormat format)
{
	const gchar *str;

	str = gst_video_format_to_string(format);
	if (!str)
		return FALSE;

	g_value_init(value, G_TYPE_STRING);
	g_value_set_string(value, str);
	return TRUE;
}

gboolean
gst_mfx_value_set_format_list(GValue * value, GArray * formats)
{
	GValue v_format = G_VALUE_INIT;
	guint i;

	g_value_init(value, GST_TYPE_LIST);
	for (i = 0; i < formats->len; i++) {
		GstVideoFormat const format = g_array_index(formats, GstVideoFormat, i);

		if (!gst_mfx_value_set_format(&v_format, format))
			continue;
		gst_value_list_append_value(value, &v_format);
		g_value_unset(&v_format);
	}
	return TRUE;
}

void
set_video_template_caps(GstCaps * caps)
{
	GstStructure *const structure = gst_caps_get_structure(caps, 0);

	gst_structure_set(structure,
		"width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
		"height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
		"framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1,
		"pixel-aspect-ratio", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
}

GstCaps *
gst_mfx_video_format_new_template_caps(GstVideoFormat format)
{
	GstCaps *caps;

	g_return_val_if_fail(format != GST_VIDEO_FORMAT_UNKNOWN, NULL);

	caps = gst_caps_new_empty_simple("video/x-raw");
	if (!caps)
		return NULL;

	gst_caps_set_simple(caps,
		"format", G_TYPE_STRING, gst_video_format_to_string(format), NULL);
	set_video_template_caps(caps);

	return caps;
}

GstCaps *
gst_mfx_video_format_new_template_caps_from_list(GArray * formats)
{
	GValue v_formats = G_VALUE_INIT;
	GstCaps *caps;

	caps = gst_caps_new_empty_simple("video/x-raw");
	if (!caps)
		return NULL;

	if (!gst_mfx_value_set_format_list(&v_formats, formats)) {
		gst_caps_unref(caps);
		return NULL;
	}

	gst_caps_set_value(caps, "format", &v_formats);
	set_video_template_caps(caps);
	g_value_unset(&v_formats);
	return caps;
}

GstCaps *
gst_mfx_video_format_new_template_caps_with_features(GstVideoFormat format,
	const gchar * features_string)
{
	GstCaps *caps;

	GstCapsFeatures *const features =
		gst_caps_features_new(features_string, NULL);

	if (!features) {
		//gst_caps_unref(caps);
		return NULL;
	}

	caps = gst_mfx_video_format_new_template_caps(format);
	if (!caps)
		return NULL;

	gst_caps_set_features(caps, 0, features);
	return caps;
}

static GstCaps *
new_gl_texture_upload_meta_caps(void)
{
	return
		gst_caps_from_string(GST_VIDEO_CAPS_MAKE_WITH_FEATURES
		(GST_CAPS_FEATURE_META_GST_VIDEO_GL_TEXTURE_UPLOAD_META,
		"{ RGBA, BGRA }"));
}

GstMfxCapsFeature
gst_mfx_find_preferred_caps_feature(GstPad * pad, GstVideoFormat format,
	GstVideoFormat * out_format_ptr)
{
	GstMfxCapsFeature feature = GST_MFX_CAPS_FEATURE_SYSTEM_MEMORY;
	guint i, num_structures;
	GstCaps *caps = NULL;
	GstCaps *gl_texture_upload_caps = NULL;
	GstCaps *sysmem_caps = NULL;
	GstCaps *mfx_caps = NULL;
	GstCaps *out_caps, *templ;
	GstVideoFormat out_format;

	templ = gst_pad_get_pad_template_caps(pad);
	out_caps = gst_pad_peer_query_caps(pad, templ);
	gst_caps_unref(templ);
	if (!out_caps) {
		feature = GST_MFX_CAPS_FEATURE_NOT_NEGOTIATED;
		goto cleanup;
	}

	out_format = format == GST_VIDEO_FORMAT_ENCODED ?
	GST_VIDEO_FORMAT_NV12 : format;

	gl_texture_upload_caps = new_gl_texture_upload_meta_caps();
	if (!gl_texture_upload_caps)
		goto cleanup;

	mfx_caps =
		gst_mfx_video_format_new_template_caps_with_features(out_format,
		GST_CAPS_FEATURE_MEMORY_MFX_SURFACE);
	if (!mfx_caps)
		goto cleanup;

	sysmem_caps =
		gst_mfx_video_format_new_template_caps_with_features(out_format,
		GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY);
	if (!sysmem_caps)
		goto cleanup;

	num_structures = gst_caps_get_size(out_caps);
	for (i = 0; i < num_structures; i++) {
		GstCapsFeatures *const features = gst_caps_get_features(out_caps, i);
		GstStructure *const structure = gst_caps_get_structure(out_caps, i);

		/* Skip ANY features, we need an exact match for correct evaluation */
		if (gst_caps_features_is_any(features))
			continue;

		caps = gst_caps_new_full(gst_structure_copy(structure), NULL);
		if (!caps)
			continue;
		gst_caps_set_features(caps, 0, gst_caps_features_copy(features));

		if (gst_caps_can_intersect(caps, mfx_caps) &&
			feature < GST_MFX_CAPS_FEATURE_MFX_SURFACE)
			feature = GST_MFX_CAPS_FEATURE_MFX_SURFACE;
		else if (gst_caps_can_intersect(caps, gl_texture_upload_caps) &&
			feature < GST_MFX_CAPS_FEATURE_GL_TEXTURE_UPLOAD_META)
			feature = GST_MFX_CAPS_FEATURE_GL_TEXTURE_UPLOAD_META;
		else if (gst_caps_can_intersect(caps, sysmem_caps) &&
			feature < GST_MFX_CAPS_FEATURE_SYSTEM_MEMORY)
			feature = GST_MFX_CAPS_FEATURE_SYSTEM_MEMORY;
		gst_caps_replace(&caps, NULL);

		/* Stop at the first match, the caps should already be sorted out
		by preference order from downstream elements */
		if (feature != GST_MFX_CAPS_FEATURE_SYSTEM_MEMORY)
			break;
	}

	if (out_format_ptr) {
		if (feature == GST_MFX_CAPS_FEATURE_GL_TEXTURE_UPLOAD_META) {
			GstStructure *structure;
			gchar *format_str;
			out_format = GST_VIDEO_FORMAT_UNKNOWN;
			do {
				caps = gst_caps_intersect_full(out_caps, gl_texture_upload_caps,
					GST_CAPS_INTERSECT_FIRST);
				if (!caps)
					break;
				structure = gst_caps_get_structure(caps, 0);
				if (!structure)
					break;
				if (!gst_structure_get(structure, "format", G_TYPE_STRING,
					&format_str, NULL))
					break;
				out_format = gst_video_format_from_string(format_str);
				g_free(format_str);
			} while (0);
			if (!out_format)
				goto cleanup;
		}
		*out_format_ptr = out_format;
	}

cleanup:
	gst_caps_replace(&gl_texture_upload_caps, NULL);
	gst_caps_replace(&sysmem_caps, NULL);
	gst_caps_replace(&mfx_caps, NULL);
	gst_caps_replace(&caps, NULL);
	gst_caps_replace(&out_caps, NULL);
	return feature;
}

const gchar *
gst_mfx_caps_feature_to_string(GstMfxCapsFeature feature)
{
	const gchar *str;

	switch (feature) {
	case GST_MFX_CAPS_FEATURE_SYSTEM_MEMORY:
		str = GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY;
		break;
	case GST_MFX_CAPS_FEATURE_GL_TEXTURE_UPLOAD_META:
		str = GST_CAPS_FEATURE_META_GST_VIDEO_GL_TEXTURE_UPLOAD_META;
		break;
	case GST_MFX_CAPS_FEATURE_MFX_SURFACE:
		str = GST_CAPS_FEATURE_MEMORY_MFX_SURFACE;
		break;
	default:
		str = NULL;
		break;
	}
	return str;
}

static gboolean
_gst_caps_has_feature(const GstCaps * caps, const gchar * feature)
{
	guint i;

	for (i = 0; i < gst_caps_get_size(caps); i++) {
		GstCapsFeatures *const features = gst_caps_get_features(caps, i);
		/* Skip ANY features, we need an exact match for correct evaluation */
		if (gst_caps_features_is_any(features))
			continue;
		if (gst_caps_features_contains(features, feature))
			return TRUE;
	}

	return FALSE;
}

gboolean
gst_mfx_caps_feature_contains(const GstCaps * caps, GstMfxCapsFeature feature)
{
	const gchar *feature_str;

	g_return_val_if_fail(caps != NULL, FALSE);

	feature_str = gst_mfx_caps_feature_to_string(feature);
	if (!feature_str)
		return FALSE;

	return _gst_caps_has_feature(caps, feature_str);
}

/* Checks whether the supplied caps contain VA surfaces */
gboolean
gst_caps_has_mfx_surface(GstCaps * caps)
{
	g_return_val_if_fail(caps != NULL, FALSE);

	return _gst_caps_has_feature(caps, GST_CAPS_FEATURE_MEMORY_MFX_SURFACE);
}

void
gst_video_info_change_format(GstVideoInfo * vip, GstVideoFormat format,
guint width, guint height)
{
	GstVideoInfo vi = *vip;

	gst_video_info_set_format(vip, format, width, height);

	vip->interlace_mode = vi.interlace_mode;
	vip->flags = vi.flags;
	vip->views = vi.views;
	vip->par_n = vi.par_n;
	vip->par_d = vi.par_d;
	vip->fps_n = vi.fps_n;
	vip->fps_d = vi.fps_d;
}
