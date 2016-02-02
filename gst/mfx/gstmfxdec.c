/*
 ============================================================================
 Name        : gst-mfx-dec.c
 Author      : Ishmael Sameen <ishmael.visayana.sameen@intel.com>
 Version     : 0.0.1
 Copyright   : Copyright (C) 2015
 Description :
 ============================================================================
 */

#include "gstmfxdec.h"

#include <string.h>

#include "gstmfxsurfaceproxy.h"
#include "gstmfxsurfaceproxy_priv.h"
#include "gstmfxcodecmap.h"
#include "gstmfxvideomemory.h"
#include "gstmfxvideometa.h"
#include "gstmfxvideobufferpool.h"
#include "gstmfxpluginutil.h"

GST_DEBUG_CATEGORY_STATIC(mfxdec_debug);
#define GST_CAT_DEFAULT (mfxdec_debug)

#define DEFAULT_ASYNC_DEPTH 4

/* Default templates */
#define GST_CAPS_CODEC(CODEC) CODEC "; "

/* *INDENT-OFF* */
static const char gst_mfxdecode_sink_caps_str[] =
	GST_CAPS_CODEC("video/mpeg, mpegversion=2, systemstream=(boolean)false")
	GST_CAPS_CODEC("video/x-h264, stream-format = (string) { byte-stream }, alignment = (string) { au, nal }")
	GST_CAPS_CODEC("video/x-wmv")
	;

static const char gst_mfxdecode_src_caps_str[] =
	GST_MFX_MAKE_SURFACE_CAPS ";"
	GST_VIDEO_CAPS_MAKE("{ NV12 }");

enum
{
	PROP_0,
	PROP_ASYNC_DEPTH
};

static GstStaticPadTemplate sink_template_factory =
GST_STATIC_PAD_TEMPLATE("sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS(gst_mfxdecode_sink_caps_str)
);

static GstStaticPadTemplate src_template_factory =
	GST_STATIC_PAD_TEMPLATE("src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	//GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE("NV12"))
	GST_STATIC_CAPS(gst_mfxdecode_src_caps_str)
);

G_DEFINE_TYPE_WITH_CODE(
    GstMfxDec,
    gst_mfxdec,
    GST_TYPE_VIDEO_DECODER,
    GST_MFX_PLUGIN_BASE_INIT_INTERFACES);

static gboolean
gst_mfxdec_update_sink_caps(GstMfxDec * decode, GstCaps * caps);
static gboolean gst_mfxdec_update_src_caps(GstMfxDec * decode);

static gboolean
gst_mfxdec_input_state_replace(GstMfxDec * decode,
	const GstVideoCodecState * new_state);

static void gst_mfx_dec_set_property(GObject * object, guint prop_id,
	const GValue * value, GParamSpec * pspec);
static void gst_mfx_dec_get_property(GObject * object, guint prop_id,
	GValue * value, GParamSpec * pspec);

/* GstVideoDecoder base class method */
static gboolean gst_mfxdec_open(GstVideoDecoder * decoder);
static gboolean gst_mfxdec_close(GstVideoDecoder * decoder);
static gboolean gst_mfxdec_set_format(GstVideoDecoder * decoder,
	GstVideoCodecState * state);
static gboolean gst_mfxdec_flush(GstVideoDecoder * decoder);
static GstFlowReturn gst_mfxdec_handle_frame(GstVideoDecoder * decoder,
	GstVideoCodecFrame * frame);
static gboolean gst_mfxdec_decide_allocation(GstVideoDecoder * decoder,
	GstQuery * query);



static void
gst_mfx_decoder_state_changed(GstMfxDecoder * decoder,
	const GstVideoCodecState * codec_state, gpointer user_data)
{
	GstMfxDec *const decode = GST_MFXDEC(user_data);

	g_assert(decode->decoder == decoder);

	if (!gst_mfxdec_input_state_replace(decode, codec_state))
		return;
	if (!gst_mfxdec_update_sink_caps(decode, decode->input_state->caps))
		return;

	decode->do_renego = TRUE;
}

static GstVideoCodecState *
copy_video_codec_state(const GstVideoCodecState * in_state)
{
	GstVideoCodecState *state;

	g_return_val_if_fail(in_state != NULL, NULL);

	state = g_slice_new0(GstVideoCodecState);
	state->ref_count = 1;
	state->info = in_state->info;
	state->caps = gst_caps_copy(in_state->caps);
	if (in_state->codec_data)
		state->codec_data = gst_buffer_copy_deep(in_state->codec_data);

	return state;
}

static gboolean
gst_mfxdec_input_state_replace(GstMfxDec * decode,
	const GstVideoCodecState * new_state)
{
	if (decode->input_state) {
		if (new_state) {
			const GstCaps *curcaps = decode->input_state->caps;
			/* If existing caps are equal of the new state, keep the
			* existing state without renegotiating. */
			if (gst_caps_is_strictly_equal(curcaps, new_state->caps)) {
				GST_DEBUG("Ignoring new caps %" GST_PTR_FORMAT
					" since are equal to current ones", new_state->caps);
				return FALSE;
			}
		}
		gst_video_codec_state_unref(decode->input_state);
	}

	if (new_state)
		decode->input_state = copy_video_codec_state(new_state);
	else
		decode->input_state = NULL;

	return TRUE;
}

static inline gboolean
gst_mfxdec_update_sink_caps(GstMfxDec * decode, GstCaps * caps)
{
	GST_INFO_OBJECT(decode, "new sink caps = %" GST_PTR_FORMAT, caps);
	gst_caps_replace(&decode->sinkpad_caps, caps);
	return TRUE;
}

static gboolean
gst_mfxdec_update_src_caps(GstMfxDec * decode)
{
	GstVideoDecoder *const vdec = GST_VIDEO_DECODER(decode);
	GstVideoCodecState *state, *ref_state;
	GstVideoInfo *vi;
	GstVideoFormat format = GST_VIDEO_FORMAT_NV12;

	if (!decode->input_state)
		return FALSE;

	ref_state = decode->input_state;

	GstCapsFeatures *features = NULL;
	GstMfxCapsFeature feature;

	feature =
		gst_mfx_find_preferred_caps_feature(GST_VIDEO_DECODER_SRC_PAD(vdec),
		GST_VIDEO_INFO_FORMAT(&ref_state->info), &format);

	if (feature == GST_MFX_CAPS_FEATURE_NOT_NEGOTIATED)
		return FALSE;

	switch (feature) {
	case GST_MFX_CAPS_FEATURE_MFX_SURFACE:
		features =
			gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_MFX_SURFACE, NULL);
		break;
	default:
		break;
	}

	state = gst_video_decoder_set_output_state(vdec, format,
		ref_state->info.width, ref_state->info.height, ref_state);
	if (!state || state->info.width == 0 || state->info.height == 0)
		return FALSE;

	vi = &state->info;

	state->caps = gst_video_info_to_caps(vi);
	if (features)
		gst_caps_set_features(state->caps, 0, features);
	GST_INFO_OBJECT(decode, "new src caps = %" GST_PTR_FORMAT, state->caps);
	gst_caps_replace(&decode->srcpad_caps, state->caps);
	gst_video_codec_state_unref(state);

	return TRUE;
}

static void
gst_mfxdec_release(GstMfxDec * decode)
{
	//g_mutex_lock(&decode->surface_ready_mutex);
	//g_cond_signal(&decode->surface_ready);
	//g_mutex_unlock(&decode->surface_ready_mutex);
	gst_object_unref(decode);
}


static GstFlowReturn
gst_mfxdec_push_decoded_frame(GstMfxDec *decode, GstVideoCodecFrame * frame)
{
	GstFlowReturn ret;
	GstMfxDecoderStatus sts;
	GstMfxSurfaceProxy *proxy;
	GstMfxVideoMeta *meta;
	const GstMfxRectangle *crop_rect;

	sts = gst_mfx_decoder_get_surface_proxy(decode->decoder, &proxy);

	ret = gst_video_decoder_allocate_output_frame(GST_VIDEO_DECODER(decode), frame);

	if (ret != GST_FLOW_OK)
		goto error_create_buffer;

	meta = gst_buffer_get_mfx_video_meta(frame->output_buffer);
	if (!meta)
		goto error_get_meta;
	gst_mfx_video_meta_set_surface_proxy(meta, proxy);
	crop_rect = gst_mfx_surface_proxy_get_crop_rect(proxy);
	if (crop_rect) {
		GstVideoCropMeta *const crop_meta =
			gst_buffer_add_video_crop_meta(frame->output_buffer);
		if (crop_meta) {
			crop_meta->x = crop_rect->x;
			crop_meta->y = crop_rect->y;
			crop_meta->width = crop_rect->width;
			crop_meta->height = crop_rect->height;
		}
	}

	ret = gst_video_decoder_finish_frame(GST_VIDEO_DECODER(decode), frame);
	if (ret != GST_FLOW_OK)
		goto error_commit_buffer;

	return ret;

	/* ERRORS */
error_create_buffer:
	{
		gst_video_decoder_drop_frame(GST_VIDEO_DECODER(decode), frame);
		gst_video_codec_frame_unref(frame);
		return GST_FLOW_ERROR;
	}
error_get_meta:
	{
		gst_video_decoder_drop_frame(GST_VIDEO_DECODER(decode), frame);
		gst_video_codec_frame_unref(frame);
		return GST_FLOW_ERROR;
	}
error_commit_buffer:
	{
		GST_INFO_OBJECT(decode, "downstream element rejected the frame (%s [%d])",
			gst_flow_get_name(ret), ret);
		gst_video_codec_frame_unref(frame);
		return GST_FLOW_ERROR;
	}
}

static gboolean
gst_mfxdec_negotiate(GstMfxDec * decode)
{
	GstVideoDecoder *const vdec = GST_VIDEO_DECODER(decode);
	GstMfxPluginBase *const plugin = GST_MFX_PLUGIN_BASE(vdec);

	if (!decode->do_renego)
		return TRUE;

	GST_DEBUG_OBJECT(decode, "Input codec state changed, doing renegotiation");

	if (!gst_mfx_plugin_base_set_caps(plugin, decode->sinkpad_caps, NULL))
		return FALSE;
	if (!gst_mfxdec_update_src_caps(decode))
		return FALSE;
	if (!gst_video_decoder_negotiate(vdec))
		return FALSE;
	if (!gst_mfx_plugin_base_set_caps(plugin, NULL, decode->srcpad_caps))
		return FALSE;

	decode->do_renego = FALSE;

	return TRUE;
}

static void
gst_mfx_dec_set_property(GObject * object, guint prop_id,
const GValue * value, GParamSpec * pspec)
{
	GstMfxDec *dec;

	g_return_if_fail(GST_IS_MFXDEC(object));
	dec = GST_MFXDEC(object);

	GST_DEBUG_OBJECT(object, "gst_mfx_dec_set_property");
	switch (prop_id) {
	case PROP_ASYNC_DEPTH:
		//dec->async_depth = g_value_get_uint(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
gst_mfx_dec_get_property(GObject * object, guint prop_id, GValue * value,
	GParamSpec * pspec)
{
	GstMfxDec *dec;

	g_return_if_fail(GST_IS_MFXDEC(object));
	dec = GST_MFXDEC(object);

	switch (prop_id) {
	case PROP_ASYNC_DEPTH:
		//g_value_set_uint(value, dec->async_depth);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static gboolean
gst_mfxdec_decide_allocation(GstVideoDecoder * vdec, GstQuery * query)
{
	return gst_mfx_plugin_base_decide_allocation(GST_MFX_PLUGIN_BASE(vdec),
		query);
}

static inline gboolean
gst_mfxdec_ensure_display(GstMfxDec * decode)
{
	return gst_mfx_plugin_base_ensure_display(GST_MFX_PLUGIN_BASE(decode));
}

static gboolean
gst_mfxdec_create(GstMfxDec * mfxdec, GstCaps * caps)
{
	mfxU32 codec_id = gst_get_mfx_codec_from_caps(caps);

	if (!codec_id)
		return FALSE;

	mfxdec->decoder = gst_mfx_decoder_new(GST_MFX_PLUGIN_BASE(mfxdec)->display,
        &GST_MFX_PLUGIN_BASE(mfxdec)->alloc_ctx, codec_id);
	if (!mfxdec->decoder)
		return FALSE;

	return TRUE;
}

static void
gst_mfxdec_purge(GstMfxDec * decode)
{
	GstMfxDecoderStatus status;

	if (!decode->decoder)
		return;

	//status = gst_mfx_decoder_flush(decode->decoder);
	//if (status != GST_MFX_DECODER_STATUS_SUCCESS)
		//GST_INFO_OBJECT(decode, "failed to flush decoder (status %d)", status);

	/* Purge all decoded frames as we don't need them (e.g. flush and close)
	* Releasing the frames is important, otherwise the frames are not
	* freed. */
	/*do {
		GstVideoCodecFrame *frame = NULL;

		status =
			gst_vaapi_decoder_get_frame_with_timeout(decode->decoder, &frame, 0);
		if (frame) {
			gst_video_decoder_release_frame(GST_VIDEO_DECODER(decode), frame);
			gst_video_codec_frame_unref(frame);
		}
	} while (status == GST_MFX_DECODER_STATUS_SUCCESS);*/
}

static void
gst_mfxdec_destroy(GstMfxDec * decode)
{
	gst_mfxdec_purge(decode);

	gst_mfx_decoder_replace(&decode->decoder, NULL);
	gst_caps_replace(&decode->decoder_caps, NULL);

	decode->active = FALSE;

	gst_mfxdec_release(gst_object_ref(decode));
}

static gboolean
gst_mfxdec_reset_full(GstMfxDec * decode, GstCaps * caps,
	gboolean hard)
{
	mfxU32 codec;

	if (!hard && decode->decoder && decode->decoder_caps) {
		if (gst_caps_is_always_compatible(caps, decode->decoder_caps))
			return TRUE;
		codec = gst_get_mfx_codec_from_caps(caps);
		if (codec == gst_mfx_decoder_get_codec(decode->decoder))
			return TRUE;
	}

	gst_mfxdec_destroy(decode);
	return gst_mfxdec_create(decode, caps);
}

static void
gst_mfxdec_finalize(GObject * object)
{
	GstMfxDec *const decode = GST_MFXDEC(object);

	gst_caps_replace(&decode->sinkpad_caps, NULL);
	gst_caps_replace(&decode->srcpad_caps, NULL);
	//gst_caps_replace(&decode->allowed_caps, NULL);

	//g_cond_clear(&decode->surface_ready);
	//g_mutex_clear(&decode->surface_ready_mutex);

	gst_mfx_plugin_base_finalize(GST_MFX_PLUGIN_BASE(object));
	G_OBJECT_CLASS(gst_mfxdec_parent_class)->finalize(object);
}

static gboolean
gst_mfxdec_open(GstVideoDecoder * vdec)
{
	GstMfxDec *const decode = GST_MFXDEC(vdec);
	GstMfxDisplay *const old_display = GST_MFX_PLUGIN_BASE_DISPLAY(decode);
	gboolean success;

	/* Let GstVideoContext ask for a proper display to its neighbours */
	/* Note: steal old display that may be allocated from get_caps()
	so that to retain a reference to it, thus avoiding extra
	initialization steps if we turn out to simply re-use the
	existing (cached) VA display */
	GST_MFX_PLUGIN_BASE_DISPLAY(decode) = NULL;
	success = gst_mfxdec_ensure_display(decode);
	if (success)
        GST_MFX_PLUGIN_BASE(decode)->alloc_ctx.va_dpy =
            GST_MFX_DISPLAY_VADISPLAY(GST_MFX_PLUGIN_BASE(decode)->display);
	if (old_display)
		gst_mfx_display_unref(old_display);
	return success;
}

static gboolean
gst_mfxdec_close(GstVideoDecoder * vdec)
{
	GstMfxDec *const decode = GST_MFXDEC(vdec);

	gst_mfxdec_input_state_replace(decode, NULL);
	gst_mfxdec_destroy(decode);
	gst_mfx_plugin_base_close(GST_MFX_PLUGIN_BASE(decode));
	return TRUE;
}

static gboolean
gst_mfxdec_flush(GstVideoDecoder * vdec)
{
	GstMfxDec *const decode = GST_MFXDEC(vdec);

	//if (decode->decoder && !gst_mfxdec_internal_flush(vdec))
		//return FALSE;

	/* There could be issues if we avoid the reset_full() while doing
	* seeking: we have to reset the internal state */
	return gst_mfxdec_reset_full(decode, decode->sinkpad_caps, TRUE);
}

static gboolean
gst_mfxdec_set_format(GstVideoDecoder * vdec, GstVideoCodecState * state)
{
	GstMfxPluginBase *const plugin = GST_MFX_PLUGIN_BASE(vdec);
	GstMfxDec *const decode = GST_MFXDEC(vdec);

	GstMfxContextAllocatorVaapi *ctx = &GST_MFX_PLUGIN_BASE(decode)->alloc_ctx;

	if (!gst_mfxdec_input_state_replace(decode, state))
		return TRUE;
	if (!gst_mfxdec_update_sink_caps(decode, state->caps))
		return FALSE;
	if (!gst_mfx_plugin_base_set_caps(plugin, decode->sinkpad_caps, NULL))
		return FALSE;
	if (!gst_mfxdec_reset_full(decode, decode->sinkpad_caps, FALSE))
		return FALSE;

	return TRUE;
}

static GstFlowReturn
gst_mfxdec_handle_frame(GstVideoDecoder *vdec, GstVideoCodecFrame * frame)
{
	GstMfxDec *mfxdec = GST_MFXDEC(vdec);
	GstMfxDecoderStatus sts;
	GstFlowReturn ret = GST_FLOW_OK;

	if (!mfxdec->input_state)
		goto not_negotiated;

	sts = gst_mfx_decoder_decode(mfxdec->decoder, frame);

	switch (sts) {
	case GST_MFX_DECODER_STATUS_READY:
		if (!gst_mfxdec_update_src_caps(mfxdec))
			goto not_negotiated;
		if (!gst_video_decoder_negotiate(vdec))
			goto not_negotiated;
		/* Fall through */
	case GST_MFX_DECODER_STATUS_ERROR_NO_DATA:
		GST_VIDEO_CODEC_FRAME_FLAG_SET(frame,
			GST_VIDEO_CODEC_FRAME_FLAG_DECODE_ONLY);
		ret = GST_FLOW_OK;
		break;
    case GST_MFX_DECODER_STATUS_SUCCESS:
		ret = gst_mfxdec_push_decoded_frame(mfxdec, frame);
		break;
	case GST_MFX_DECODER_STATUS_ERROR_INIT_FAILED:
	case GST_MFX_DECODER_STATUS_ERROR_BITSTREAM_PARSER:
		goto error_decode;
	default:
		ret = GST_FLOW_ERROR;
	}

	return ret;

error_decode:
	{
		GST_ERROR("MFX decode error %d", sts);
		gst_video_decoder_drop_frame(vdec, frame);
		return GST_FLOW_NOT_SUPPORTED;
	}
not_negotiated:
	{
		GST_ERROR_OBJECT(mfxdec, "not negotiated");
		gst_video_decoder_drop_frame(vdec, frame);
		return GST_FLOW_NOT_SUPPORTED;
	}
}

static void
gst_mfxdec_class_init(GstMfxDecClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
	GstVideoDecoderClass *video_decoder_class = GST_VIDEO_DECODER_CLASS(klass);

	GST_DEBUG_CATEGORY_INIT(mfxdec_debug, "mfxdec", 0, "MFX Video Decoder");

	gst_mfx_plugin_base_class_init(GST_MFX_PLUGIN_BASE_CLASS(klass));

	gobject_class->set_property = gst_mfx_dec_set_property;
	gobject_class->get_property = gst_mfx_dec_get_property;

	g_object_class_install_property(gobject_class, PROP_ASYNC_DEPTH,
		g_param_spec_uint("async-depth", "Asynchronous Depth",
		"Number of async operations before explicit sync",
		0, 16, DEFAULT_ASYNC_DEPTH,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	gst_element_class_add_pad_template(element_class,
		gst_static_pad_template_get(&src_template_factory));
	gst_element_class_add_pad_template(element_class,
		gst_static_pad_template_get(&sink_template_factory));
	gst_element_class_set_static_metadata(element_class,
		"MFX Video Decoder",
		"Codec/Decoder/Video",
		"Uses libmfx for decoding video streams",
		"Ishmael Sameen<ishmael.visayana.sameen@intel.com>");

	video_decoder_class->open = GST_DEBUG_FUNCPTR(gst_mfxdec_open);
	video_decoder_class->close = GST_DEBUG_FUNCPTR(gst_mfxdec_close);
	video_decoder_class->flush = GST_DEBUG_FUNCPTR(gst_mfxdec_flush);
	video_decoder_class->set_format = GST_DEBUG_FUNCPTR(gst_mfxdec_set_format);
	video_decoder_class->handle_frame =
		GST_DEBUG_FUNCPTR(gst_mfxdec_handle_frame);
	video_decoder_class->decide_allocation =
		GST_DEBUG_FUNCPTR(gst_mfxdec_decide_allocation);

}

static void
gst_mfxdec_init(GstMfxDec *mfxdec)
{
	//mfxdec->async_depth = DEFAULT_ASYNC_DEPTH;

	gst_video_decoder_set_packetized(GST_VIDEO_DECODER(mfxdec), TRUE);
	gst_video_decoder_set_needs_format(GST_VIDEO_DECODER(mfxdec), TRUE);
}
