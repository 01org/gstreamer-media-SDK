/*
 *  Copyright (C) 2012-2013 Intel Corporation
 *    Author: Gwenole Beauchesne <gwenole.beauchesne@intel.com>
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

#include "sysdeps.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <libudev.h>
#include <xf86drm.h>
#include <va/va_drm.h>
#include "gstmfxtypes.h"
#include "gstmfxdisplay_priv.h"
#include "gstmfxdisplay_drm.h"
#include "gstmfxdisplay_drm_priv.h"

#define DEBUG 1
#include "gstmfxdebug.h"

static GMutex g_drm_device_type_lock;

static const gchar *
get_device_path (GstMfxDisplay * display)
{
  GstMfxDisplayDRMPrivate *const priv = GST_MFX_DISPLAY_DRM_PRIVATE (display);
  const gchar *device_path = priv->device_path;

  if (!device_path || *device_path == '\0')
    return NULL;
  return device_path;
}

static gboolean
set_device_path (GstMfxDisplay * display, const gchar * device_path)
{
  GstMfxDisplayDRMPrivate *const priv = GST_MFX_DISPLAY_DRM_PRIVATE (display);

  g_free (priv->device_path);
  priv->device_path = NULL;

  if (!device_path) {
    device_path = get_default_device_path (display);
    if (!device_path)
      return FALSE;
  }
  priv->device_path = g_strdup (device_path);
  return priv->device_path != NULL;
}

/* Set device path from file descriptor */
static gboolean
set_device_path_from_fd (GstMfxDisplay * display, gint drm_device)
{
  GstMfxDisplayDRMPrivate *const priv = GST_MFX_DISPLAY_DRM_PRIVATE (display);
  const gchar *busid, *path, *str;
  gsize busid_length, path_length;
  struct udev *udev = NULL;
  struct udev_device *device;
  struct udev_enumerate *e = NULL;
  struct udev_list_entry *l;
  gboolean success = FALSE;

  g_free (priv->device_path);
  priv->device_path = NULL;

  if (drm_device < 0)
    goto end;

  busid = drmGetBusid (drm_device);
  if (!busid)
    goto end;
  if (strncmp (busid, "pci:", 4) != 0)
    goto end;
  busid += 4;
  busid_length = strlen (busid);

  udev = udev_new ();
  if (!udev)
    goto end;

  e = udev_enumerate_new (udev);
  if (!e)
    goto end;

  udev_enumerate_add_match_subsystem (e, "drm");
  udev_enumerate_scan_devices (e);
  udev_list_entry_foreach (l, udev_enumerate_get_list_entry (e)) {
    path = udev_list_entry_get_name (l);
    str = strstr (path, busid);
    if (!str || str <= path || str[-1] != '/')
      continue;

    path_length = strlen (path);
    if (str + busid_length >= path + path_length)
      continue;
    if (strncmp (&str[busid_length], "/drm/card", 9) != 0 &&
        strncmp (&str[busid_length], "/drm/renderD", 12) != 0)
      continue;

    device = udev_device_new_from_syspath (udev, path);
    if (!device)
      continue;

    path = udev_device_get_devnode (device);
    priv->device_path = g_strdup (path);
    udev_device_unref (device);
    break;
  }
  success = TRUE;

end:
  if (e)
    udev_enumerate_unref (e);
  if (udev)
    udev_unref (udev);
  return success;
}

static gboolean
gst_mfx_display_drm_bind_display (GstMfxDisplay * display,
    gpointer native_display)
{
  GstMfxDisplayDRMPrivate *const priv = GST_MFX_DISPLAY_DRM_PRIVATE (display);

  priv->drm_device = GPOINTER_TO_INT (native_display);
  priv->use_foreign_display = TRUE;

  if (!set_device_path_from_fd (display, priv->drm_device))
    return FALSE;
  return TRUE;
}

static gboolean
gst_mfx_display_drm_open_display (GstMfxDisplay * display, const gchar * name)
{
  GstMfxDisplayDRMPrivate *const priv = GST_MFX_DISPLAY_DRM_PRIVATE (display);

  if (!set_device_path (display, name))
    return FALSE;

  priv->drm_device = open (get_device_path (display), O_RDWR | O_CLOEXEC);
  if (priv->drm_device < 0)
    return FALSE;
  priv->use_foreign_display = FALSE;

  return TRUE;
}

static void
gst_mfx_display_drm_close_display (GstMfxDisplay * display)
{
  GstMfxDisplayDRMPrivate *const priv = GST_MFX_DISPLAY_DRM_PRIVATE (display);

  if (priv->drm_device >= 0) {
    if (!priv->use_foreign_display)
      close (priv->drm_device);
    priv->drm_device = -1;
  }

  if (priv->device_path) {
    g_free (priv->device_path);
    priv->device_path = NULL;
  }
}

static gboolean
gst_mfx_display_drm_get_display_info (GstMfxDisplay * display,
    GstMfxDisplayInfo * info)
{
  GstMfxDisplayDRMPrivate *const priv = GST_MFX_DISPLAY_DRM_PRIVATE (display);

  /* Otherwise, create VA display if there is none already */
  info->native_display = GINT_TO_POINTER (priv->drm_device);
  info->display_name = priv->device_path;
  info->display_type = GST_MFX_DISPLAY_TYPE_DRM;
  return TRUE;
}

static void
gst_mfx_display_drm_init (GstMfxDisplay * display)
{
  GstMfxDisplayDRMPrivate *const priv = GST_MFX_DISPLAY_DRM_PRIVATE (display);

  priv->drm_device = -1;
}

static void
gst_mfx_display_drm_class_init (GstMfxDisplayDRMClass * klass)
{
  GstMfxMiniObjectClass *const object_class = GST_MFX_MINI_OBJECT_CLASS (klass);
  GstMfxDisplayClass *const dpy_class = GST_MFX_DISPLAY_CLASS (klass);

  gst_mfx_display_class_init (&klass->parent_class);

  object_class->size = sizeof (GstMfxDisplayDRM);
  dpy_class->display_type = GST_MFX_DISPLAY_TYPE_DRM;
  dpy_class->init = gst_mfx_display_drm_init;
  dpy_class->bind_display = gst_mfx_display_drm_bind_display;
  dpy_class->open_display = gst_mfx_display_drm_open_display;
  dpy_class->close_display = gst_mfx_display_drm_close_display;
  dpy_class->get_display = gst_mfx_display_drm_get_display_info;
}

static inline const GstMfxDisplayClass *
gst_mfx_display_drm_class (void)
{
  static GstMfxDisplayDRMClass g_class;
  static gsize g_class_init = FALSE;

  if (g_once_init_enter (&g_class_init)) {
    gst_mfx_display_drm_class_init (&g_class);
    g_once_init_leave (&g_class_init, TRUE);
  }
  return GST_MFX_DISPLAY_CLASS (&g_class);
}

/**
 * gst_mfx_display_drm_new:
 * @device_path: the DRM device path
 *
 * Opens an DRM file descriptor using @device_path and returns a newly
 * allocated #GstMfxDisplay object. The DRM display will be cloed
 * when the reference count of the object reaches zero.
 *
 * If @device_path is NULL, the DRM device path will be automatically
 * determined as the first positive match in the list of available DRM
 * devices.
 *
 * Return value: a newly allocated #GstMfxDisplay object
 */
GstMfxDisplay *
gst_mfx_display_drm_new (const gchar * device_path)
{
  GstMfxDisplay *display;

  g_mutex_lock (&g_drm_device_type_lock);
  display = gst_mfx_display_new (gst_mfx_display_drm_class (),
      GST_MFX_DISPLAY_INIT_FROM_DISPLAY_NAME, (gpointer) device_path);
  g_mutex_unlock (&g_drm_device_type_lock);
  return display;
}

/**
 * gst_mfx_display_drm_new_with_device:
 * @device: an open DRM device (file descriptor)
 *
 * Creates a #GstMfxDisplay based on the open DRM @device. The
 * caller still owns the device file descriptor and must call close()
 * when all #GstMfxDisplay references are released. Doing so too
 * early can yield undefined behaviour.
 *
 * Return value: a newly allocated #GstMfxDisplay object
 */
GstMfxDisplay *
gst_mfx_display_drm_new_with_device (gint device)
{
  g_return_val_if_fail (device >= 0, NULL);

  return gst_mfx_display_new (gst_mfx_display_drm_class (),
      GST_MFX_DISPLAY_INIT_FROM_NATIVE_DISPLAY, GINT_TO_POINTER (device));
}

/**
 * gst_mfx_display_drm_get_device:
 * @display: a #GstMfxDisplayDRM
 *
 * Returns the underlying DRM device file descriptor that was created
 * by gst_mfx_display_drm_new() or that was bound from
 * gst_mfx_display_drm_new_with_device().
 *
 * Return value: the DRM file descriptor attached to @display
 */
gint
gst_mfx_display_drm_get_device (GstMfxDisplayDRM * display)
{
  g_return_val_if_fail (GST_MFX_IS_DISPLAY_DRM (display), -1);

  return GST_MFX_DISPLAY_DRM_DEVICE (display);
}

/**
 * gst_mfx_display_drm_get_device_path:
 * @display: a #GstMfxDisplayDRM
 *
 * Returns the underlying DRM device path name was created by
 * gst_mfx_display_drm_new() or that was bound from
 * gst_mfx_display_drm_new_with_device().
 *
 * Note: the #GstMfxDisplayDRM object owns the resulting string, so
 * it shall not be deallocated.
 *
 * Return value: the DRM device path name attached to @display
 */
const gchar *
gst_mfx_display_drm_get_device_path (GstMfxDisplayDRM * display)
{
  g_return_val_if_fail (GST_MFX_IS_DISPLAY_DRM (display), NULL);

  return get_device_path (GST_MFX_DISPLAY_CAST (display));
}
