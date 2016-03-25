#include <mfxplugin.h>
#include "gstmfxdecoder.h"
#include "gstmfxfilter.h"
#include "gstmfxsurfacepool.h"
#include "gstmfxvideometa.h"
#include "gstmfxsurfaceproxy.h"
#include "gstmfxtask.h"

#define DEBUG 1
#include "gstmfxdebug.h"

struct _GstMfxDecoder
{
	/*< private >*/
	GstMfxMiniObject parent_instance;

	GstMfxTaskAggregator *context;
	GstMfxTask *decode_task;
	GstMfxSurfacePool *pool;
	GstMfxFilter *filter;
	GAsyncQueue *surfaces;
	GByteArray *bitstream;

	mfxSession session;
	mfxVideoParam param;
	mfxBitstream bs;
	mfxU32 codec;

	gboolean decoder_inited;
};

mfxU32
gst_mfx_decoder_get_codec(GstMfxDecoder * decoder)
{
	g_return_val_if_fail(decoder != NULL, 0);

	return decoder->codec;
}

GstMfxDecoderStatus
gst_mfx_decoder_get_surface_proxy(GstMfxDecoder * decoder,
	GstMfxSurfaceProxy ** out_proxy_ptr)
{
	g_return_val_if_fail(decoder != NULL,
		GST_MFX_DECODER_STATUS_ERROR_INVALID_PARAMETER);
	g_return_val_if_fail(out_proxy_ptr != NULL,
		GST_MFX_DECODER_STATUS_ERROR_INVALID_PARAMETER);

	*out_proxy_ptr = g_async_queue_try_pop(decoder->surfaces);
	if (!*out_proxy_ptr)
        return GST_MFX_DECODER_STATUS_ERROR_NO_SURFACE;

	return GST_MFX_DECODER_STATUS_SUCCESS;
}

static void
gst_mfx_decoder_finalize(GstMfxDecoder *decoder)
{
    g_async_queue_unref(decoder->surfaces);
	g_byte_array_unref(decoder->bitstream);
	gst_mfx_task_aggregator_unref(decoder->context);
	gst_mfx_surface_pool_unref(decoder->pool);

	MFXVideoDECODE_Close(decoder->session);
}


static gboolean
gst_mfx_decoder_init(GstMfxDecoder * decoder,
	GstMfxTaskAggregator * context, mfxU32 codec, mfxU16 async_depth)
{
    memset(&(decoder->bs), 0, sizeof (mfxBitstream));
	memset(&(decoder->param), 0, sizeof (mfxVideoParam));

	decoder->codec = codec;
	decoder->context = gst_mfx_task_aggregator_ref(context);
	decoder->param.mfx.CodecId = codec;
	decoder->param.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
	decoder->param.AsyncDepth = async_depth;
	decoder->decode_task = gst_mfx_task_aggregator_get_current_task(context);
	decoder->surfaces = g_async_queue_new();
	decoder->pool = NULL;
	decoder->decoder_inited = FALSE;
	decoder->bs.MaxLength = 1024 * 16;
    decoder->bitstream = g_byte_array_sized_new(decoder->bs.MaxLength);
    decoder->session = gst_mfx_task_get_session(decoder->decode_task);
    decoder->filter = NULL;

    return TRUE;
}

static inline const GstMfxMiniObjectClass *
gst_mfx_decoder_class(void)
{
	static const GstMfxMiniObjectClass GstMfxDecoderClass = {
		sizeof(GstMfxDecoder),
		(GDestroyNotify)gst_mfx_decoder_finalize
	};
	return &GstMfxDecoderClass;
}

GstMfxDecoder *
gst_mfx_decoder_new(GstMfxTaskAggregator * context,
	mfxU32 codec, mfxU16 async_depth)
{
	GstMfxDecoder *decoder;

	g_return_val_if_fail(context != NULL, NULL);

	decoder = gst_mfx_mini_object_new(gst_mfx_decoder_class());
	if (!decoder)
		goto error;

	if (!gst_mfx_decoder_init(decoder, context, codec, async_depth))
        goto error;

	return decoder;

error:
	gst_mfx_mini_object_unref(decoder);
	return NULL;
}

GstMfxDecoder *
gst_mfx_decoder_ref(GstMfxDecoder * decoder)
{
	g_return_val_if_fail(decoder != NULL, NULL);

	return gst_mfx_mini_object_ref(GST_MFX_MINI_OBJECT(decoder));
}

void
gst_mfx_decoder_unref(GstMfxDecoder * decoder)
{
	gst_mfx_mini_object_unref(GST_MFX_MINI_OBJECT(decoder));
}

void
gst_mfx_decoder_replace(GstMfxDecoder ** old_decoder_ptr,
	GstMfxDecoder * new_decoder)
{
	g_return_if_fail(old_decoder_ptr != NULL);

	gst_mfx_mini_object_replace((GstMfxMiniObject **)old_decoder_ptr,
		GST_MFX_MINI_OBJECT(new_decoder));
}

static mfxStatus
gst_mfx_decoder_load_decoder_plugins(GstMfxDecoder *decoder)
{
    mfxPluginUID uid;
    mfxStatus sts;

    switch (decoder->codec) {
    case MFX_CODEC_HEVC:
    {
        gchar *plugin_uids[] = { "33a61c0b4c27454ca8d85dde757c6f8e",
                                 "15dd936825ad475ea34e35f3f54217a6",
                                 NULL };
        guint i, c;
        for (i = 0; plugin_uids[i]; i++) {
            for (c = 0; c < sizeof(uid.Data); c++)
                sscanf(plugin_uids[i] + 2 * c, "%2hhx", uid.Data + c);
            sts = MFXVideoUSER_Load(decoder->session, &uid, 1);
            if (MFX_ERR_NONE == sts)
                break;
        }
    }
        break;
    default:
        sts = MFX_ERR_NONE;
    }

    return sts;
}

static GstMfxDecoderStatus
gst_mfx_decoder_start(GstMfxDecoder *decoder, GstVideoInfo * info)
{
	GstMfxDecoderStatus ret = GST_MFX_DECODER_STATUS_READY;
	mfxFrameInfo *frame_info = &decoder->param.mfx.FrameInfo;
	mfxStatus sts = MFX_ERR_NONE;

    sts = gst_mfx_decoder_load_decoder_plugins(decoder);
    if (sts < 0)
        return GST_MFX_DECODER_STATUS_ERROR_UNSUPPORTED_CODEC;

	sts = MFXVideoDECODE_DecodeHeader(decoder->session, &decoder->bs,
                &decoder->param);
	if (MFX_ERR_MORE_DATA == sts) {
		return GST_MFX_DECODER_STATUS_ERROR_NO_DATA;
	}
	else if (sts < 0) {
		GST_ERROR("Decode header error %d\n", sts);
		return GST_MFX_DECODER_STATUS_ERROR_BITSTREAM_PARSER;
	}

    /* Fill in missing required frame info, if any */
    if (!frame_info->FrameRateExtN)
        frame_info->FrameRateExtN = info->fps_n;
    if (!frame_info->FrameRateExtD)
        frame_info->FrameRateExtD = info->fps_d;
    if (!frame_info->AspectRatioW)
        frame_info->AspectRatioW = info->par_n;
    if (!frame_info->AspectRatioH)
        frame_info->AspectRatioH = info->par_d;

    //if (GST_VIDEO_INFO_FORMAT(info) != GST_VIDEO_FORMAT_NV12)
        gst_mfx_task_set_task_type(decoder->decode_task, GST_MFX_TASK_VPP_IN);

	if (gst_mfx_task_has_type(decoder->decode_task, GST_MFX_TASK_VPP_IN)) {
        mfxFrameAllocRequest dec_request;

        decoder->filter = gst_mfx_filter_new_with_session(decoder->context,
                                &decoder->session);

		memset(&dec_request, 0, sizeof (mfxFrameAllocRequest));

		sts = MFXVideoDECODE_QueryIOSurf(decoder->session, &decoder->param,
                    &dec_request);
		if (sts < 0) {
            GST_ERROR("Unable to query decode allocation request %d", sts);
            return GST_MFX_DECODER_STATUS_ERROR_ALLOCATION_FAILED;
        }

		dec_request.NumFrameSuggested =
            (dec_request.NumFrameSuggested - decoder->param.AsyncDepth) + 1;
		dec_request.Type =
            MFX_MEMTYPE_EXTERNAL_FRAME | MFX_MEMTYPE_FROM_DECODE |
            MFX_MEMTYPE_EXPORT_FRAME;

        gst_mfx_filter_set_request(decoder->filter, &dec_request,
            GST_MFX_TASK_VPP_IN);

        gst_mfx_filter_set_format(decoder->filter,
            gst_video_format_to_mfx_fourcc(GST_VIDEO_INFO_FORMAT(info)));

        if(!gst_mfx_filter_set_saturation(decoder->filter, 3.0))
            return FALSE;

        if(!gst_mfx_filter_start(decoder->filter))
            return GST_MFX_DECODER_STATUS_ERROR_INIT_FAILED;

        decoder->pool = gst_mfx_filter_get_pool(decoder->filter,
                            GST_MFX_TASK_VPP_IN);
        if (!decoder->pool)
            return GST_MFX_DECODER_STATUS_ERROR_ALLOCATION_FAILED;
	}
	else {
        decoder->pool = gst_mfx_surface_pool_new(decoder->decode_task);
        if (!decoder->pool)
            return GST_MFX_DECODER_STATUS_ERROR_ALLOCATION_FAILED;
	}

	sts = MFXVideoDECODE_Init(decoder->session, &decoder->param);
	if (sts < 0) {
		GST_ERROR("Error initializing the MFX video decoder %d", sts);
		return GST_MFX_DECODER_STATUS_ERROR_INIT_FAILED;
	}

	return ret;
}

GstMfxDecoderStatus
gst_mfx_decoder_decode(GstMfxDecoder * decoder,
	GstVideoCodecFrame * frame, GstVideoInfo *info)
{
	GstMapInfo minfo;
	GstMfxDecoderStatus ret = GST_MFX_DECODER_STATUS_SUCCESS;

	GstMfxSurfaceProxy *proxy, *out_proxy;
	mfxFrameSurface1 *insurf, *outsurf = NULL;
	mfxSyncPoint syncp;
	mfxStatus sts = MFX_ERR_NONE;

	if (!gst_buffer_map(frame->input_buffer, &minfo, GST_MAP_READ)) {
		GST_ERROR("Failed to map input buffer");
		return GST_MFX_DECODER_STATUS_ERROR_UNKNOWN;
	}

	if (minfo.size) {
        decoder->bs.DataLength += minfo.size;
        if (decoder->bs.MaxLength < decoder->bs.DataLength + decoder->bitstream->len)
            decoder->bs.MaxLength = decoder->bs.DataLength + decoder->bitstream->len;
		decoder->bitstream = g_byte_array_append(decoder->bitstream,
                                minfo.data, minfo.size);
		decoder->bs.Data = decoder->bitstream->data;
		decoder->bs.TimeStamp = GST_BUFFER_PTS(frame->input_buffer);
	}

	/* Initialize the MFX decoder session */
	if (G_UNLIKELY(!decoder->decoder_inited)) {
		ret = gst_mfx_decoder_start(decoder, info);
		if (GST_MFX_DECODER_STATUS_READY == ret)
			decoder->decoder_inited = TRUE;
        else
            goto end;
	}

	do {
        proxy = gst_mfx_surface_proxy_new_from_pool(decoder->pool);
        if (!proxy)
            return GST_MFX_DECODER_STATUS_ERROR_ALLOCATION_FAILED;

		insurf = gst_mfx_surface_proxy_get_frame_surface(proxy);
		sts = MFXVideoDECODE_DecodeFrameAsync(decoder->session, &decoder->bs,
			insurf, &outsurf, &syncp);

		if (MFX_WRN_DEVICE_BUSY == sts)
			g_usleep(500);
	} while (MFX_WRN_DEVICE_BUSY == sts || MFX_ERR_MORE_SURFACE == sts);

	if (sts == MFX_ERR_MORE_DATA || sts > 0) {
		ret = GST_MFX_DECODER_STATUS_ERROR_NO_DATA;
		goto end;
	}

	if (sts != MFX_ERR_NONE &&
		sts != MFX_ERR_MORE_DATA &&
		sts != MFX_WRN_VIDEO_PARAM_CHANGED) {
		GST_ERROR("Error during MFX decoding.");
		ret = GST_MFX_DECODER_STATUS_ERROR_UNKNOWN;
		goto end;
	}

	if (syncp) {
		decoder->bitstream = g_byte_array_remove_range(decoder->bitstream, 0,
            decoder->bs.DataOffset);
		decoder->bs.DataOffset = 0;

		proxy = gst_mfx_surface_pool_find_proxy(decoder->pool, outsurf);

		do {
			sts = MFXVideoCORE_SyncOperation(decoder->session, syncp, 1000);
		} while (MFX_WRN_IN_EXECUTION == sts);

		if (gst_mfx_task_has_type(decoder->decode_task, GST_MFX_TASK_VPP_IN)) {
			gst_mfx_filter_process(decoder->filter, proxy, &out_proxy);

			proxy = out_proxy;
		}

        g_async_queue_push(decoder->surfaces, proxy);
	}

end:
	gst_buffer_unmap(frame->input_buffer, &minfo);

	return ret;
}
