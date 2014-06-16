/*
 * Copyright (C) 2014, Renesas Electronics Corporation
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

#include "gstomxaacdec.h"

GST_DEBUG_CATEGORY_STATIC (gst_omx_aac_dec_debug_category);
#define GST_CAT_DEFAULT gst_omx_aac_dec_debug_category

/* prototypes */
static gboolean gst_omx_aac_dec_set_format (GstOMXAudioDec * dec,
    GstCaps *caps);

/* class initialization */

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_omx_aac_dec_debug_category, "omxaacdec", 0, \
      "debug category for gst-omx audio decoder base class");

G_DEFINE_TYPE_WITH_CODE (GstOMXAACDec, gst_omx_aac_dec,
    GST_TYPE_OMX_AUDIO_DEC, DEBUG_INIT);


static void
gst_omx_aac_dec_class_init (GstOMXAACDecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstOMXAudioDecClass *audiodec_class = GST_OMX_AUDIO_DEC_CLASS (klass);

  audiodec_class->set_format = GST_DEBUG_FUNCPTR (gst_omx_aac_dec_set_format);

  audiodec_class->cdata.default_sink_template_caps = "audio/mpeg, "
      "mpegversion=(int){2, 4}, "
      "stream-format=(string){raw, adts, adif}";


  gst_element_class_set_static_metadata (element_class,
      "OpenMAX AAC Audio Decoder", "Codec/Decoder/Audio",
      "Decode AAC audio streams", "Renesas Electronics Corporation");

  gst_omx_set_default_role (&audiodec_class->cdata, "audio_decoder.aac");
}

static void
gst_omx_aac_dec_init (GstOMXAACDec * self)
{
}

static gboolean
gst_omx_aac_dec_set_format (GstOMXAudioDec * dec, GstCaps *caps)
{
  GstOMXAACDec *self = GST_OMX_AAC_DEC (dec);
  OMX_ERRORTYPE err;

  GstStructure *structure;
  OMX_AUDIO_PARAM_AACPROFILETYPE param;
  gint channels = 0;
  gint sample_rate = 0;
  gint mpegversion = 0;
  const gchar *stream_format;
  GST_OMX_INIT_STRUCT (&param);

  GST_INFO_OBJECT (self, "setcaps (sink): %" GST_PTR_FORMAT, caps);

  structure = gst_caps_get_structure (caps, 0);

  gst_structure_get_int(structure, "mpegversion", &mpegversion);
  gst_structure_get_int(structure, "channels", &channels);
  gst_structure_get_int(structure, "rate", &sample_rate);
  stream_format = gst_structure_get_string(structure, "stream-format");

  /* retrieve current in port params */
  param.nPortIndex = dec->in_port->index;
  gst_omx_component_get_parameter (dec->comp, OMX_IndexParamAudioAac, &param);

  if(channels > 0)
    param.nChannels = (OMX_U32)channels;
  if(sample_rate > 0)
    param.nSampleRate = (OMX_U32)sample_rate;
  if(!g_strcmp0(stream_format, "adif")) {
    param.eAACStreamFormat = OMX_AUDIO_AACStreamFormatADIF;
  }
  else if(!g_strcmp0(stream_format, "raw")) {
    param.eAACStreamFormat = OMX_AUDIO_AACStreamFormatRAW;
  }
  else if(!g_strcmp0(stream_format, "adts")) {
    if(mpegversion == 2)
      param.eAACStreamFormat = OMX_AUDIO_AACStreamFormatMP2ADTS;
    else if(mpegversion == 4)
      param.eAACStreamFormat = OMX_AUDIO_AACStreamFormatMP4ADTS;
  }
  err = gst_omx_component_set_parameter (dec->comp, OMX_IndexParamAudioAac, &param);
  if (err == OMX_ErrorNone)
    return TRUE;
  else return FALSE;
}
