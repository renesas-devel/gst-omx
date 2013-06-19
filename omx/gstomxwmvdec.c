/*
 * Copyright (C) 2011, Hewlett-Packard Development Company, L.P.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>, Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include "gstomxwmvdec.h"

GST_DEBUG_CATEGORY_STATIC (gst_omx_wmv_dec_debug_category);
#define GST_CAT_DEFAULT gst_omx_wmv_dec_debug_category

/* prototypes */
static gboolean gst_omx_wmv_dec_is_format_change (GstOMXVideoDec * dec,
    GstOMXPort * port, GstVideoCodecState * state);
static gboolean gst_omx_wmv_dec_set_format (GstOMXVideoDec * dec,
    GstOMXPort * port, GstVideoCodecState * state);
static GstFlowReturn gst_omx_wmv_dec_prepare_frame (GstOMXVideoDec * self,
    GstVideoCodecFrame * frame);

enum
{
  PROP_0
};

#define SEQ_PARAM_BUF_SIZE 24

/* class initialization */

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_omx_wmv_dec_debug_category, "omxwmvdec", 0, \
      "debug category for gst-omx video decoder base class");

G_DEFINE_TYPE_WITH_CODE (GstOMXWMVDec, gst_omx_wmv_dec,
    GST_TYPE_OMX_VIDEO_DEC, DEBUG_INIT);

static void
gst_omx_wmv_dec_class_init (GstOMXWMVDecClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstOMXVideoDecClass *videodec_class = GST_OMX_VIDEO_DEC_CLASS (klass);

  videodec_class->is_format_change =
      GST_DEBUG_FUNCPTR (gst_omx_wmv_dec_is_format_change);
  videodec_class->set_format = GST_DEBUG_FUNCPTR (gst_omx_wmv_dec_set_format);
  videodec_class->prepare_frame =
      GST_DEBUG_FUNCPTR (gst_omx_wmv_dec_prepare_frame);

  videodec_class->cdata.default_sink_template_caps = "video/x-wmv, "
      "width=(int) [1,MAX], " "height=(int) [1,MAX]";

  gst_element_class_set_static_metadata (element_class,
      "OpenMAX WMV Video Decoder",
      "Codec/Decoder/Video",
      "Decode WMV video streams",
      "Sebastian Dröge <sebastian.droege@collabora.co.uk>");

  gst_omx_set_default_role (&videodec_class->cdata, "video_decoder.wmv");
}

static void
gst_omx_wmv_dec_init (GstOMXWMVDec * self)
{
}

static gboolean
gst_omx_wmv_dec_is_format_change (GstOMXVideoDec * dec,
    GstOMXPort * port, GstVideoCodecState * state)
{
  return FALSE;
}

static gboolean
gst_omx_wmv_dec_set_format (GstOMXVideoDec * dec, GstOMXPort * port,
    GstVideoCodecState * state)
{
  gboolean ret;
  OMX_PARAM_PORTDEFINITIONTYPE port_def;

  gst_omx_port_get_port_definition (port, &port_def);
  port_def.format.video.eCompressionFormat = OMX_VIDEO_CodingWMV;
  ret = gst_omx_port_update_port_definition (port, &port_def) == OMX_ErrorNone;

  return ret;
}

static GstFlowReturn
gst_omx_wmv_dec_prepare_frame (GstOMXVideoDec * self,
    GstVideoCodecFrame * frame)
{
  GstCaps *caps;
  gboolean is_ap = FALSE;
  GstStructure *structure;
  const gchar *fourcc;

  if (self->codec_data == NULL)
    return GST_FLOW_OK;

  caps = gst_pad_get_current_caps (GST_VIDEO_DECODER_SINK_PAD (self));
  structure = gst_caps_get_structure (caps, 0);
  fourcc = gst_structure_get_string (structure, "format");
  if (fourcc) {
    if (strncmp (fourcc, "WVC1", strlen ("WVC1")) == 0) {
      GST_INFO_OBJECT (self, "stream type is Advanced Profile");
      is_ap = TRUE;
    } else {
      GST_INFO_OBJECT (self, "stream type is Simple/Main Profile");
      is_ap = FALSE;
    }
  }
  gst_caps_unref (caps);

  if (is_ap) {
    frame->input_buffer =
        gst_buffer_append (self->codec_data, frame->input_buffer);
    self->codec_data = NULL;
  } else {
    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    guint32 *SeqHdrBuf;
    guint8 *u8ptr;
    GstMapInfo info;

    gst_omx_port_get_port_definition (self->dec_in_port, &port_def);

    if (!gst_buffer_map (self->codec_data, &info, GST_MAP_READ)) {
      GST_ERROR_OBJECT (self, "Failed to create a gstbuffer mapping");
      return GST_FLOW_ERROR;
    }

    SeqHdrBuf = (guint32 *) g_malloc (SEQ_PARAM_BUF_SIZE);
    if (SeqHdrBuf == NULL) {
      GST_ERROR_OBJECT (self, "Failed to g_malloc");
      return GST_FLOW_ERROR;
    }

    /* create sequence header */
    SeqHdrBuf[0] = 0xc5000000;
    SeqHdrBuf[1] = 0x00000004;
    u8ptr = (guint8 *) & SeqHdrBuf[2];
    u8ptr[0] = info.data[0];
    u8ptr[1] = info.data[1];
    u8ptr[2] = info.data[2];
    u8ptr[3] = info.data[3];
    SeqHdrBuf[3] = port_def.format.video.nFrameHeight;
    SeqHdrBuf[4] = port_def.format.video.nFrameWidth;
    SeqHdrBuf[5] = 0x0000000c;

    gst_buffer_unmap (self->codec_data, &info);

    gst_buffer_replace (&self->codec_data, NULL);
    self->codec_data = gst_buffer_new_wrapped (SeqHdrBuf, SEQ_PARAM_BUF_SIZE);
  }

  return GST_FLOW_OK;
}
