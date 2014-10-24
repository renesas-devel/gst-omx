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
#include <string.h>

#include "gstomxaudiodec.h"

GST_DEBUG_CATEGORY_STATIC (gst_omx_audio_dec_debug_category);
#define GST_CAT_DEFAULT gst_omx_audio_dec_debug_category

/* prototypes */
static void gst_omx_audio_dec_finalize (GObject * object);

static GstStateChangeReturn
gst_omx_audio_dec_change_state (GstElement * element,
    GstStateChange transition);

static gboolean gst_omx_audio_dec_start (GstAudioDecoder * decoder);
static gboolean gst_omx_audio_dec_stop (GstAudioDecoder * decoder);
static gboolean gst_omx_audio_dec_set_format (GstAudioDecoder * decoder,
    GstCaps *caps);
static gboolean gst_omx_audio_dec_sink_event (GstAudioDecoder * decoder,
    GstEvent * event);
static GstFlowReturn gst_omx_audio_dec_handle_frame (GstAudioDecoder *
    decoder, GstBuffer * buffer);
static void gst_omx_audio_dec_flush (GstAudioDecoder * decoder, gboolean hard);

static GstFlowReturn gst_omx_audio_dec_drain (GstOMXAudioDec * self);

enum
{
  PROP_0
};

/* class initialization */

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_omx_audio_dec_debug_category, "omxaudiodec", 0, \
      "debug category for gst-omx audio decoder base class");

G_DEFINE_ABSTRACT_TYPE_WITH_CODE (GstOMXAudioDec, gst_omx_audio_dec,
    GST_TYPE_AUDIO_DECODER, DEBUG_INIT);

static void
gst_omx_audio_dec_class_init (GstOMXAudioDecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstAudioDecoderClass *audio_decoder_class = GST_AUDIO_DECODER_CLASS (klass);

  gobject_class->finalize = gst_omx_audio_dec_finalize;

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_omx_audio_dec_change_state);

  audio_decoder_class->start = GST_DEBUG_FUNCPTR (gst_omx_audio_dec_start);
  audio_decoder_class->stop = GST_DEBUG_FUNCPTR (gst_omx_audio_dec_stop);
  audio_decoder_class->flush = GST_DEBUG_FUNCPTR (gst_omx_audio_dec_flush);
  audio_decoder_class->set_format =
      GST_DEBUG_FUNCPTR (gst_omx_audio_dec_set_format);
  audio_decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_omx_audio_dec_handle_frame);
  audio_decoder_class->sink_event =
      GST_DEBUG_FUNCPTR (gst_omx_audio_dec_sink_event);

  klass->cdata.default_src_template_caps = "audio/x-raw, "
      "rate = (int) [ 1, MAX ], "
      "channels = (int) [ 1, " G_STRINGIFY (OMX_AUDIO_MAXCHANNELS) " ], "
      "format = (string) { S8, U8, S16LE, S16BE, U16LE, U16BE, "
      "S24LE, S24BE, U24LE, U24BE, S32LE, S32BE, U32LE, U32BE }";
}

static void
gst_omx_audio_dec_init (GstOMXAudioDec * self)
{
  g_mutex_init (&self->drain_lock);
  g_cond_init (&self->drain_cond);
}

static gboolean
gst_omx_audio_dec_open (GstOMXAudioDec * self)
{
  GstOMXAudioDecClass *klass = GST_OMX_AUDIO_DEC_GET_CLASS (self);
  gint in_port_index, out_port_index;

  self->comp =
      gst_omx_component_new (GST_OBJECT_CAST (self), klass->cdata.core_name,
      klass->cdata.component_name, klass->cdata.component_role,
      klass->cdata.hacks);
  self->started = FALSE;

  if (!self->comp)
    return FALSE;

  if (gst_omx_component_get_state (self->comp,
          GST_CLOCK_TIME_NONE) != OMX_StateLoaded)
    return FALSE;

  in_port_index = klass->cdata.in_port_index;
  out_port_index = klass->cdata.out_port_index;

  if (in_port_index == -1 || out_port_index == -1) {
    OMX_PORT_PARAM_TYPE param;
    OMX_ERRORTYPE err;

    GST_OMX_INIT_STRUCT (&param);

    err =
        gst_omx_component_get_parameter (self->comp, OMX_IndexParamAudioInit,
        &param);
    if (err != OMX_ErrorNone) {
      GST_WARNING_OBJECT (self, "Couldn't get port information: %s (0x%08x)",
          gst_omx_error_to_string (err), err);
      /* Fallback */
      in_port_index = 0;
      out_port_index = 1;
    } else {
      GST_DEBUG_OBJECT (self, "Detected %u ports, starting at %u", param.nPorts,
          param.nStartPortNumber);
      in_port_index = param.nStartPortNumber + 0;
      out_port_index = param.nStartPortNumber + 1;
    }
  }

  self->in_port = gst_omx_component_add_port (self->comp, in_port_index);
  self->out_port = gst_omx_component_add_port (self->comp, out_port_index);

  if (!self->in_port || !self->out_port)
    return FALSE;

  return TRUE;
}


static gboolean
gst_omx_audio_dec_shutdown (GstOMXAudioDec * self)
{
  OMX_STATETYPE state;

  GST_DEBUG_OBJECT (self, "Shutting down decoder");

  state = gst_omx_component_get_state (self->comp, 0);
  if (state > OMX_StateLoaded || state == OMX_StateInvalid) {
    if (state > OMX_StateIdle) {
      gst_omx_component_set_state (self->comp, OMX_StateIdle);
      gst_omx_component_get_state (self->comp, 5 * GST_SECOND);
    }
    gst_omx_component_set_state (self->comp, OMX_StateLoaded);
    gst_omx_port_deallocate_buffers (self->in_port);
    gst_omx_port_deallocate_buffers (self->out_port);
    if (state > OMX_StateLoaded)
      gst_omx_component_get_state (self->comp, 5 * GST_SECOND);
  }

  return TRUE;
}

static gboolean
gst_omx_audio_dec_close (GstOMXAudioDec * self)
{
  GST_DEBUG_OBJECT (self, "Closing decoder");

  if (!gst_omx_audio_dec_shutdown (self))
    return FALSE;

  self->in_port = NULL;
  self->out_port = NULL;
  if (self->comp)
    gst_omx_component_free (self->comp);
  self->comp = NULL;

  return TRUE;
}

static void
gst_omx_audio_dec_finalize (GObject * object)
{
  GstOMXAudioDec *self = GST_OMX_AUDIO_DEC (object);

  g_mutex_clear (&self->drain_lock);
  g_cond_clear (&self->drain_cond);

  G_OBJECT_CLASS (gst_omx_audio_dec_parent_class)->finalize (object);
}

static GstStateChangeReturn
gst_omx_audio_dec_change_state (GstElement * element, GstStateChange transition)
{
  GstOMXAudioDec *self;
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

  g_return_val_if_fail (GST_IS_OMX_AUDIO_DEC (element),
      GST_STATE_CHANGE_FAILURE);
  self = GST_OMX_AUDIO_DEC (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!gst_omx_audio_dec_open (self))
        ret = GST_STATE_CHANGE_FAILURE;
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      self->downstream_flow_ret = GST_FLOW_OK;

      self->draining = FALSE;
      self->started = FALSE;
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      if (self->in_port)
        gst_omx_port_set_flushing (self->in_port, 5 * GST_SECOND, TRUE);
      if (self->out_port)
        gst_omx_port_set_flushing (self->out_port, 5 * GST_SECOND, TRUE);

      g_mutex_lock (&self->drain_lock);
      self->draining = FALSE;
      g_cond_broadcast (&self->drain_cond);
      g_mutex_unlock (&self->drain_lock);
      break;
    default:
      break;
  }

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  ret =
      GST_ELEMENT_CLASS (gst_omx_audio_dec_parent_class)->change_state (element,
      transition);

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      self->downstream_flow_ret = GST_FLOW_FLUSHING;
      self->started = FALSE;

      if (!gst_omx_audio_dec_shutdown (self))
        ret = GST_STATE_CHANGE_FAILURE;
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      if (!gst_omx_audio_dec_close (self))
        ret = GST_STATE_CHANGE_FAILURE;
      break;
    default:
      break;
  }

  return ret;
}

static void
gst_omx_audio_dec_loop (GstOMXAudioDec * self)
{
  GstOMXAudioDecClass *klass;
  GstOMXPort *port = self->out_port;
  GstOMXBuffer *buf = NULL;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  GstOMXAcquireBufferReturn acq_return;
  OMX_ERRORTYPE err;

    GstAudioInfo *info =
        gst_audio_decoder_get_audio_info (GST_AUDIO_DECODER (self));

  klass = GST_OMX_AUDIO_DEC_GET_CLASS (self);

  acq_return = gst_omx_port_acquire_buffer (port, &buf);
  if (acq_return == GST_OMX_ACQUIRE_BUFFER_ERROR) {
    goto component_error;
  } else if (acq_return == GST_OMX_ACQUIRE_BUFFER_FLUSHING) {
    goto flushing;
  } else if (acq_return == GST_OMX_ACQUIRE_BUFFER_EOS) {
    goto eos;
  }

  if (!gst_pad_has_current_caps (GST_AUDIO_DECODER_SRC_PAD (self))
      || acq_return == GST_OMX_ACQUIRE_BUFFER_RECONFIGURE) {
    GstCaps *caps;

    GST_DEBUG_OBJECT (self, "Port settings have changed");

    /* Reallocate all buffers */
    if (acq_return == GST_OMX_ACQUIRE_BUFFER_RECONFIGURE) {
      err = gst_omx_port_set_enabled (port, FALSE);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_wait_buffers_released (port, 5 * GST_SECOND);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_deallocate_buffers (port);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_wait_enabled (port, 1 * GST_SECOND);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

    }

    GST_AUDIO_DECODER_STREAM_LOCK (self);

    /* Check the setting of MC for caps */
    {
      OMX_AUDIO_PARAM_PCMMODETYPE pcm_param;
      OMX_ERRORTYPE err;
      GstAudioFormat  format;
      gint sign, endian, i;


      /* Use this array to map channel position from OMX define
       * to GStreamer define. In OMX it is
       *     OMX_AUDIO_ChannelNone = 0x0,    < Unused or empty >
       *     OMX_AUDIO_ChannelLF   = 0x1,    < Left front >
       *     OMX_AUDIO_ChannelRF   = 0x2,    < Right front >
       *     OMX_AUDIO_ChannelCF   = 0x3,    < Center front >
       *     OMX_AUDIO_ChannelLS   = 0x4,    < Left surround >
       *     OMX_AUDIO_ChannelRS   = 0x5,    < Right surround >
       *     OMX_AUDIO_ChannelLFE  = 0x6,    < Low frequency effects >
       *     OMX_AUDIO_ChannelCS   = 0x7,    < Back surround >
       *     OMX_AUDIO_ChannelLR   = 0x8,    < Left rear. >
       *     OMX_AUDIO_ChannelRR   = 0x9,    < Right rear. >
       * The corresponding channels in GStreamer are (in same order) :*/
      static const GstAudioChannelPosition map_omx_channel_to_gst[] = {
          GST_AUDIO_CHANNEL_POSITION_NONE,
          GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
          GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
          GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
          GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
          GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
          GST_AUDIO_CHANNEL_POSITION_LFE1,
          GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
          GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
          GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT
      };

      GST_OMX_INIT_STRUCT (&pcm_param);
      pcm_param.nPortIndex = self->out_port->index;
      err = gst_omx_component_get_parameter (self->comp,
              OMX_IndexParamAudioPcm, &pcm_param);

      if (err == OMX_ErrorNone) {
        GST_DEBUG_OBJECT (self, "Generate format with channels=%d, "
        "rate=%d, bps=%d, endian=%d",
        pcm_param.nChannels, pcm_param.nSamplingRate,
        pcm_param.nBitPerSample, pcm_param.eEndian);

        info->channels = pcm_param.nChannels;
        info->rate = pcm_param.nSamplingRate;
        info->bpf = pcm_param.nBitPerSample * info->channels;

        for (i = 0; i < info->channels; i++)
          info->position[i] =
                map_omx_channel_to_gst[pcm_param.eChannelMapping[i]];

        if (pcm_param.bInterleaved == OMX_TRUE)
          info->layout = GST_AUDIO_LAYOUT_INTERLEAVED;
        else info->layout = GST_AUDIO_LAYOUT_NON_INTERLEAVED;

        if (pcm_param.eNumData == OMX_NumericalDataSigned)
          sign = TRUE;
        else sign = FALSE;
        if (pcm_param.eEndian == OMX_EndianLittle)
          endian = G_LITTLE_ENDIAN;
        else endian = G_BIG_ENDIAN;

        format = gst_audio_format_build_integer(sign, endian,
                      pcm_param.nBitPerSample, pcm_param.nBitPerSample);

        if (format != GST_AUDIO_FORMAT_UNKNOWN) {
          info->finfo = gst_audio_format_get_info (format);

          caps = gst_audio_info_to_caps (info);

          GST_DEBUG_OBJECT (self, "format=%d, caps = %"GST_PTR_FORMAT, format, caps);
        }
      }
    }

    GST_DEBUG_OBJECT (self, "Setting output caps: %" GST_PTR_FORMAT, caps);

    if (!gst_pad_set_caps (GST_AUDIO_DECODER_SRC_PAD (self), caps)) {
      gst_caps_unref (caps);
      if (buf)
        gst_omx_port_release_buffer (self->out_port, buf);
      GST_AUDIO_DECODER_STREAM_UNLOCK (self);
      goto caps_failed;
    }
    gst_caps_unref (caps);

    GST_AUDIO_DECODER_STREAM_UNLOCK (self);

    if (acq_return == GST_OMX_ACQUIRE_BUFFER_RECONFIGURE) {
      err = gst_omx_port_set_enabled (port, TRUE);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_allocate_buffers (port);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_wait_enabled (port, 5 * GST_SECOND);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_populate (port);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_mark_reconfigured (port);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;
    }

    /* Now get a buffer */
    if (acq_return != GST_OMX_ACQUIRE_BUFFER_OK)
      return;
  }

  g_assert (acq_return == GST_OMX_ACQUIRE_BUFFER_OK);
  if (!buf) {
    g_assert ((klass->cdata.hacks & GST_OMX_HACK_NO_EMPTY_EOS_BUFFER));
    GST_AUDIO_DECODER_STREAM_LOCK (self);
    goto eos;
  }

  GST_DEBUG_OBJECT (self, "Handling buffer: 0x%08x %lu", buf->omx_buf->nFlags,
      buf->omx_buf->nTimeStamp);

  /* This prevents a deadlock between the srcpad stream
   * lock and the videocodec stream lock, if ::reset()
   * is called at the wrong time
   */
  if (gst_omx_port_is_flushing (self->out_port)) {
    GST_DEBUG_OBJECT (self, "Flushing");
    gst_omx_port_release_buffer (self->out_port, buf);
    goto flushing;
  }

  GST_AUDIO_DECODER_STREAM_LOCK (self);

  if ((buf->omx_buf->nFlags & OMX_BUFFERFLAG_CODECCONFIG)
      && buf->omx_buf->nFilledLen > 0) {
    GstCaps *caps;
    GstBuffer *codec_data;
    GstMapInfo map = GST_MAP_INFO_INIT;

    GST_DEBUG_OBJECT (self, "Handling codec data");
    caps =
        gst_caps_copy (gst_pad_get_current_caps (GST_AUDIO_DECODER_SRC_PAD
            (self)));
    codec_data = gst_buffer_new_and_alloc (buf->omx_buf->nFilledLen);

    gst_buffer_map (codec_data, &map, GST_MAP_WRITE);
    memcpy (map.data,
        buf->omx_buf->pBuffer + buf->omx_buf->nOffset,
        buf->omx_buf->nFilledLen);
    gst_buffer_unmap (codec_data, &map);

    gst_caps_set_simple (caps, "codec_data", GST_TYPE_BUFFER, codec_data, NULL);
    if (!gst_pad_set_caps (GST_AUDIO_DECODER_SRC_PAD (self), caps)) {
      gst_caps_unref (caps);
      if (buf)
        gst_omx_port_release_buffer (self->out_port, buf);
      GST_AUDIO_DECODER_STREAM_UNLOCK (self);
      goto caps_failed;
    }
    gst_caps_unref (caps);
    flow_ret = GST_FLOW_OK;
  } else if (buf->omx_buf->nFilledLen > 0) {
    GstBuffer *outbuf;
    guint n_samples;
    GstMapInfo map = GST_MAP_INFO_INIT;

    GST_DEBUG_OBJECT (self, "Handling output data, filled len = %d", buf->omx_buf->nFilledLen);

    n_samples = 1;

    outbuf = gst_buffer_new_and_alloc (buf->omx_buf->nFilledLen);

    gst_buffer_map (outbuf, &map, GST_MAP_WRITE);

    memcpy (map.data,
        buf->omx_buf->pBuffer + buf->omx_buf->nOffset,
        buf->omx_buf->nFilledLen);
    gst_buffer_unmap (outbuf, &map);

    GST_BUFFER_TIMESTAMP (outbuf) = buf->omx_buf->nTimeStamp;
    GST_BUFFER_DURATION (outbuf) = GST_CLOCK_TIME_NONE;


    flow_ret =
        gst_audio_decoder_finish_frame (GST_AUDIO_DECODER (self),
        outbuf, n_samples);
  }

  GST_DEBUG_OBJECT (self, "Finished frame: %s", gst_flow_get_name (flow_ret));

  err = gst_omx_port_release_buffer (port, buf);
  if (err != OMX_ErrorNone)
    goto release_error;

  self->downstream_flow_ret = flow_ret;

  if (flow_ret != GST_FLOW_OK)
    goto flow_error;

  GST_AUDIO_DECODER_STREAM_UNLOCK (self);

  return;

component_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("OpenMAX component in error state %s (0x%08x)",
            gst_omx_component_get_last_error_string (self->comp),
            gst_omx_component_get_last_error (self->comp)));
    gst_pad_push_event (GST_AUDIO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_AUDIO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    self->started = FALSE;

    return;
  }
flushing:
  {
    GST_DEBUG_OBJECT (self, "Flushing -- stopping task");
    gst_pad_pause_task (GST_AUDIO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_FLUSHING;
    self->started = FALSE;

    return;
  }
eos:
  {
    g_mutex_lock (&self->drain_lock);
    if (self->draining) {
      GST_DEBUG_OBJECT (self, "Drained");
      self->draining = FALSE;
      g_cond_broadcast (&self->drain_cond);
      flow_ret = GST_FLOW_OK;
      gst_pad_pause_task (GST_AUDIO_DECODER_SRC_PAD (self));
    } else {
      GST_DEBUG_OBJECT (self, "Component signalled EOS");
      flow_ret = GST_FLOW_EOS;
    }
    g_mutex_unlock (&self->drain_lock);
    self->downstream_flow_ret = flow_ret;

    /* Here we fallback and pause the task for the EOS case */
    if (flow_ret != GST_FLOW_OK)
      goto flow_error;

    GST_AUDIO_DECODER_STREAM_UNLOCK (self);

    return;
  }
flow_error:
  {
    if (flow_ret == GST_FLOW_EOS) {
      GST_DEBUG_OBJECT (self, "EOS");

      gst_pad_push_event (GST_AUDIO_DECODER_SRC_PAD (self),
          gst_event_new_eos ());
      gst_pad_pause_task (GST_AUDIO_DECODER_SRC_PAD (self));
    } else if (flow_ret == GST_FLOW_NOT_LINKED || flow_ret < GST_FLOW_EOS) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Internal data stream error."),
          ("stream stopped, reason %s", gst_flow_get_name (flow_ret)));

      gst_pad_push_event (GST_AUDIO_DECODER_SRC_PAD (self),
          gst_event_new_eos ());
      gst_pad_pause_task (GST_AUDIO_DECODER_SRC_PAD (self));
    }
    self->started = FALSE;
    GST_AUDIO_DECODER_STREAM_UNLOCK (self);

    return;
  }
reconfigure_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Unable to reconfigure output port"));
    gst_pad_push_event (GST_AUDIO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_AUDIO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_NOT_NEGOTIATED;
    self->started = FALSE;

    return;
  }
caps_failed:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL), ("Failed to set caps"));
    gst_pad_push_event (GST_AUDIO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_AUDIO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_NOT_NEGOTIATED;
    self->started = FALSE;

    return;
  }
release_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Failed to relase output buffer to component: %s (0x%08x)",
            gst_omx_error_to_string (err), err));
    gst_pad_push_event (GST_AUDIO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_AUDIO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    self->started = FALSE;
    GST_AUDIO_DECODER_STREAM_UNLOCK (self);

    return;
  }
}

static gboolean
gst_omx_audio_dec_start (GstAudioDecoder * decoder)
{
  GstOMXAudioDec *self;

  self = GST_OMX_AUDIO_DEC (decoder);

  self->last_upstream_ts = 0;
  self->eos = FALSE;
  self->downstream_flow_ret = GST_FLOW_OK;

  return TRUE;
}

static gboolean
gst_omx_audio_dec_stop (GstAudioDecoder * decoder)
{
  GstOMXAudioDec *self;

  self = GST_OMX_AUDIO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Stopping decoder");

  gst_omx_port_set_flushing (self->in_port, 5 * GST_SECOND, TRUE);
  gst_omx_port_set_flushing (self->out_port, 5 * GST_SECOND, TRUE);

  gst_pad_stop_task (GST_AUDIO_DECODER_SRC_PAD (decoder));

  if (gst_omx_component_get_state (self->comp, 0) > OMX_StateIdle)
    gst_omx_component_set_state (self->comp, OMX_StateIdle);

  self->downstream_flow_ret = GST_FLOW_FLUSHING;
  self->started = FALSE;
  self->eos = FALSE;

  g_mutex_lock (&self->drain_lock);
  self->draining = FALSE;
  g_cond_broadcast (&self->drain_cond);
  g_mutex_unlock (&self->drain_lock);

  gst_omx_component_get_state (self->comp, 5 * GST_SECOND);

  return TRUE;
}

static gboolean
gst_omx_audio_dec_set_format (GstAudioDecoder * decoder, GstCaps *caps)
{
  GstOMXAudioDec *self;
  GstOMXAudioDecClass *klass;
  gboolean needs_disable = FALSE;
  OMX_PARAM_PORTDEFINITIONTYPE port_def;

  self = GST_OMX_AUDIO_DEC (decoder);
  klass = GST_OMX_AUDIO_DEC_GET_CLASS (decoder);

  GST_DEBUG_OBJECT (self, "Setting new caps");

  gst_omx_port_get_port_definition (self->in_port, &port_def);

  needs_disable =
      gst_omx_component_get_state (self->comp,
      GST_CLOCK_TIME_NONE) != OMX_StateLoaded;
  /* If the component is not in Loaded state and a real format change happens
   * we have to disable the port and re-allocate all buffers. If no real
   * format change happened we can just exit here.
   */
  if (needs_disable) {
    GST_DEBUG_OBJECT (self, "Need to disable and drain decoder");
    gst_omx_audio_dec_drain (self);
    gst_omx_port_set_flushing (self->out_port, 5 * GST_SECOND, TRUE);

    /* Wait until the srcpad loop is finished,
     * unlock GST_AUDIO_DECODER_STREAM_LOCK to prevent deadlocks
     * caused by using this lock from inside the loop function */
    GST_AUDIO_DECODER_STREAM_UNLOCK (self);
    gst_pad_stop_task (GST_AUDIO_DECODER_SRC_PAD (decoder));
    GST_AUDIO_DECODER_STREAM_LOCK (self);

    if (gst_omx_port_set_enabled (self->in_port, FALSE) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_port_set_enabled (self->out_port, FALSE) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_port_wait_buffers_released (self->in_port,
            5 * GST_SECOND) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_port_wait_buffers_released (self->out_port,
            1 * GST_SECOND) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_port_deallocate_buffers (self->in_port) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_port_deallocate_buffers (self->out_port) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_port_wait_enabled (self->in_port,
            1 * GST_SECOND) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_port_wait_enabled (self->out_port,
            1 * GST_SECOND) != OMX_ErrorNone)
      return FALSE;

    GST_DEBUG_OBJECT (self, "Decoder drained and disabled");
  }


  if (klass->set_format) {
    if (!klass->set_format (self, caps)) {
      GST_ERROR_OBJECT (self, "Subclass failed to set the new format");
      return FALSE;
    }
  }

  GST_DEBUG_OBJECT (self, "Updating outport port definition");
  if (gst_omx_port_update_port_definition (self->out_port,
          NULL) != OMX_ErrorNone)
    return FALSE;


  port_def.format.audio.eEncoding = OMX_AUDIO_CodingPCM;
  GST_DEBUG_OBJECT (self, "Setting outport port definition");
  if (gst_omx_port_update_port_definition (self->out_port,
          &port_def) != OMX_ErrorNone)
    return FALSE;

  GST_DEBUG_OBJECT (self, "Enabling component");
  if (needs_disable) {
    if (gst_omx_port_set_enabled (self->in_port, TRUE) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_port_allocate_buffers (self->in_port) != OMX_ErrorNone ||
        gst_omx_port_allocate_buffers (self->out_port) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_port_wait_enabled (self->in_port,
            5 * GST_SECOND) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_port_mark_reconfigured (self->in_port) != OMX_ErrorNone)
      return FALSE;
  } else {
    if (gst_omx_component_set_state (self->comp, OMX_StateIdle) != OMX_ErrorNone)
      return FALSE;

    /* Need to allocate buffers to reach Idle state */
    if (gst_omx_port_allocate_buffers (self->in_port) != OMX_ErrorNone ||
        gst_omx_port_allocate_buffers (self->out_port) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_component_get_state (self->comp,
            GST_CLOCK_TIME_NONE) != OMX_StateIdle)
      return FALSE;

    if (gst_omx_component_set_state (self->comp,
            OMX_StateExecuting) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_component_get_state (self->comp,
            GST_CLOCK_TIME_NONE) != OMX_StateExecuting)
      return FALSE;
  }

  /* Unset flushing to allow ports to accept data again */
  gst_omx_port_set_flushing (self->in_port, 5 * GST_SECOND, FALSE);
  gst_omx_port_set_flushing (self->out_port, 5 * GST_SECOND, FALSE);

  if (gst_omx_component_get_last_error (self->comp) != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self, "Component in error state: %s (0x%08x)",
        gst_omx_component_get_last_error_string (self->comp),
        gst_omx_component_get_last_error (self->comp));

    return FALSE;
  }

  /* Start the srcpad loop again */
  GST_DEBUG_OBJECT (self, "Starting task again");
  self->downstream_flow_ret = GST_FLOW_OK;
  gst_pad_start_task (GST_AUDIO_DECODER_SRC_PAD (self),
      (GstTaskFunction) gst_omx_audio_dec_loop, decoder, NULL);

  return TRUE;
}

static void
gst_omx_audio_dec_flush (GstAudioDecoder * decoder, gboolean hard)
{
  GstOMXAudioDec *self;

  self = GST_OMX_AUDIO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Flushing decoder");

  gst_omx_audio_dec_drain (self);

  gst_omx_port_set_flushing (self->in_port, 5 * GST_SECOND, TRUE);
  gst_omx_port_set_flushing (self->out_port, 5 * GST_SECOND, TRUE);

  /* Wait until the srcpad loop is finished */
  GST_AUDIO_DECODER_STREAM_UNLOCK (self);
  GST_PAD_STREAM_LOCK (GST_AUDIO_DECODER_SRC_PAD (self));
  GST_PAD_STREAM_UNLOCK (GST_AUDIO_DECODER_SRC_PAD (self));
  GST_AUDIO_DECODER_STREAM_LOCK (self);

  gst_omx_port_set_flushing (self->in_port, 5 * GST_SECOND, FALSE);
  gst_omx_port_set_flushing (self->out_port, 5 * GST_SECOND, FALSE);
  gst_omx_port_populate (self->out_port);

  /* Start the srcpad loop again */
  self->last_upstream_ts = 0;
  self->downstream_flow_ret = GST_FLOW_OK;
  self->eos = FALSE;
  gst_pad_start_task (GST_AUDIO_DECODER_SRC_PAD (self),
      (GstTaskFunction) gst_omx_audio_dec_loop, decoder, NULL);
}

static GstFlowReturn
gst_omx_audio_dec_handle_frame (GstAudioDecoder * decoder, GstBuffer * inbuf)
{
  GstOMXAcquireBufferReturn acq_ret = GST_OMX_ACQUIRE_BUFFER_ERROR;
  GstOMXAudioDec *self;
  GstOMXPort *port;
  GstOMXBuffer *buf;
  gsize size;
  guint offset = 0;
  GstClockTime timestamp, duration;
  OMX_ERRORTYPE err;

  self = GST_OMX_AUDIO_DEC (decoder);

  if (self->eos) {
    GST_WARNING_OBJECT (self, "Got frame after EOS");
    return GST_FLOW_EOS;
  }

  if (self->downstream_flow_ret != GST_FLOW_OK) {
    return self->downstream_flow_ret;
  }

  if (inbuf == NULL)
    return GST_FLOW_OK;

  GST_DEBUG_OBJECT (self, "Handling frame");

  timestamp = GST_BUFFER_TIMESTAMP (inbuf);
  duration = GST_BUFFER_DURATION (inbuf);

  port = self->in_port;

  size = gst_buffer_get_size (inbuf);
  while (offset < size) {
    /* Make sure to release the base class stream lock, otherwise
     * _loop() can't call _finish_frame() and we might block forever
     * because no input buffers are released */
    GST_AUDIO_DECODER_STREAM_UNLOCK (self);
    acq_ret = gst_omx_port_acquire_buffer (port, &buf);

    if (acq_ret == GST_OMX_ACQUIRE_BUFFER_ERROR) {
      GST_AUDIO_DECODER_STREAM_LOCK (self);
      goto component_error;
    } else if (acq_ret == GST_OMX_ACQUIRE_BUFFER_FLUSHING) {
      GST_AUDIO_DECODER_STREAM_LOCK (self);
      goto flushing;
    } else if (acq_ret == GST_OMX_ACQUIRE_BUFFER_RECONFIGURE) {
      /* Reallocate all buffers */
      err = gst_omx_port_set_enabled (port, FALSE);
      if (err != OMX_ErrorNone) {
        GST_AUDIO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_wait_buffers_released (port, 5 * GST_SECOND);
      if (err != OMX_ErrorNone) {
        GST_AUDIO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_deallocate_buffers (port);
      if (err != OMX_ErrorNone) {
        GST_AUDIO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_wait_enabled (port, 1 * GST_SECOND);
      if (err != OMX_ErrorNone) {
        GST_AUDIO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_set_enabled (port, TRUE);
      if (err != OMX_ErrorNone) {
        GST_AUDIO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_allocate_buffers (port);
      if (err != OMX_ErrorNone) {
        GST_AUDIO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_wait_enabled (port, 5 * GST_SECOND);
      if (err != OMX_ErrorNone) {
        GST_AUDIO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_mark_reconfigured (port);
      if (err != OMX_ErrorNone) {
        GST_AUDIO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      /* Now get a new buffer and fill it */
      GST_AUDIO_DECODER_STREAM_LOCK (self);
      continue;
    }
    GST_AUDIO_DECODER_STREAM_LOCK (self);

    g_assert (acq_ret == GST_OMX_ACQUIRE_BUFFER_OK && buf != NULL);

    if (self->downstream_flow_ret != GST_FLOW_OK) {
      gst_omx_port_release_buffer (port, buf);
      GST_DEBUG_OBJECT (self, "return sth ...");
      return self->downstream_flow_ret;
    }

    if (buf->omx_buf->nAllocLen - buf->omx_buf->nOffset <= 0) {
      gst_omx_port_release_buffer (port, buf);
      goto full_buffer;
    }

    /* Copy the buffer content in chunks of size as requested
     * by the port */
    buf->omx_buf->nFilledLen =
        MIN (size, buf->omx_buf->nAllocLen - buf->omx_buf->nOffset);

    gst_buffer_extract (inbuf, 0,
        buf->omx_buf->pBuffer + buf->omx_buf->nOffset,
        buf->omx_buf->nFilledLen);

    if (timestamp != GST_CLOCK_TIME_NONE) {
      buf->omx_buf->nTimeStamp =
          gst_util_uint64_scale (timestamp,
          OMX_TICKS_PER_SECOND, GST_SECOND);
      self->last_upstream_ts = timestamp;
    }
    if (duration != GST_CLOCK_TIME_NONE) {
      buf->omx_buf->nTimeStamp = self->last_upstream_ts;
      self->last_upstream_ts += duration;
    }

    offset += buf->omx_buf->nFilledLen;
    self->started = TRUE;

    err = gst_omx_port_release_buffer (port, buf);
    if (err != OMX_ErrorNone)
      goto release_error;
  }

  GST_DEBUG_OBJECT (self, "Passed frame to component");

  return self->downstream_flow_ret;

full_buffer:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("Got OpenMAX buffer with no free space (%p, %u/%u)", buf,
            buf->omx_buf->nOffset, buf->omx_buf->nAllocLen));
    return GST_FLOW_ERROR;
  }
component_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("OpenMAX component in error state %s (0x%08x)",
            gst_omx_component_get_last_error_string (self->comp),
            gst_omx_component_get_last_error (self->comp)));
    return GST_FLOW_ERROR;
  }

flushing:
  {
    GST_DEBUG_OBJECT (self, "Flushing -- returning FLUSHING");
    return GST_FLOW_FLUSHING;
  }
reconfigure_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Unable to reconfigure input port"));
    return GST_FLOW_ERROR;
  }
release_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Failed to relase input buffer to component: %s (0x%08x)",
            gst_omx_error_to_string (err), err));
    return GST_FLOW_ERROR;
  }
}

static gboolean
gst_omx_audio_dec_sink_event (GstAudioDecoder * decoder, GstEvent * event)
{
  GstOMXAudioDec *self;
  GstOMXAudioDecClass *klass;
  gboolean err;
  self = GST_OMX_AUDIO_DEC (decoder);
  klass = GST_OMX_AUDIO_DEC_GET_CLASS (self);

  err = (GST_AUDIO_DECODER_CLASS
      (gst_omx_audio_dec_parent_class)->sink_event (decoder, event));

  if (GST_EVENT_TYPE (event) == GST_EVENT_EOS) {
    GstOMXBuffer *buf;
    GstOMXAcquireBufferReturn acq_ret;

    GST_DEBUG_OBJECT (self, "Sending EOS to the component");

    /* Don't send EOS buffer twice, this doesn't work */
    if (self->eos) {
      GST_DEBUG_OBJECT (self, "Component is already EOS");
    } else {
      self->eos = TRUE;
      if (!gst_omx_audio_dec_drain (self))
        err= FALSE;
    }
  }

  return err;
}

static GstFlowReturn
gst_omx_audio_dec_drain (GstOMXAudioDec * self)
{
  GstOMXAudioDecClass *klass;
  GstOMXBuffer *buf;
  GstOMXAcquireBufferReturn acq_ret;
  OMX_ERRORTYPE err;

  GST_DEBUG_OBJECT (self, "Draining component");

  klass = GST_OMX_AUDIO_DEC_GET_CLASS (self);

  if (!self->started) {
    GST_DEBUG_OBJECT (self, "Component not started yet");
    return GST_FLOW_OK;
  }
  self->started = FALSE;

  /* Don't send EOS buffer twice, this doesn't work */
  if (self->eos) {
    GST_DEBUG_OBJECT (self, "Component is EOS already");
    return GST_FLOW_OK;
  }

  if ((klass->cdata.hacks & GST_OMX_HACK_NO_EMPTY_EOS_BUFFER)) {
    GST_WARNING_OBJECT (self, "Component does not support empty EOS buffers");
    return GST_FLOW_OK;
  }

  /* Make sure to release the base class stream lock, otherwise
   * _loop() can't call _finish_frame() and we might block forever
   * because no input buffers are released */
  GST_AUDIO_DECODER_STREAM_UNLOCK (self);

  /* Send an EOS buffer to the component and let the base
   * class drop the EOS event. We will send it later when
   * the EOS buffer arrives on the output port. */
  acq_ret = gst_omx_port_acquire_buffer (self->in_port, &buf);
  if (acq_ret != GST_OMX_ACQUIRE_BUFFER_OK) {
    GST_AUDIO_DECODER_STREAM_LOCK (self);
    GST_ERROR_OBJECT (self, "Failed to acquire buffer for draining: %d",
        acq_ret);
    return GST_FLOW_ERROR;
  }

  g_mutex_lock (&self->drain_lock);
  self->draining = TRUE;
  buf->omx_buf->nFilledLen = 0;
  buf->omx_buf->nTimeStamp =
      gst_util_uint64_scale (self->last_upstream_ts, OMX_TICKS_PER_SECOND,
      GST_SECOND);
  buf->omx_buf->nTickCount = 0;
  buf->omx_buf->nFlags |= OMX_BUFFERFLAG_EOS;
  err = gst_omx_port_release_buffer (self->in_port, buf);
  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self, "Failed to drain component: %s (0x%08x)",
        gst_omx_error_to_string (err), err);
    GST_AUDIO_DECODER_STREAM_LOCK (self);
    return GST_FLOW_ERROR;
  }

  GST_DEBUG_OBJECT (self, "Waiting until component is drained");
  while (self->draining)
    g_cond_wait (&self->drain_cond, &self->drain_lock);

  GST_DEBUG_OBJECT (self, "Drained component");
  g_mutex_unlock (&self->drain_lock);
  GST_AUDIO_DECODER_STREAM_LOCK (self);

  self->started = FALSE;

  return GST_FLOW_OK;
}
