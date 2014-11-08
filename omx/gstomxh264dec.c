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
#include "OMXR_Extension_h264d.h"
#include "OMXR_Extension_vdcmn.h"

GST_DEBUG_CATEGORY_STATIC (gst_omx_h264_dec_debug_category);
#define GST_CAT_DEFAULT gst_omx_h264_dec_debug_category

/* prototypes */
static gboolean gst_omx_h264_dec_is_format_change (GstOMXVideoDec * dec,
    GstOMXPort * port, GstVideoCodecState * state);
static gboolean gst_omx_h264_dec_set_format (GstOMXVideoDec * dec,
    GstOMXPort * port, GstVideoCodecState * state);
static gsize gst_omx_h264_dec_copy_frame (GstOMXVideoDec * dec,
    GstBuffer * inbuf, guint offset, GstOMXBuffer * outbuf);

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
  videodec_class->copy_frame = GST_DEBUG_FUNCPTR (gst_omx_h264_dec_copy_frame);

  videodec_class->cdata.default_sink_template_caps = "video/x-h264, "
      "alignment=(string) au, "
      "stream-format=(string) avc, "
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
  OMX_ERRORTYPE err;
  GstMapInfo map = GST_MAP_INFO_INIT;
  GstBuffer *new_codec_data;

  gst_omx_port_get_port_definition (port, &port_def);
  port_def.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
  err = gst_omx_port_update_port_definition (port, &port_def);
  if (err != OMX_ErrorNone)
    return FALSE;

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

  {
    /*
     * Setting store unit mode (input port only)
     *
     * Can set:
     *
     *   OMXR_MC_VIDEO_StoreUnitEofSeparated    (default) :
     *     Each OMX buffer sent to input port will contains a frame data
     *      (many NALs, each NAL must have start code)
     *
     *   OMXR_MC_VIDEO_StoreUnitTimestampSeparated        :
     *     Each OMX buffer sent to input port will contains a NAL data
     *      (without or without start code)
     */
    OMXR_MC_VIDEO_PARAM_STREAM_STORE_UNITTYPE sStore;
    GST_OMX_INIT_STRUCT (&sStore);
    sStore.nPortIndex = dec->dec_in_port->index;

    sStore.eStoreUnit = OMXR_MC_VIDEO_StoreUnitEofSeparated;  /* default */
    gst_omx_component_set_parameter
      (dec->dec, OMXR_MC_IndexParamVideoStreamStoreUnit, &sStore);


    /*
     * Setting reorder mode (output port only)
     */
    OMXR_MC_VIDEO_PARAM_REORDERTYPE sReorder;
    GST_OMX_INIT_STRUCT (&sReorder);
    sReorder.nPortIndex = dec->dec_out_port->index;  /* default */

    sReorder.bReorder = OMX_TRUE;
    gst_omx_component_set_parameter
      (dec->dec, OMXR_MC_IndexParamVideoReorder, &sReorder);


    /*
     * Setting de-interlace mode (output port only)
     */
    OMXR_MC_VIDEO_PARAM_DEINTERLACE_MODETYPE sDeinterlace;
    GST_OMX_INIT_STRUCT (&sDeinterlace);
    sDeinterlace.nPortIndex = dec->dec_out_port->index;

    sDeinterlace.eDeinterlace = OMXR_MC_VIDEO_Deinterlace3DHalf; /* default */
    gst_omx_component_set_parameter
      (dec->dec, OMXR_MC_IndexParamVideoDeinterlaceMode, &sDeinterlace);
  }

  return TRUE;
}

static gsize
gst_omx_h264_dec_get_nal_size (GstOMXH264Dec * self, guint8 * buf)
{
  gsize nal_size = 0;
  gint i;

  for (i = 0; i < self->nal_length_field_size; i++)
    nal_size = (nal_size << 8) | buf[i];

  return nal_size;
}

static gsize
gst_omx_h264_dec_copy_frame (GstOMXVideoDec * dec, GstBuffer * inbuf,
    guint offset, GstOMXBuffer * outbuf)
{
  GstOMXH264Dec *self = GST_OMX_H264_DEC (dec);
  gsize inbuf_size, nal_size, outbuf_size, output_amount = 0,
      inbuf_consumed = 0;
  GstMapInfo map = GST_MAP_INFO_INIT;
  guint8 *in_data, *out_data;

  gst_buffer_map (inbuf, &map, GST_MAP_READ);

  /* Transform AVC format into bytestream and copy frames */
  in_data = map.data + offset;
  out_data = outbuf->omx_buf->pBuffer + outbuf->omx_buf->nOffset;
  inbuf_size = gst_buffer_get_size (inbuf) - offset;
  outbuf_size = outbuf->omx_buf->nAllocLen - outbuf->omx_buf->nOffset;
  nal_size = gst_omx_h264_dec_get_nal_size (self, in_data);
  while (output_amount + nal_size + 4 <= outbuf_size) {
    guint inbuf_to_next, outbuf_to_next;

    out_data[0] = 0x00;
    out_data[1] = 0x00;
    out_data[2] = 0x00;
    out_data[3] = 0x01;

    memcpy (out_data + 4, in_data + self->nal_length_field_size, nal_size);

    outbuf_to_next = nal_size + 4;
    out_data += outbuf_to_next;
    output_amount += outbuf_to_next;

    inbuf_to_next = nal_size + self->nal_length_field_size;
    inbuf_consumed += inbuf_to_next;
    if ((inbuf_size - inbuf_consumed) < self->nal_length_field_size)
      /* the end of an input buffer */
      break;

    in_data += inbuf_to_next;

    nal_size = gst_omx_h264_dec_get_nal_size (self, in_data);
  }

  gst_buffer_unmap (inbuf, &map);

  outbuf->omx_buf->nFilledLen = output_amount;

  return inbuf_consumed;
}
