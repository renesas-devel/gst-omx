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

#include "gstomxh264dec.h"

GST_DEBUG_CATEGORY_STATIC (gst_omx_h264_dec_debug_category);
#define GST_CAT_DEFAULT gst_omx_h264_dec_debug_category

/* prototypes */
static gboolean gst_omx_h264_dec_is_format_change (GstOMXVideoDec * dec,
    GstOMXPort * port, GstVideoCodecState * state);
static gboolean gst_omx_h264_dec_set_format (GstOMXVideoDec * dec,
    GstOMXPort * port, GstVideoCodecState * state);

enum
{
  PROP_0
};

/* class initialization */

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_omx_h264_dec_debug_category, "omxh264dec", 0, \
      "debug category for gst-omx video decoder base class");

G_DEFINE_TYPE_WITH_CODE (GstOMXH264Dec, gst_omx_h264_dec,
    GST_TYPE_OMX_VIDEO_DEC, DEBUG_INIT);

static void
gst_omx_h264_dec_class_init (GstOMXH264DecClass * klass)
{
  GstOMXVideoDecClass *videodec_class = GST_OMX_VIDEO_DEC_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  videodec_class->is_format_change =
      GST_DEBUG_FUNCPTR (gst_omx_h264_dec_is_format_change);
  videodec_class->set_format = GST_DEBUG_FUNCPTR (gst_omx_h264_dec_set_format);

  videodec_class->cdata.default_sink_template_caps = "video/x-h264, "
      "parsed=(boolean) true, "
      "alignment=(string) au, "
      "stream-format=(string) byte-stream, "
      "width=(int) [1,MAX], " "height=(int) [1,MAX]";

  gst_element_class_set_static_metadata (element_class,
      "OpenMAX H.264 Video Decoder",
      "Codec/Decoder/Video",
      "Decode H.264 video streams",
      "Sebastian Dröge <sebastian.droege@collabora.co.uk>");

  gst_omx_set_default_role (&videodec_class->cdata, "video_decoder.avc");
}

static void
gst_omx_h264_dec_init (GstOMXH264Dec * self)
{
}

static gboolean
gst_omx_h264_dec_is_format_change (GstOMXVideoDec * dec,
    GstOMXPort * port, GstVideoCodecState * state)
{
  return FALSE;
}

static GstBuffer *
gst_omx_h264_dec_retrieve_sps_pps (GstOMXH264Dec * self, guint8 * data)
{
  guint8 *ptr, *outbuf, *dest;
  guint sps_num, pps_num;
  guint *sps_size_list, *pps_size_list;
  guint sps_pps_size = 0;
  guint i;

  ptr = data;

  sps_num = ptr[5] & 0x1f;      /* reserved(3bit) + numOfSequenceParameterSets(uint 5bit) */

  sps_size_list = g_malloc (sps_num);
  if (!sps_size_list) {
    GST_ERROR_OBJECT (self, "failed g_malloc");
    return NULL;
  }

  ptr += 6;

  for (i = 0; i < sps_num; i++) {
    sps_size_list[i] = GST_READ_UINT16_BE (ptr);
    ptr += sps_size_list[i] + 2;
    sps_pps_size += sps_size_list[i] + 4;       /* take account of the start code length */
  }

  pps_num = *ptr++;             /* numOfPictureParameterSets (unint 8bit) */
  pps_size_list = g_malloc (pps_num);
  if (!pps_size_list) {
    GST_ERROR_OBJECT (self, "failed g_malloc");
    g_free (sps_size_list);
    return NULL;
  }

  for (i = 0; i < pps_num; i++) {
    pps_size_list[i] = GST_READ_UINT16_BE (ptr);
    ptr += pps_size_list[i] + 2;
    sps_pps_size += pps_size_list[i] + 4;       /* take account of the start code length */
  }

  outbuf = g_malloc (sps_pps_size);
  if (!outbuf) {
    GST_ERROR_OBJECT (self, "failed g_malloc");
    g_free (sps_size_list);
    g_free (pps_size_list);
    return NULL;
  }
  dest = outbuf;

  /* reset ptr */
  ptr = data;

  /* jump to sps data */
  ptr += 8;
  for (i = 0; i < sps_num; i++) {
    dest[0] = 0x00;
    dest[1] = 0x00;
    dest[2] = 0x00;
    dest[3] = 0x01;
    memcpy (dest + 4, ptr, sps_size_list[i]);
    dest += sps_size_list[i] + 4;
    ptr += sps_size_list[i] + 2;
  }

  /* jump to pps data */
  ptr++;
  for (i = 0; i < pps_num; i++) {
    dest[0] = 0x00;
    dest[1] = 0x00;
    dest[2] = 0x00;
    dest[3] = 0x01;
    memcpy (dest + 4, ptr, pps_size_list[i]);
    dest += pps_size_list[i] + 4;
    ptr += pps_size_list[i] + 2;
  }

  g_free (sps_size_list);
  g_free (pps_size_list);

  return gst_buffer_new_wrapped (outbuf, sps_pps_size);
}

static gboolean
gst_omx_h264_dec_set_format (GstOMXVideoDec * dec, GstOMXPort * port,
    GstVideoCodecState * state)
{
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  GstOMXH264Dec *self = GST_OMX_H264_DEC (dec);
  OMXR_MC_VIDEO_PARAM_STREAM_STORE_UNITTYPE param;
  OMX_ERRORTYPE err;
  GstMapInfo map = GST_MAP_INFO_INIT;
  GstBuffer *new_codec_data;

  gst_omx_port_get_port_definition (port, &port_def);
  port_def.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
  err = gst_omx_port_update_port_definition (port, &port_def);
  if (err != OMX_ErrorNone)
    return FALSE;

  GST_OMX_INIT_STRUCT (&param);
  param.nPortIndex = GST_OMX_VIDEO_DEC (self)->dec_in_port->index;

  err = gst_omx_component_get_parameter (GST_OMX_VIDEO_DEC (self)->dec,
      OMXR_MC_IndexParamVideoStreamStoreUnit, &param);
  if (err != OMX_ErrorNone) {
    GST_WARNING_OBJECT (self,
        "VideoStreamStoreUnit is not supported by component");
    return TRUE;
  }

  param.eStoreUnit = OMXR_MC_VIDEO_StoreUnitTimestampSeparated;
  err = gst_omx_component_set_parameter (GST_OMX_VIDEO_DEC (self)->dec,
      OMXR_MC_IndexParamVideoStreamStoreUnit, &param);
  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self,
        "Error setting VideoStreamStoreUnit StoreUnitTimestampSeparated");
    return FALSE;
  }

  gst_buffer_map (state->codec_data, &map, GST_MAP_READ);

  /* Get the nal length field size from lengthSizeMinusOne field,
   * which is contained in AVC Configuration.
   */
  self->nal_length_field_size = (map.data[4] & 0x03) + 1;

  /* Retrieve sps and pps nals from codec_data, transformed into bytestream */
  new_codec_data = gst_omx_h264_dec_retrieve_sps_pps (self, map.data);
  if (!new_codec_data) {
    GST_ERROR_OBJECT (self,
        "failed sps and pps nals retrieval from codec_data");
    gst_buffer_unmap (state->codec_data, &map);
    return FALSE;
  }

  gst_buffer_unmap (state->codec_data, &map);

  gst_buffer_replace (&state->codec_data, new_codec_data);

  return TRUE;
}
