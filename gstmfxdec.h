/*
 ============================================================================
 Name        : gst-mfx-dec.h
 Author      : Heiher <admin@heiher.info>
 Version     : 0.0.1
 Copyright   : Copyright (C) 2013 everyone.
 Description :
 ============================================================================
 */

#ifndef __GST_MFX_DEC_H__
#define __GST_MFX_DEC_H__

#include <gst/gst.h>
#include <gst/video/gstvideodecoder.h>

#include <mfxvideo.h>
#include <X11/Xlib.h>
#include <stdio.h>

#include "gstmfxdecoder.h"
#include "gstmfxcontext.h"

G_BEGIN_DECLS

#define GST_TYPE_MFXDEC (gst_mfxdec_get_type ())
#define GST_MFXDEC(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_MFXDEC, GstMfxDec))
#define GST_IS_MFXDEC(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_MFXDEC))
#define GST_MFXDEC_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_MFXDEC, GstMfxDecClass))
#define GST_IS_MFXDEC_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_MFXDEC))
#define GST_MFXDEC_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_MFXDEC, GstMfxDecClass))

typedef struct _GstMfxDec GstMfxDec;
typedef struct _GstMfxDecClass GstMfxDecClass;

struct _GstMfxDec
{
	GstVideoDecoder parent;

	GstVideoCodecState *input_state;
	GstVideoCodecState *output_state;

	//mfxVideoParam param;

	GstMfxDecoder *decoder;

	/* properties */
	int async_depth;
	mfxExtBuffer **ext_buffers;
	int nb_ext_buffers;

	/* VA specific */
	Display *dpy;
	GstMfxContextAllocatorVaapi alloc_ctx;
};

struct _GstMfxDecClass
{
	GstVideoDecoderClass parent_class;
};

GType gst_mfxdec_get_type (void);

G_END_DECLS


#endif /* __GST_MFX_DEC_H__ */

