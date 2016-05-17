#include "sysdeps.h"
#include "gstmfxvideocontext.h"
#include "gstmfxpluginutil.h"
#include "gstmfxpluginbase.h"


gboolean
gst_mfx_ensure_aggregator(GstElement * element)
{
	GstMfxPluginBase *const plugin = GST_MFX_PLUGIN_BASE(element);
	GstMfxTaskAggregator *aggregator;

	g_return_val_if_fail(GST_IS_ELEMENT(element), FALSE);

	if (gst_mfx_video_context_prepare(element, &plugin->aggregator))
		return TRUE;

	aggregator = gst_mfx_task_aggregator_new();
    if (!aggregator)
        return FALSE;

	gst_mfx_video_context_propagate(element, aggregator);
	gst_mfx_task_aggregator_unref(aggregator);
	return TRUE;
}

gboolean
gst_mfx_handle_context_query (GstQuery * query, GstMfxTaskAggregator * task)
{
    const gchar *type = NULL;
    GstContext *context, *old_context;

    g_return_val_if_fail (query != NULL, FALSE);

    if (!task)
        return FALSE;

    if (!gst_query_parse_context_type (query, &type))
        return FALSE;

    if (g_strcmp0 (type, GST_MFX_AGGREGATOR_CONTEXT_TYPE_NAME))
        return FALSE;

    gst_query_parse_context (query, &old_context);
    if (old_context) {
        context = gst_context_copy (old_context);
        gst_mfx_video_context_set_aggregator (context, task);
    } else {
        context = gst_mfx_video_context_new_with_aggregator (task, FALSE);
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
gst_mfx_video_format_new_template_caps_with_features(GstVideoFormat format,
	const gchar * features_string)
{
	GstCaps *caps;

	GstCapsFeatures *const features =
		gst_caps_features_new(features_string, NULL);

	if (!features)
		return NULL;

	caps = gst_mfx_video_format_new_template_caps(format);
	if (!caps)
		return NULL;

	gst_caps_set_features(caps, 0, features);
	return caps;
}

GstMfxCapsFeature
gst_mfx_find_preferred_caps_feature(GstPad * pad, GstVideoFormat * out_format_ptr)
{
	GstMfxCapsFeature feature = GST_MFX_CAPS_FEATURE_SYSTEM_MEMORY;
	guint i, num_structures;
	GstCaps *caps = NULL;
	GstCaps *sysmem_caps = NULL;
	GstCaps *mfx_caps = NULL;
	GstCaps *out_caps, *temp1;
	GstVideoFormat out_format;
	GstStructure *structure;
	gchar *format = NULL;

    temp1 = gst_pad_get_pad_template_caps(pad);
    out_caps = gst_pad_peer_query_caps(pad, temp1);
    gst_caps_unref(temp1);
	if (!out_caps) {
		feature = GST_MFX_CAPS_FEATURE_NOT_NEGOTIATED;
		goto cleanup;
	}

    num_structures = gst_caps_get_size(out_caps);
    structure = gst_structure_copy(
        gst_caps_get_structure(out_caps, num_structures - 1));
    if (gst_structure_has_field(structure, "format"))
        gst_structure_fixate_field(structure, "format");
    format = gst_structure_get_string(structure, "format");
    out_format = format ? gst_video_format_from_string(format) :
        GST_VIDEO_FORMAT_NV12;
    if (structure)
        gst_structure_free(structure);

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

	for (i = 0; i < num_structures; i++) {
		GstCapsFeatures *const features = gst_caps_get_features(out_caps, i);
		structure = gst_caps_get_structure(out_caps, i);

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
		else if (gst_caps_can_intersect(caps, sysmem_caps) &&
			feature < GST_MFX_CAPS_FEATURE_SYSTEM_MEMORY)
			feature = GST_MFX_CAPS_FEATURE_SYSTEM_MEMORY;
		gst_caps_replace(&caps, NULL);

		/* Stop at the first match, the caps should already be sorted out
		by preference order from downstream elements */
		if (feature != GST_MFX_CAPS_FEATURE_SYSTEM_MEMORY)
			break;
	}

	*out_format_ptr = out_format;

cleanup:
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

/* Checks whether the supplied caps contain MFX surfaces */
gboolean
gst_caps_has_mfx_surface(GstCaps * caps)
{
	g_return_val_if_fail(caps != NULL, FALSE);

	return _gst_caps_has_feature(caps, GST_CAPS_FEATURE_MEMORY_MFX_SURFACE);
}

gboolean
gst_mfx_query_peer_has_raw_caps(GstPad * pad)
{
    GstPad *other_pad = NULL;
    GstElement *element = NULL;
    //gchar *element_name = NULL;
    GstCaps *caps = NULL;
    gboolean mapped = FALSE;

    gst_object_ref (pad);

    for (;;) {
        other_pad = gst_pad_get_peer (pad);
        gst_object_unref (pad);
        if (!other_pad)
            break;

        element = gst_pad_get_parent_element (other_pad);
        if (!element)
            break;

        caps = gst_pad_get_allowed_caps(other_pad);
        gst_object_unref (other_pad);

        //g_print("%s\n\n", gst_caps_to_string(caps));

        if (!gst_caps_has_mfx_surface(caps)) {
            mapped = TRUE;
            goto cleanup;
        }
        gst_caps_replace(&caps, NULL);

        //element_name = gst_element_get_name (element);
        //g_print("%s\n", element_name);

        pad = gst_element_get_static_pad (element, "src");
        if (!pad)
            break;


        //g_free (element_name);
        //element_name = NULL;
        g_clear_object (&element);
    }

cleanup:
    gst_caps_replace(&caps, NULL);
    //g_free (element_name);
    g_clear_object (&element);

    return mapped;
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
