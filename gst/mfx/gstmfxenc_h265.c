#include "gstmfxcompat.h"
#include "gstmfxdisplay.h"
#include "gstmfxencoder_h265.h"
#include "gstmfxutils_h265.h"
#include "gstmfxenc_h265.h"
#include "gstmfxpluginutil.h"
#include "gstmfxvideomemory.h"

#define GST_PLUGIN_NAME "mfxh265enc"
#define GST_PLUGIN_DESC "An MFX based H.265 video encoder"

GST_DEBUG_CATEGORY_STATIC (gst_mfx_h265_enc_debug);
#define GST_CAT_DEFAULT gst_mfx_h265_enc_debug

#define GST_CODEC_CAPS                              \
	"video/x-h265, "                                  \
	"stream-format = (string) { hvc1, byte-stream }, " \
	"alignment = (string) au"

static const char gst_mfxenc_h265_sink_caps_str[] =
	GST_MFX_MAKE_SURFACE_CAPS "; "
#ifdef WITH_MSS
    GST_VIDEO_CAPS_MAKE ("{ NV12, YV12, I420, YUY2, BGRA, BGRx }");
#else
	GST_VIDEO_CAPS_MAKE ("{ NV12, YV12, I420, UYVY, YUY2, BGRA, BGRx }");
#endif

static const char gst_mfxenc_h265_src_caps_str[] =
	GST_CODEC_CAPS ", "
	"profile = (string) { main }";

static GstStaticPadTemplate gst_mfxenc_h265_sink_factory =
	GST_STATIC_PAD_TEMPLATE ("sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (gst_mfxenc_h265_sink_caps_str));

static GstStaticPadTemplate gst_mfxenc_h265_src_factory =
	GST_STATIC_PAD_TEMPLATE ("src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (gst_mfxenc_h265_src_caps_str));

/* h265 encode */
G_DEFINE_TYPE (GstMfxEncH265, gst_mfxenc_h265, GST_TYPE_MFXENC);

static void
gst_mfxenc_h265_init (GstMfxEncH265 * encode)
{
	gst_mfxenc_init_properties (GST_MFXENC_CAST (encode));
}

static void
gst_mfxenc_h265_finalize (GObject * object)
{
	G_OBJECT_CLASS (gst_mfxenc_h265_parent_class)->finalize (object);
}

static void
gst_mfxenc_h265_set_property (GObject * object,
guint prop_id, const GValue * value, GParamSpec * pspec)
{
	GstMfxEncClass *const encode_class = GST_MFXENC_GET_CLASS (object);
	GstMfxEnc *const base_encode = GST_MFXENC_CAST (object);

	switch (prop_id) {
	default:
		if (!encode_class->set_property (base_encode, prop_id, value))
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gst_mfxenc_h265_get_property (GObject * object,
guint prop_id, GValue * value, GParamSpec * pspec)
{
	GstMfxEncClass *const encode_class = GST_MFXENC_GET_CLASS (object);
	GstMfxEnc *const base_encode = GST_MFXENC_CAST (object);

	switch (prop_id) {
	default:
		if (!encode_class->get_property (base_encode, prop_id, value))
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/*static mfxU16
get_profile (GstCaps * caps)
{
	mfxU16 profile = MFX_PROFILE_UNKNOWN;
	guint i, j, num_structures, num_values;

	num_structures = gst_caps_get_size (caps);
	for (i = 0; i < num_structures; i++) {
		GstStructure *const structure = gst_caps_get_structure (caps, i);
		const GValue *const value = gst_structure_get_value (structure, "profile");

		if (!value)
			continue;
		if (G_VALUE_HOLDS_STRING (value)) {
			const gchar *str = g_value_get_string (value);
			profile = gst_mfx_utils_h265_get_profile_from_string (str);
		}
	}
	return profile;
}*/

typedef struct
{
	mfxU16 best_profile;
	guint best_score;
} FindBestProfileData;

static void
find_best_profile_value (FindBestProfileData * data, const GValue * value)
{
	const gchar *str;
	mfxU16 profile;
	guint score;

	if (!value || !G_VALUE_HOLDS_STRING (value))
		return;

	str = g_value_get_string (value);
	if (!str)
		return;
	profile = gst_mfx_utils_h265_get_profile_from_string (str);
	if (!profile)
		return;
	score = gst_mfx_utils_h265_get_profile_score (profile);
	if (score < data->best_score)
		return;
	data->best_profile = profile;
	data->best_score = score;
}

static mfxU16
find_best_profile (GstCaps * caps)
{
	FindBestProfileData data;
	guint i, j, num_structures, num_values;

	data.best_profile = MFX_PROFILE_UNKNOWN;
	data.best_score = 0;

	num_structures = gst_caps_get_size (caps);
	for (i = 0; i < num_structures; i++) {
		GstStructure *const structure = gst_caps_get_structure (caps, i);
		const GValue *const value = gst_structure_get_value (structure, "profile");

		if (!value)
			continue;
		if (G_VALUE_HOLDS_STRING (value))
			find_best_profile_value (&data, value);
		else if (GST_VALUE_HOLDS_LIST (value)) {
			num_values = gst_value_list_get_size (value);
			for (j = 0; j < num_values; j++)
				find_best_profile_value (&data, gst_value_list_get_value (value, j));
		}
	}
	return data.best_profile;
}

static gboolean
gst_mfxenc_h265_set_config (GstMfxEnc * base_encode)
{
	GstCaps *allowed_caps;
	mfxU16 profile;

	/* Check for the largest profile that is supported */
	allowed_caps =
		gst_pad_get_allowed_caps (GST_MFX_PLUGIN_BASE_SRC_PAD (base_encode));
	if (!allowed_caps)
		return TRUE;

	//profile = get_profile (allowed_caps);
	profile = find_best_profile (allowed_caps);
	gst_caps_unref (allowed_caps);
	if (profile != MFX_PROFILE_UNKNOWN) {
		GST_INFO ("using %s profile as target decoder constraints",
			gst_mfx_utils_h265_get_profile_string (profile));
		if (!gst_mfx_encoder_h265_set_max_profile (base_encode->encoder, profile))
			return FALSE;
	}
	return TRUE;
}

static GstCaps *
gst_mfxenc_h265_get_caps (GstMfxEnc * base_encode)
{
	GstMfxEncH265 *const encode = GST_MFXENC_H265_CAST (base_encode);
	GstCaps *caps, *allowed_caps;

	caps = gst_caps_from_string (GST_CODEC_CAPS);

	/* Check whether "stream-format" is hvcC mode */
	allowed_caps =
		gst_pad_get_allowed_caps (GST_MFX_PLUGIN_BASE_SRC_PAD (encode));
	if (allowed_caps) {
		const char *stream_format = NULL;
		GstStructure *structure;
		guint i, num_structures;

		num_structures = gst_caps_get_size (allowed_caps);
		for (i = 0; !stream_format && i < num_structures; i++) {
			structure = gst_caps_get_structure (allowed_caps, i);
			if (!gst_structure_has_field_typed (structure, "stream-format",
				G_TYPE_STRING))
				continue;
			stream_format = gst_structure_get_string (structure, "stream-format");
		}
		encode->is_hvc = stream_format && strcmp (stream_format, "hvc1") == 0;
		gst_caps_unref (allowed_caps);
	}
	gst_caps_set_simple (caps, "stream-format", G_TYPE_STRING,
		encode->is_hvc ? "hvc1" : "byte-stream", NULL);

	base_encode->need_codec_data = encode->is_hvc;

	return caps;
}

static GstMfxEncoder *
gst_mfxenc_h265_alloc_encoder (GstMfxEnc * base)
{
	GstMfxPluginBase *const plugin = GST_MFX_PLUGIN_BASE (base);

	if (base->encoder)
		return base->encoder;

	return gst_mfx_encoder_h265_new (
		plugin->aggregator, &plugin->sinkpad_info, plugin->mapped);
}

/* h265 NAL byte stream operations */
static guint8 *
_h265_byte_stream_next_nal (guint8 * buffer, guint32 len, guint32 * nal_size)
{
	const guint8 *cur = buffer;
	const guint8 *const end = buffer + len;
	guint8 *nal_start = NULL;
	guint32 flag = 0xFFFFFFFF;
	guint32 nal_start_len = 0;

	g_assert (len >= 0 && buffer && nal_size);
	if (len < 3) {
		*nal_size = len;
		nal_start = (len ? buffer : NULL);
		return nal_start;
	}

	/*locate head postion */
	if (!buffer[0] && !buffer[1]) {
		if (buffer[2] == 1) {       /* 0x000001 */
			nal_start_len = 3;
		}
		else if (!buffer[2] && len >= 4 && buffer[3] == 1) {      /* 0x00000001 */
			nal_start_len = 4;
		}
	}
	nal_start = buffer + nal_start_len;
	cur = nal_start;

	/*find next nal start position */
	while (cur < end) {
		flag = ((flag << 8) | ((*cur++) & 0xFF));
		if ((flag & 0x00FFFFFF) == 0x00000001) {
			if (flag == 0x00000001)
				*nal_size = cur - 4 - nal_start;
			else
				*nal_size = cur - 3 - nal_start;
			break;
		}
	}
	if (cur >= end) {
		*nal_size = end - nal_start;
		if (nal_start >= end) {
			nal_start = NULL;
		}
	}
	return nal_start;
}

static gboolean
_h265_convert_byte_stream_to_hvc (GstBuffer * buf)
{
	GstMapInfo info;
	guint32 nal_size;
	guint8 *nal_start_code, *nal_body;
	guint8 *frame_end;

	g_assert (buf);

	if (!gst_buffer_map (buf, &info, GST_MAP_READ | GST_MAP_WRITE))
		return FALSE;

	nal_start_code = info.data;
	frame_end = info.data + info.size;
	nal_size = 0;

	while ((frame_end > nal_start_code) &&
		(nal_body = _h265_byte_stream_next_nal (nal_start_code,
		frame_end - nal_start_code, &nal_size)) != NULL) {
		if (!nal_size)
			goto error;

		/* A start code size of 3 indicates the start of an
		* encoded picture in MSDK */
		if (nal_body - nal_start_code == 3) {
			memmove (info.data + 4, nal_body, nal_size);
			/* Precede NALU with NALU size */
			GST_WRITE_UINT32_BE (info.data, nal_size);
			gst_buffer_resize (buf, 0, nal_size + 4);
			break;
		}

		nal_start_code = nal_body + nal_size;
	}
	gst_buffer_unmap (buf, &info);

	return TRUE;

error:
	gst_buffer_unmap (buf, &info);
	return FALSE;
}

static GstFlowReturn
gst_mfxenc_h265_format_buffer (GstMfxEnc * base_encode,
    GstBuffer ** out_buffer_ptr)
{
	GstMfxEncH265 *const encode = GST_MFXENC_H265_CAST (base_encode);

	if (!encode->is_hvc)
		return GST_FLOW_OK;

	/* Convert to hvcC format */
	if (!_h265_convert_byte_stream_to_hvc (*out_buffer_ptr))
		goto error_convert_buffer;
	return GST_FLOW_OK;

	/* ERRORS */
error_convert_buffer:
	{
		GST_ERROR ("failed to convert from bytestream format to hvcC format");
		gst_buffer_replace (out_buffer_ptr, NULL);
		return GST_FLOW_ERROR;
	}
}

static void
gst_mfxenc_h265_class_init (GstMfxEncH265Class * klass)
{
	GObjectClass *const object_class = G_OBJECT_CLASS (klass);
	GstElementClass *const element_class = GST_ELEMENT_CLASS (klass);
	GstMfxEncClass *const encode_class = GST_MFXENC_CLASS (klass);

	GST_DEBUG_CATEGORY_INIT (gst_mfx_h265_enc_debug,
		GST_PLUGIN_NAME, 0, GST_PLUGIN_DESC);

	object_class->finalize = gst_mfxenc_h265_finalize;
	object_class->set_property = gst_mfxenc_h265_set_property;
	object_class->get_property = gst_mfxenc_h265_get_property;

	encode_class->get_properties = gst_mfx_encoder_h265_get_default_properties;
	encode_class->set_config = gst_mfxenc_h265_set_config;
	encode_class->get_caps = gst_mfxenc_h265_get_caps;
	encode_class->alloc_encoder = gst_mfxenc_h265_alloc_encoder;
	encode_class->format_buffer = gst_mfxenc_h265_format_buffer;

	gst_element_class_set_static_metadata (element_class,
		"MFX H.265 encoder",
		"Codec/Encoder/Video",
		GST_PLUGIN_DESC,
		"Ishmael Sameen <ishmael.visayana.sameen@intel.com>");

	/* sink pad */
	gst_element_class_add_pad_template (element_class,
		gst_static_pad_template_get (&gst_mfxenc_h265_sink_factory));

	/* src pad */
	gst_element_class_add_pad_template (element_class,
		gst_static_pad_template_get (&gst_mfxenc_h265_src_factory));

	gst_mfxenc_class_init_properties (encode_class);
}
