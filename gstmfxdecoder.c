#include "gstmfxdecoder.h"
#include "gstmfxobjectpool_priv.h"
#include "gstmfxvideometa.h"

/**
* gst_mfx_decoder_ref:
* @decoder: a #GstMfxDecoder
*
* Atomically increases the reference count of the given @decoder by one.
*
* Returns: The same @decoder argument
*/
GstMfxDecoder *
gst_mfx_decoder_ref(GstMfxDecoder * decoder)
{
	return gst_mfx_mini_object_ref(decoder);
}

/**
* gst_mfx_decoder_unref:
* @decoder: a #GstMfxDecoder
*
* Atomically decreases the reference count of the @decoder by one. If
* the reference count reaches zero, the decoder will be free'd.
*/
void
gst_mfx_decoder_unref(GstMfxDecoder * decoder)
{
	gst_mfx_mini_object_unref(decoder);
}

/**
* gst_mfx_decoder_replace:
* @old_decoder_ptr: a pointer to a #GstMfxDecoder
* @new_decoder: a #GstMfxDecoder
*
* Atomically replaces the decoder decoder held in @old_decoder_ptr
* with @new_decoder. This means that @old_decoder_ptr shall reference
* a valid decoder. However, @new_decoder can be NULL.
*/
void
gst_mfx_decoder_replace(GstMfxDecoder ** old_decoder_ptr,
GstMfxDecoder * new_decoder)
{
	gst_mfx_mini_object_replace(old_decoder_ptr, new_decoder);
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

	return GST_MFX_DECODER_STATUS_SUCCESS;
}

static void
gst_mfx_decoder_push_surface(GstMfxDecoder * decoder, GstMfxSurfaceProxy * proxy)
{
	g_async_queue_push(decoder->surfaces, proxy);
}


static void
destroy_surfaces(GstMfxDecoder *decoder)
{
	gst_mfx_object_pool_unref(decoder->pool);
}


static void
gst_mfx_decoder_finalize(GstMfxDecoder *decoder)
{
	destroy_surfaces(decoder);

	g_slice_free1(decoder->bs.MaxLength, decoder->bs.Data);

	MFXVideoDECODE_Close(decoder->session);
}


static void
gst_mfx_decoder_init(GstMfxDecoder *decoder, mfxU32 codec_id, GstMfxContextAllocatorVaapi * ctx)
{
	memset(&(decoder->session), 0, sizeof (mfxSession));
	memset(&(decoder->bs), 0, sizeof (mfxBitstream));
	memset(&(decoder->param), 0, sizeof (mfxVideoParam));

	decoder->param.mfx.CodecId = codec_id;
	decoder->alloc_ctx = ctx;
	decoder->param.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
	decoder->context = NULL;
	decoder->surfaces = g_async_queue_new();
	decoder->pool = NULL;
	decoder->decoder_inited = FALSE;
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
gst_mfx_decoder_new(GstMfxContextAllocatorVaapi *allocator, mfxU32 codec_id)
{
	GstMfxDecoder *decoder;

	decoder = gst_mfx_mini_object_new(gst_mfx_decoder_class());
	if (!decoder)
		goto error;

	gst_mfx_decoder_init(decoder, codec_id, allocator);

	return decoder;

error:
	gst_mfx_mini_object_unref(decoder);
	return NULL;
}

static gboolean
gst_mfx_decoder_ensure_context(GstMfxDecoder *decoder)
{
	if (!decoder->context) {
		decoder->context = gst_mfx_context_new(decoder->alloc_ctx);
		if (!decoder->context)
			return FALSE;

		decoder->session = gst_mfx_context_get_session(decoder->context);
		MFXVideoCORE_SetHandle(decoder->session, MFX_HANDLE_VA_DISPLAY, decoder->alloc_ctx->va_dpy);
	}

	return TRUE;
}

static void
put_unused_frames(gpointer surface, gpointer pool)
{
	GstMfxSurface *_surface = (GstMfxSurface *)surface;
	GstMfxObjectPool *_pool = (GstMfxObjectPool *)pool;

	//g_print("Used objects : %d\n", _pool->used_count);
    //g_print("Free objects : %d\n", g_queue_get_length(&(_pool->free_objects)));

	mfxFrameSurface1 *surf = gst_mfx_surface_get_frame_surface(_surface);
	if (surf && !surf->Data.Locked) {
		gst_mfx_object_pool_put_object(_pool, _surface);
    }
}

static gboolean
get_surface(GstMfxDecoder *decoder, GstMfxSurfaceProxy **proxy)
{
	g_list_foreach(decoder->pool->used_objects, put_unused_frames, decoder->pool);

	*proxy = gst_mfx_surface_proxy_new_from_pool(decoder->pool);

	if (!*proxy)
		return FALSE;

    //g_print("Used surfaces : %d\n", decoder->allocator->used_count);

	return TRUE;
}

static GstMfxDecoderStatus
surface_pool_init(GstMfxDecoder *decoder)
{
	mfxStatus sts;

	decoder->pool = gst_mfx_surface_pool_new(decoder->alloc_ctx);
	if (!decoder->pool)
		return GST_MFX_DECODER_STATUS_ERROR_ALLOCATION_FAILED;

	return GST_MFX_DECODER_STATUS_SUCCESS;
}

static GstMfxDecoderStatus
gst_mfx_decoder_start(GstMfxDecoder *decoder)
{
	GstMfxDecoderStatus ret = GST_MFX_DECODER_STATUS_SUCCESS;
	mfxStatus sts = MFX_ERR_NONE;

	if (!gst_mfx_decoder_ensure_context(decoder))
		return GST_MFX_DECODER_STATUS_ERROR_INIT_FAILED;

	sts = MFXVideoDECODE_DecodeHeader(decoder->session, &decoder->bs, &decoder->param);
	if (MFX_ERR_MORE_DATA == sts) {
		return GST_MFX_DECODER_STATUS_ERROR_NO_DATA;
	}
	else if (sts < 0) {
		GST_ERROR_OBJECT(decoder, "Decode header error %d\n", sts);
		return GST_MFX_DECODER_STATUS_ERROR_BITSTREAM_PARSER;
	}

	sts = MFXVideoDECODE_Init(decoder->session, &decoder->param);
	if (sts < 0) {
		GST_ERROR_OBJECT(decoder, "Error initializing the MFX video decoder %d", sts);
		return GST_MFX_DECODER_STATUS_ERROR_INIT_FAILED;
	}

	ret = surface_pool_init(decoder);
	if (ret) {
		GST_ERROR_OBJECT(decoder, "Error allocating MFX input frame surfaces");
		return ret;
	}

	return ret;
}

GstMfxDecoderStatus
gst_mfx_decoder_decode(GstMfxDecoder * decoder,
	GstVideoCodecFrame * frame)
{
	GstMapInfo minfo;
	GstMfxDecoderStatus ret = GST_MFX_DECODER_STATUS_SUCCESS;

	GstMfxSurfaceProxy *proxy;
	GstMfxSurface *surface;
	GstMfxRectangle crop_rect;
	mfxFrameSurface1 *insurf;
	mfxFrameSurface1 *outsurf = NULL;
	mfxSyncPoint sync;
	mfxStatus sts = MFX_ERR_NONE;

	if (!gst_buffer_map(frame->input_buffer, &minfo, GST_MAP_READ)) {
		GST_ERROR_OBJECT(decoder, "Failed to map input buffer");
		return GST_MFX_DECODER_STATUS_ERROR_UNKNOWN;
	}

	if (decoder->bs.Data == NULL) {
		decoder->bs.MaxLength = 1024 * 1024;
		decoder->bs.Data = g_slice_alloc(decoder->bs.MaxLength);
	}

	if (minfo.size) {
		memcpy(decoder->bs.Data + decoder->bs.DataOffset + decoder->bs.DataLength, minfo.data, minfo.size);
		decoder->bs.DataLength += minfo.size;
		decoder->bs.TimeStamp = GST_BUFFER_PTS(frame->input_buffer);
	}

	/* Initialize the MFX decoder session */
	if (G_UNLIKELY(!decoder->decoder_inited)) {
		ret = gst_mfx_decoder_start(decoder);

		switch (ret) {
		case GST_MFX_DECODER_STATUS_SUCCESS:
			decoder->decoder_inited = TRUE;
			return GST_MFX_DECODER_STATUS_READY;
		default:
			return ret;
		}
	}

	do {
		if (!get_surface(decoder, &proxy))
			return GST_MFX_DECODER_STATUS_ERROR_ALLOCATION_FAILED;

		insurf = gst_mfx_surface_proxy_get_frame_surface(proxy);
		sts = MFXVideoDECODE_DecodeFrameAsync(decoder->session, &decoder->bs,
			insurf, &outsurf, &sync);

		if (sts == MFX_WRN_DEVICE_BUSY)
			g_usleep(500);
	} while (sts == MFX_WRN_DEVICE_BUSY || sts == MFX_ERR_MORE_SURFACE);

	if (sts == MFX_ERR_MORE_DATA || sts > 0) {
		ret = GST_MFX_DECODER_STATUS_ERROR_NO_DATA;
	}

	if (sts != MFX_ERR_NONE &&
		sts != MFX_ERR_MORE_DATA &&
		sts != MFX_WRN_VIDEO_PARAM_CHANGED &&
		sts != MFX_ERR_MORE_SURFACE) {
		GST_ERROR_OBJECT(decoder, "Error during MFX decoding.");
		return GST_MFX_DECODER_STATUS_ERROR_UNKNOWN;
	}

	if (sync) {
		MFXVideoCORE_SyncOperation(decoder->session, sync, 60000);

		memmove(decoder->bs.Data, decoder->bs.Data + decoder->bs.DataOffset, decoder->bs.DataLength);
		decoder->bs.DataOffset = 0;

		surface = GST_MFX_SURFACE_PROXY_SURFACE(proxy);

		crop_rect.x = surface->surface->Info.CropX;
		crop_rect.y = surface->surface->Info.CropY;
		crop_rect.width = surface->surface->Info.CropW;
		crop_rect.height = surface->surface->Info.CropH;

		gst_mfx_surface_proxy_set_crop_rect(proxy, &crop_rect);

		gst_mfx_decoder_push_surface(decoder, proxy);
	}

	gst_buffer_unmap(frame->input_buffer, &minfo);

	return ret;

}
