#ifndef GST_MFX_PRIME_BUFFER_PROXY_H
#define GST_MFX_PRIME_BUFFER_PROXY_H

G_BEGIN_DECLS

#define GST_MFX_PRIME_BUFFER_PROXY(obj) \
	((GstMfxPrimeBufferProxy *)(obj))

/**
* GST_MFX_PRIME_BUFFER_PROXY_HANDLE:
* @buf: a #GstMfxPrimeBufferProxy
*
* Macro that evaluates to the handle of the underlying VA buffer @buf
*/
#define GST_MFX_PRIME_BUFFER_PROXY_HANDLE(buf) \
	gst_mfx_prime_buffer_proxy_get_handle (GST_MFX_PRIME_BUFFER_PROXY (buf))

typedef struct _GstMfxPrimeBufferProxy GstMfxPrimeBufferProxy;

GstMfxPrimeBufferProxy *
gst_mfx_prime_buffer_proxy_ref(GstMfxPrimeBufferProxy * proxy);

void
gst_mfx_prime_buffer_proxy_unref(GstMfxPrimeBufferProxy * proxy);

void
gst_mfx_prime_buffer_proxy_replace(GstMfxPrimeBufferProxy ** old_proxy_ptr,
	GstMfxPrimeBufferProxy * new_proxy);

guintptr
gst_mfx_prime_buffer_proxy_get_handle(GstMfxPrimeBufferProxy * proxy);

G_END_DECLS

#endif /* GST_VAAPI_BUFFER_PROXY_H */
