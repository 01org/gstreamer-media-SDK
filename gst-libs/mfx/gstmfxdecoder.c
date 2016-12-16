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

#include <mfxplugin.h>
#include <mfxvp8.h>

#include "gstmfxdecoder.h"
#include "gstmfxfilter.h"
#include "gstmfxsurfacepool.h"
#include "gstmfxvideometa.h"
#include "gstmfxsurface.h"
#include "gstmfxtask.h"

#define DEBUG 1
#include "gstmfxdebug.h"

struct _GstMfxDecoder
{
  /*< private > */
  GstMfxMiniObject parent_instance;

  GstMfxTaskAggregator *aggregator;
  GstMfxTask *decode;
  GstMfxProfile profile;
  GstMfxSurfacePool *pool;
  GstMfxFilter *filter;
  GByteArray *bitstream;

  GQueue *decoded_frames;

  mfxSession session;
  mfxVideoParam params;
  mfxBitstream bs;
  mfxPluginUID plugin_uid;
  mfxFrameAllocRequest request;

  GstVideoInfo info;
  gboolean inited;
  gboolean memtype_is_system;
  gboolean live_mode;

  GstClockTime current_pts;
  GstClockTime pts_offset;
  GstClockTime duration;
  guint current_frame_num;
};

GstMfxProfile
gst_mfx_decoder_get_profile (GstMfxDecoder * decoder)
{
  g_return_val_if_fail (decoder != NULL, 0);

  return decoder->profile;
}

gboolean
gst_mfx_decoder_get_decoded_frames (GstMfxDecoder * decoder,
    GstVideoCodecFrame ** out_frame)
{
  *out_frame = g_queue_pop_tail (decoder->decoded_frames);

  return *out_frame ? TRUE : FALSE;
}

GstVideoInfo *
gst_mfx_decoder_get_video_info (GstMfxDecoder * decoder)
{
  g_return_val_if_fail (decoder != NULL, NULL);

  return &decoder->info;
}

void
gst_mfx_decoder_use_video_memory (GstMfxDecoder * decoder,
    gboolean memtype_is_video)
{
  g_return_if_fail (decoder != NULL);

  if (decoder->memtype_is_system)
    return;

  if (memtype_is_video) {
    decoder->memtype_is_system = FALSE;
    decoder->params.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
    gst_mfx_task_use_video_memory (decoder->decode);
  }
  else {
    decoder->memtype_is_system = TRUE;
    decoder->params.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    gst_mfx_task_ensure_memtype_is_system(decoder->decode);
  }
}

static void
gst_mfx_decoder_finalize (GstMfxDecoder * decoder)
{
  gst_mfx_filter_replace (&decoder->filter, NULL);

  g_byte_array_unref (decoder->bitstream);
  gst_mfx_task_aggregator_unref (decoder->aggregator);
  gst_mfx_surface_pool_replace (&decoder->pool, NULL);
  g_queue_free_full (decoder->decoded_frames, gst_video_codec_frame_unref);

  if ((decoder->params.mfx.CodecId == MFX_CODEC_VP8) ||
#ifdef HAS_VP9
      (decoder->params.mfx.CodecId == MFX_CODEC_VP9) ||
#endif
      (decoder->params.mfx.CodecId == MFX_CODEC_HEVC))
    MFXVideoUSER_UnLoad(decoder->session, &decoder->plugin_uid);
  MFXVideoDECODE_Close (decoder->session);

  gst_mfx_task_replace (&decoder->decode, NULL);
}

static mfxStatus
gst_mfx_decoder_configure_plugins (GstMfxDecoder * decoder)
{
  mfxStatus sts;
  guint i, c;

  switch (decoder->params.mfx.CodecId) {
    case MFX_CODEC_HEVC: {
      gchar *uids[] = {
        "33a61c0b4c27454ca8d85dde757c6f8e",
        "15dd936825ad475ea34e35f3f54217a6",
        NULL
      };
      for (i = 0; uids[i]; i++) {
        for (c = 0; c < sizeof (decoder->plugin_uid.Data); c++)
          sscanf (uids[i] + 2 * c, "%2hhx", decoder->plugin_uid.Data + c);
        sts = MFXVideoUSER_Load (decoder->session, &decoder->plugin_uid, 1);
        if (MFX_ERR_NONE == sts) {
          if (!g_strcmp0 (uids[i], "15dd936825ad475ea34e35f3f54217a6"))
            decoder->params.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
          break;
        }
      }
      break;
    }
    case MFX_CODEC_VP8: {
      gchar *uid = "f622394d8d87452f878c51f2fc9b4131";
      for (c = 0; c < sizeof (decoder->plugin_uid.Data); c++)
        sscanf (uid + 2 * c, "%2hhx", decoder->plugin_uid.Data + c);
      sts = MFXVideoUSER_Load (decoder->session, &decoder->plugin_uid, 1);

      break;
    }
#ifdef HAS_VP9
    case MFX_CODEC_VP9: {
      gchar *uid = "a922394d8d87452f878c51f2fc9b4131";
      for (c = 0; c < sizeof (decoder->plugin_uid.Data); c++)
        sscanf (uid + 2 * c, "%2hhx", decoder->plugin_uid.Data + c);
      sts = MFXVideoUSER_Load (decoder->session, &decoder->plugin_uid, 1);

      break;
    }
#endif
    default:
      sts = MFX_ERR_NONE;
  }

  return sts;
}

static void
gst_mfx_decoder_set_video_properties (GstMfxDecoder * decoder)
{
  mfxFrameInfo *frame_info = &decoder->params.mfx.FrameInfo;

  frame_info->ChromaFormat = MFX_CHROMAFORMAT_YUV420;
  frame_info->FourCC = MFX_FOURCC_NV12;
#ifndef WITH_MSS
  if (decoder->params.mfx.CodecId == MFX_CODEC_JPEG) {
    frame_info->FourCC = MFX_FOURCC_RGB4;
    frame_info->ChromaFormat = MFX_CHROMAFORMAT_YUV444;
  }
#endif

  frame_info->PicStruct = GST_VIDEO_INFO_IS_INTERLACED (&decoder->info) ?
      (GST_VIDEO_INFO_FLAG_IS_SET (&decoder->info,
          GST_VIDEO_FRAME_FLAG_TFF) ? MFX_PICSTRUCT_FIELD_TFF :
          MFX_PICSTRUCT_FIELD_BFF)
      : MFX_PICSTRUCT_PROGRESSIVE;

  frame_info->CropX = 0;
  frame_info->CropY = 0;
  frame_info->CropW = decoder->info.width;
  frame_info->CropH = decoder->info.height;
  frame_info->FrameRateExtN = decoder->info.fps_n ? decoder->info.fps_n : 30;
  frame_info->FrameRateExtD = decoder->info.fps_d;
  frame_info->AspectRatioW = decoder->info.par_n;
  frame_info->AspectRatioH = decoder->info.par_d;
  frame_info->BitDepthChroma = 8;
  frame_info->BitDepthLuma = 8;

  frame_info->Width = GST_ROUND_UP_16 (decoder->info.width);
  frame_info->Height =
      (MFX_PICSTRUCT_PROGRESSIVE == frame_info->PicStruct) ?
          GST_ROUND_UP_16 (decoder->info.height) :
          GST_ROUND_UP_32 (decoder->info.height);

  decoder->params.mfx.CodecProfile =
      gst_mfx_profile_get_codec_profile(decoder->profile);
}

static gboolean
task_init (GstMfxDecoder * decoder)
{
  mfxStatus sts = MFX_ERR_NONE;

  decoder->decode = gst_mfx_task_new (decoder->aggregator,
      GST_MFX_TASK_DECODER);
  if (!decoder->decode)
    return FALSE;

  gst_mfx_task_aggregator_set_current_task (decoder->aggregator,
      decoder->decode);
  decoder->session = gst_mfx_task_get_session (decoder->decode);

  gst_mfx_decoder_set_video_properties (decoder);

  sts = gst_mfx_decoder_configure_plugins (decoder);
  if (sts < 0)
    return FALSE;

  sts = MFXVideoDECODE_QueryIOSurf (decoder->session, &decoder->params,
      &decoder->request);
  if (sts < 0) {
    GST_ERROR ("Unable to query decode allocation request %d", sts);
    return FALSE;
  } else if (sts == MFX_WRN_PARTIAL_ACCELERATION) {
    decoder->params.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
  }

  decoder->memtype_is_system =
    !!(decoder->params.IOPattern & MFX_IOPATTERN_OUT_SYSTEM_MEMORY);
  decoder->request.Type = decoder->memtype_is_system ?
      MFX_MEMTYPE_SYSTEM_MEMORY : MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET;

  if (decoder->memtype_is_system)
    gst_mfx_task_ensure_memtype_is_system (decoder->decode);

  gst_mfx_task_set_request (decoder->decode, &decoder->request);

  return TRUE;
}

static gboolean
gst_mfx_decoder_init (GstMfxDecoder * decoder,
    GstMfxTaskAggregator * aggregator, GstMfxProfile profile,
    GstVideoInfo * info, mfxU16 async_depth,
    gboolean memtype_is_system, gboolean live_mode)
{
  decoder->info = *info;
  decoder->profile = profile;
  decoder->params.mfx.CodecId = gst_mfx_profile_get_codec(profile);
  /* live streaming configuration cannot be used with VC1 or MPEG2 */
  if (decoder->params.mfx.CodecId == MFX_CODEC_MPEG2 ||
      decoder->params.mfx.CodecId == MFX_CODEC_VC1)
    live_mode = FALSE;

  decoder->params.AsyncDepth = live_mode ? 1 : async_depth;
  decoder->live_mode = live_mode;
  if (decoder->live_mode) {
    decoder->bs.DataFlag = MFX_BITSTREAM_COMPLETE_FRAME;
    decoder->params.mfx.DecodedOrder = 1;
  }

  decoder->memtype_is_system = memtype_is_system;
  decoder->params.IOPattern = memtype_is_system ?
    MFX_IOPATTERN_OUT_SYSTEM_MEMORY : MFX_IOPATTERN_OUT_VIDEO_MEMORY;
  decoder->inited = FALSE;
  decoder->bs.MaxLength = 1024 * 16;
  decoder->bitstream = g_byte_array_sized_new (decoder->bs.MaxLength);
  if (!decoder->bitstream)
    return FALSE;
  decoder->decoded_frames = g_queue_new();
  if (!decoder->decoded_frames)
    return FALSE;

  decoder->aggregator = gst_mfx_task_aggregator_ref (aggregator);
  if (!task_init(decoder))
    return FALSE;

  return TRUE;
}

static inline const GstMfxMiniObjectClass *
gst_mfx_decoder_class (void)
{
  static const GstMfxMiniObjectClass GstMfxDecoderClass = {
    sizeof (GstMfxDecoder),
    (GDestroyNotify) gst_mfx_decoder_finalize
  };
  return &GstMfxDecoderClass;
}

GstMfxDecoder *
gst_mfx_decoder_new (GstMfxTaskAggregator * aggregator,
    GstMfxProfile profile, GstVideoInfo * info, mfxU16 async_depth,
    gboolean memtype_is_system, gboolean live_mode)
{
  GstMfxDecoder *decoder;

  g_return_val_if_fail (aggregator != NULL, NULL);

  decoder = gst_mfx_mini_object_new0 (gst_mfx_decoder_class ());
  if (!decoder)
    goto error;

  if (!gst_mfx_decoder_init (decoder, aggregator, profile, info,
            async_depth, memtype_is_system, live_mode))
    goto error;

  return decoder;
error:
  gst_mfx_mini_object_unref (decoder);
  return NULL;
}

GstMfxDecoder *
gst_mfx_decoder_ref (GstMfxDecoder * decoder)
{
  g_return_val_if_fail (decoder != NULL, NULL);

  return gst_mfx_mini_object_ref (GST_MFX_MINI_OBJECT (decoder));
}

void
gst_mfx_decoder_unref (GstMfxDecoder * decoder)
{
  gst_mfx_mini_object_unref (GST_MFX_MINI_OBJECT (decoder));
}

void
gst_mfx_decoder_replace (GstMfxDecoder ** old_decoder_ptr,
    GstMfxDecoder * new_decoder)
{
  g_return_if_fail (old_decoder_ptr != NULL);

  gst_mfx_mini_object_replace ((GstMfxMiniObject **) old_decoder_ptr,
      GST_MFX_MINI_OBJECT (new_decoder));
}

static GstMfxDecoderStatus
gst_mfx_decoder_start (GstMfxDecoder * decoder)
{
  GstMfxDecoderStatus ret = GST_MFX_DECODER_STATUS_SUCCESS;
  mfxU32 output_fourcc, decoded_fourcc;
  mfxStatus sts = MFX_ERR_NONE;

  if (decoder->params.mfx.CodecId != MFX_CODEC_JPEG) {
    sts = MFXVideoDECODE_DecodeHeader (decoder->session, &decoder->bs,
        &decoder->params);
    if (MFX_ERR_MORE_DATA == sts) {
      return GST_MFX_DECODER_STATUS_ERROR_NO_DATA;
    } else if (sts < 0) {
      GST_ERROR ("Decode header error %d\n", sts);
      return GST_MFX_DECODER_STATUS_ERROR_BITSTREAM_PARSER;
    }
  }

  output_fourcc =
      gst_video_format_to_mfx_fourcc (GST_VIDEO_INFO_FORMAT (&decoder->info));
  decoded_fourcc = decoder->params.mfx.FrameInfo.FourCC;

  decoder->request.Info = decoder->params.mfx.FrameInfo;
  gst_mfx_task_set_request (decoder->decode, &decoder->request);

  if  (output_fourcc != decoded_fourcc) {
    decoder->filter = gst_mfx_filter_new_with_task (decoder->aggregator,
        decoder->decode, GST_MFX_TASK_VPP_IN,
        decoder->memtype_is_system, decoder->memtype_is_system);

    if (!decoder->filter)
      return GST_MFX_DECODER_STATUS_ERROR_UNKNOWN;

    decoder->request.Type |=
        MFX_MEMTYPE_EXTERNAL_FRAME | MFX_MEMTYPE_FROM_DECODE |
        MFX_MEMTYPE_EXPORT_FRAME;

    decoder->request.NumFrameSuggested += (1 - decoder->params.AsyncDepth);

    gst_mfx_filter_set_request (decoder->filter, &decoder->request,
        GST_MFX_TASK_VPP_IN);

    gst_mfx_filter_set_frame_info (decoder->filter, &decoder->info);
    gst_mfx_filter_set_format (decoder->filter, output_fourcc);
    gst_mfx_filter_set_async_depth(decoder->filter, decoder->params.AsyncDepth);

    if (!gst_mfx_filter_prepare (decoder->filter))
      return GST_MFX_DECODER_STATUS_ERROR_INIT_FAILED;

    decoder->pool = gst_mfx_filter_get_pool (decoder->filter,
        GST_MFX_TASK_VPP_IN);
    if (!decoder->pool)
      return GST_MFX_DECODER_STATUS_ERROR_ALLOCATION_FAILED;
  }

  sts = MFXVideoDECODE_Init (decoder->session, &decoder->params);
  if (sts < 0) {
    GST_ERROR ("Error initializing the MFX video decoder %d", sts);
    return GST_MFX_DECODER_STATUS_ERROR_INIT_FAILED;
  }

  if (!decoder->pool) {
    decoder->pool = gst_mfx_surface_pool_new_with_task (decoder->decode);
    if (!decoder->pool)
      return GST_MFX_DECODER_STATUS_ERROR_ALLOCATION_FAILED;
  }

  return ret;
}

void
gst_mfx_decoder_reset (GstMfxDecoder * decoder)
{
  if (decoder->info.interlace_mode == GST_VIDEO_INTERLACE_MODE_MIXED)
    return;

  MFXVideoDECODE_Reset (decoder->session, &decoder->params);

  decoder->pts_offset = 0;
  decoder->current_pts = 0;

  if (decoder->bitstream->len)
    g_byte_array_remove_range (decoder->bitstream, 0,
      decoder->bitstream->len);
  memset(&decoder->bs, 0, sizeof(mfxBitstream));
}

static GstVideoCodecFrame *
new_frame (GstMfxDecoder * decoder)
{
  GstVideoCodecFrame *frame = g_slice_new0 (GstVideoCodecFrame);
  if (!frame)
    return NULL;
  frame->ref_count = 1;

  if (!decoder->duration) {
    mfxFrameInfo *info = &decoder->request.Info;
    decoder->duration =
        (info->FrameRateExtD / (gfloat)info->FrameRateExtN) * 1000000000;
  }
  frame->duration = decoder->duration;
  frame->presentation_frame_number = decoder->current_frame_num++;
  frame->pts = decoder->current_pts + decoder->pts_offset;
  decoder->current_pts += decoder->duration;

  return frame;
}

GstMfxDecoderStatus
gst_mfx_decoder_decode (GstMfxDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstMapInfo minfo;
  GstMfxDecoderStatus ret = GST_MFX_DECODER_STATUS_SUCCESS;
  GstMfxFilterStatus filter_sts;
  GstMfxSurface *surface, *filter_surface;
  mfxFrameSurface1 *insurf, *outsurf = NULL;
  mfxSyncPoint syncp;
  mfxStatus sts = MFX_ERR_NONE;
  GstVideoCodecFrame *out_frame;

  if (!decoder->pts_offset && frame->pts != 0xffffffffffffffff)
    decoder->pts_offset = frame->pts;

  if (!gst_buffer_map (frame->input_buffer, &minfo, GST_MAP_READWRITE)) {
    GST_ERROR ("Failed to map input buffer");
    return GST_MFX_DECODER_STATUS_ERROR_UNKNOWN;
  }

  if (minfo.size) {
    decoder->bs.DataLength += minfo.size;
    if (decoder->bs.MaxLength <
        decoder->bs.DataLength + decoder->bitstream->len)
      decoder->bs.MaxLength = decoder->bs.DataLength + decoder->bitstream->len;
    decoder->bitstream = g_byte_array_append (decoder->bitstream,
        minfo.data, minfo.size);
    decoder->bs.Data = decoder->bitstream->data;
  }

  /* Initialize the MFX decoder session */
  if (G_UNLIKELY (!decoder->inited)) {
    ret = gst_mfx_decoder_start (decoder);
    if (GST_MFX_DECODER_STATUS_SUCCESS == ret)
      decoder->inited = TRUE;
    else
      goto end;
  }

  do {
    surface = gst_mfx_surface_new_from_pool (decoder->pool);
    if (!surface)
      return GST_MFX_DECODER_STATUS_ERROR_ALLOCATION_FAILED;

    insurf = gst_mfx_surface_get_frame_surface (surface);
    sts = MFXVideoDECODE_DecodeFrameAsync (decoder->session, &decoder->bs,
        insurf, &outsurf, &syncp);
    GST_DEBUG ("MFXVideoDECODE_DecodeFrameAsync status: %d", sts);

    if (MFX_ERR_MORE_SURFACE == sts || sts > 0) {
      if (MFX_WRN_DEVICE_BUSY == sts)
        g_usleep (100);
      continue;
    }

    if (sts != MFX_ERR_NONE &&
        sts != MFX_ERR_MORE_DATA &&
        sts != MFX_ERR_MORE_SURFACE) {
      GST_ERROR ("Status %d : Error during MFX decoding", sts);
      ret = GST_MFX_DECODER_STATUS_ERROR_UNKNOWN;
      break;
    }

    if (sts == MFX_ERR_MORE_DATA) {
      ret = GST_MFX_DECODER_STATUS_ERROR_NO_DATA;
      if (!decoder->live_mode || !decoder->bs.DataLength) {
        decoder->bitstream = g_byte_array_remove_range (decoder->bitstream, 0,
          decoder->bs.DataOffset);
        decoder->bs.DataOffset = 0;
        break;
      }
    }

    if (syncp) {
      if (!gst_mfx_task_has_type (decoder->decode, GST_MFX_TASK_ENCODER))
        do {
          sts = MFXVideoCORE_SyncOperation (decoder->session, syncp, 1000);
          GST_DEBUG ("MFXVideoCORE_SyncOperation status: %d", sts);
        } while (MFX_WRN_IN_EXECUTION == sts);

      surface = gst_mfx_surface_pool_find_surface (decoder->pool, outsurf);

      /* Update stream properties if they have interlaced frames */
      switch (outsurf->Info.PicStruct) {
        case MFX_PICSTRUCT_PROGRESSIVE:
          if (decoder->info.interlace_mode != GST_VIDEO_INTERLACE_MODE_MIXED)
            decoder->info.interlace_mode = GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;
          break;
        case MFX_PICSTRUCT_FIELD_TFF:
          GST_VIDEO_INFO_FLAG_SET (&decoder->info, GST_VIDEO_FRAME_FLAG_TFF);
          goto update;
        case MFX_PICSTRUCT_FIELD_BFF:
          GST_VIDEO_INFO_FLAG_UNSET (&decoder->info, GST_VIDEO_FRAME_FLAG_TFF);
update:
          /* Check if stream has progressive frames first.
           * If it does then it should be a mixed interlaced stream */
          if (decoder->info.interlace_mode == GST_VIDEO_INTERLACE_MODE_PROGRESSIVE &&
              decoder->current_frame_num)
            decoder->info.interlace_mode = GST_VIDEO_INTERLACE_MODE_MIXED;
          else {
            if (decoder->info.interlace_mode != GST_VIDEO_INTERLACE_MODE_MIXED)
              decoder->info.interlace_mode = GST_VIDEO_INTERLACE_MODE_INTERLEAVED;
          }
          break;
        default:
          break;
      }

      if (gst_mfx_task_has_type (decoder->decode, GST_MFX_TASK_VPP_IN)) {
        filter_sts = gst_mfx_filter_process (decoder->filter, surface,
            &filter_surface);
        if (GST_MFX_FILTER_STATUS_SUCCESS != filter_sts) {
          GST_ERROR ("MFX post-processing error while decoding.");
          ret = GST_MFX_DECODER_STATUS_ERROR_UNKNOWN;
          goto end;
        }
        surface = filter_surface;
      }
      out_frame = new_frame(decoder);
      gst_video_codec_frame_set_user_data(out_frame,
          gst_mfx_surface_ref (surface), gst_mfx_surface_unref);

      g_queue_push_head(decoder->decoded_frames, out_frame);

      GST_LOG ("decoded frame : %ld", out_frame->presentation_frame_number);

      if (!decoder->live_mode) {
        ret = GST_MFX_DECODER_STATUS_SUCCESS;
        decoder->bitstream = g_byte_array_remove_range (decoder->bitstream, 0,
          decoder->bs.DataOffset);
        decoder->bs.DataOffset = 0;
        break;
      }
    }
  } while (MFX_ERR_MORE_DATA == sts || MFX_ERR_NONE == sts);

end:
  gst_buffer_unmap (frame->input_buffer, &minfo);
  gst_buffer_unref (frame->input_buffer);

  return ret;
}

GstMfxDecoderStatus
gst_mfx_decoder_flush (GstMfxDecoder * decoder,
    GstVideoCodecFrame ** out_frame)
{
  GstMfxDecoderStatus ret;
  GstMfxFilterStatus filter_sts;
  GstMfxSurface *surface, *filter_surface;
  mfxFrameSurface1 *insurf, *outsurf = NULL;
  mfxSyncPoint syncp;
  mfxStatus sts;

  do {
    surface = gst_mfx_surface_new_from_pool (decoder->pool);
    if (!surface)
      return GST_MFX_DECODER_STATUS_ERROR_ALLOCATION_FAILED;

    insurf = gst_mfx_surface_get_frame_surface (surface);
    sts = MFXVideoDECODE_DecodeFrameAsync (decoder->session, NULL,
        insurf, &outsurf, &syncp);
    GST_DEBUG ("MFXVideoDECODE_DecodeFrameAsync status: %d", sts);
    if (sts == MFX_WRN_DEVICE_BUSY)
      g_usleep (100);
  } while (MFX_WRN_DEVICE_BUSY == sts);

  if (sts == MFX_ERR_NONE) {
    do {
      sts = MFXVideoCORE_SyncOperation (decoder->session, syncp, 1000);
      GST_DEBUG ("MFXVideoCORE_SyncOperation status: %d", sts);
    } while (MFX_WRN_IN_EXECUTION == sts);

    surface = gst_mfx_surface_pool_find_surface (decoder->pool, outsurf);

    if (gst_mfx_task_has_type (decoder->decode, GST_MFX_TASK_VPP_IN)) {
      filter_sts = gst_mfx_filter_process (decoder->filter, surface,
          &filter_surface);

      if (GST_MFX_FILTER_STATUS_SUCCESS != filter_sts) {
        GST_ERROR ("MFX post-processing error while decoding.");
        ret = GST_MFX_DECODER_STATUS_ERROR_UNKNOWN;
      }
      surface = filter_surface;
    }
    *out_frame = new_frame (decoder);
    gst_video_codec_frame_set_user_data(*out_frame,
        gst_mfx_surface_ref (surface), gst_mfx_surface_unref);

    GST_LOG ("decoded frame : %ld", (*out_frame)->presentation_frame_number);
    ret = GST_MFX_DECODER_STATUS_SUCCESS;
  } else {
    ret = GST_MFX_DECODER_STATUS_FLUSHED;
  }
  return ret;
}
